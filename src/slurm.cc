#include "slurm.hh"
#include "settings.hh"
#include "sched_util.hh"

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <thread>
using namespace std::chrono_literals;
#include <atomic>
#include <fcntl.h>
#include <chrono>

#include <nlohmann/json.hpp>
using namespace nlohmann;

#include <restclient-cpp/connection.h>
#include <restclient-cpp/restclient.h>

#include <nix/store/store-open.hh>
#include <nix/store/store-api.hh>
#include <nix/store/derivations.hh>
#include <nix/store/globals.hh>
#include <nix/store/pathlocks.hh>

constexpr std::string_view SLURM_API_VERSION = "v0.0.43";

/* Requests made with abortOnInterrupt=true are aborted by libcurl's
   progress callback once SIGTERM has flagged the interrupt: curl retries
   on EINTR, and Nix follows SIGTERM with SIGKILL after a few seconds, so
   an in-flight request that kept waiting would prevent the stack from
   unwinding (and the job from being cancelled) in time. It must be false
   for requests that have to complete once issued: the submit POST (or a
   job could be accepted server-side with nobody recording its id) and
   teardown requests (which run with the interrupt flag already set). */
static std::shared_ptr<RestClient::Connection> getConn(bool abortOnInterrupt = true)
{
    static bool init = false;
    if (!init) {
        RestClient::init();
        init = true;
    }
    auto conn = std::make_shared<RestClient::Connection>(
        nix::fmt("http://%s:%d", ourSettings.slurmApiHost.get(), ourSettings.slurmApiPort.get()));
    RestClient::HeaderFields headers;
    headers["X-SLURM-USER-TOKEN"] = ourSettings.slurmJwtToken.get();
    headers["Content-Type"] = "application/json";
    conn->SetHeaders(headers);
    conn->SetTimeout(ourSettings.slurmApiTimeout.get());
    if (abortOnInterrupt)
        conn->SetFileProgressCallback([](void *, double, double, double, double) -> int {
            return nix::getInterrupted() ? 1 : 0;
        });
    return conn;
}

/* slurmrestd failure modes (auth errors, proxies, empty replies) produce
   plain-text bodies; surface them instead of a bare json parse error. */
static json parseResponse(const RestClient::Response & r)
{
    try {
        return json::parse(r.body);
    } catch (json::parse_error &) {
        throw SlurmAPIError(nix::fmt("non-JSON response from slurmrestd (HTTP %d): %s", r.code, nix::chomp(r.body)));
    }
}

/* Polling GET whose interruption is safe: checkInterrupt() turns a
   request aborted by the progress callback (or a SIGTERM that arrived
   between requests) into nix::Interrupted, so the stack unwinds and the
   destructors cancel the job instead of reporting a spurious API error. */
static json apiGet(const std::string & path)
{
    nix::checkInterrupt();
    auto r = getConn()->get(path);
    nix::checkInterrupt();
    return parseResponse(r);
}

static bool isLive(std::string state)
{
    return (state == "PENDING" || state == "RUNNING");
}

static std::string getJobState(std::string jobId)
{
    if (ourSettings.slurmBatchStateUpdate.get()) {
        auto sleepTime = 50ms;
        while(true) {
            nix::AutoCloseFD stateFile = nix::openLockFile(std::filesystem::path{nix::settings.nixStateDir} / "nsh-job-state.json", true);
            nix::lockFile(stateFile.get(), nix::LockType::ltWrite, true);
            Finally unlock([&]{
                nix::lockFile(stateFile.get(), nix::LockType::ltNone, false);
            });
            auto s = nix::readFile(stateFile.get());
            if (s.empty())
                s = "{\"last_updated\": 0, \"jobs\": {}}";
            json cachedState = json::parse(s);
            long currentTime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            if (currentTime >= cachedState["last_updated"].get<long>() + 1) {
                json qresp = apiGet("/slurm/" + std::string(SLURM_API_VERSION) + "/jobs/state/");
                if (qresp["errors"].size() > 0) {
                    throw SlurmAPIError(nix::fmt("%s (%d): %s",
                        qresp["errors"][0]["description"],
                        qresp["errors"][0]["error_number"],
                        qresp["errors"][0]["error"]));
                }
                json currentState = json::parse(nix::fmt("{\"last_updated\": %d, \"jobs\": {}}", currentTime));
                for (auto & job : qresp["jobs"]) {
                    /* job_id is a string here, decorated for array/het jobs
                       ("123_5", "123_[0-99]", "123+0"); nsh jobs are plain. */
                    currentState["jobs"][job["job_id"].get<std::string>()] = job["state"][0];
                }
                if (currentState["jobs"].contains(jobId)) {
                    std::string state = currentState["jobs"][jobId];
                    lseek(stateFile.get(), 0, SEEK_SET);
                    if (ftruncate(stateFile.get(), 0) == -1)
                        throw nix::SysError("truncating nsh-job-state.json");
                    nix::writeFile(stateFile.get(), currentState.dump());
                    return state;
                } else {
                    nix::lockFile(stateFile.get(), nix::LockType::ltNone, false);
                    interruptibleSleep(sleepTime);
                    if (sleepTime < 2s) sleepTime *= 2;
                }
            } else {
                if (cachedState["jobs"].contains(jobId)) {
                    return cachedState["jobs"][jobId];
                } else {
                    nix::lockFile(stateFile.get(), nix::LockType::ltNone, false);
                    interruptibleSleep(sleepTime);
                }
            }
        }
    } else {
        auto sleepTime = 50ms;
        while (true) {
            json qresp = apiGet("/slurmdb/" + std::string(SLURM_API_VERSION) + "/job/" + jobId);
            if (qresp["errors"].size() > 0) {
                throw SlurmAPIError(nix::fmt("%s (%d): %s",
                    qresp["errors"][0]["description"],
                    qresp["errors"][0]["error_number"],
                    qresp["errors"][0]["error"]));
            } else if (qresp["jobs"].size() == 1) {
                return qresp["jobs"][0]["state"]["current"][0];
            } else {
                interruptibleSleep(sleepTime);
                if (sleepTime < 2s) sleepTime *= 2;
            }
        }
    }
}

static void waitForJobRunning(std::string jobId)
{
    auto sleepTime = 50ms;
    while (true) {
        auto state = getJobState(jobId);
        if (state == "RUNNING")
            return;
        else if (!isLive(state)) {
            if (state != "COMPLETED" && state != "FAILED") {
                throw nix::Error("NSH Error: unexpected job state %s", state);
            } else
                return;
        } else {
            interruptibleSleep(sleepTime);
            if (sleepTime < 4s) sleepTime *= 2;
        }
    }
}

void Slurm::submit(
    nix::StorePath drvPath,
    const nix::BasicDerivation & drv,
    std::string system,
    nix::StringSet requiredFeatures,
    nix::StorePathSet wantedPaths,
    const std::optional<std::string> & pinnedNode)
{
    auto & jobContext = contexts[drvPath];

    jobContext.rootPath = nix::fmt("%s/job-$SLURM_JOB_ID-%s.root", ourSettings.slurmStateDir.get(), std::string(drvPath.to_string()));
    jobContext.jobStderr = nix::fmt("%s/job-%%j-%s.stderr", ourSettings.slurmStateDir.get(), std::string(drvPath.to_string()));

    json req = {
        {"job", {
            {"name", "Nix Build - " + std::string(drvPath.to_string())},
            {"current_working_directory", ourSettings.submitDir.get().string()},
            {"environment", json::parse(ourSettings.submitEnv.get())},
            {"script", genScript(drvPath, jobContext.rootPath, wantedPaths)},
            {"standard_error", jobContext.jobStderr},
        }}
    };

    // Perform a basic merge of two JSON objects, top-level arrays are
    // concatenated, objects are recursively merged, all other values are
    // overwritten except constraints which are specially merged.
    auto update = [](auto & job, auto & extraParams) {
        for (auto & [key, value] : extraParams.items()) {
            if (job.contains(key)) {
                if (job[key].is_array())
                    job[key] += value;
                else if (job[key].is_object())
                    job[key].update(value, true);
                else if (key == "constraints" && job[key].template get<std::string>() != "")
                    job[key] = nix::fmt("(%s)&(%s)", job[key].template get<std::string>(), value.template get<std::string>());
                else
                    job[key] = value;
            } else
                job[key] = value;
        }
    };

    if (ourSettings.slurmExtraJobSubmissionParams.get() != "") {
        json extraParams = json::parse(ourSettings.slurmExtraJobSubmissionParams.get());
        if (!extraParams.is_object())
            throw nix::Error("invalid format for %s, expected a dictionary", ourSettings.slurmExtraJobSubmissionParams.name);
        update(req["job"], extraParams);
    }

    if (drv.env.count("extraSlurmParams") == 1) {
        json extraParams = json::parse(drv.env.at("extraSlurmParams"));
        if (!extraParams.is_object())
            throw nix::Error("invalid format for extraSlurmParams, expected a dictionary");
        update(req["job"], extraParams);
    }

    if (ourSettings.slurmSystemParams.get() != "") {
        json systemParams = json::parse(ourSettings.slurmSystemParams.get());
        if (!systemParams.is_object())
            throw nix::Error("invalid format for %s, expected a dictionary", ourSettings.slurmSystemParams.name);
        if (systemParams.contains(system)) {
            json extraParams = systemParams[system];
            if (!extraParams.is_object())
                throw nix::Error("invalid format for system key %s in %s, expected a dictionary", system, ourSettings.slurmSystemParams.name);
            update(req["job"], extraParams);
        }
    }

    if (ourSettings.slurmFeatureParams.get() != "") {
        json featureParams = json::parse(ourSettings.slurmFeatureParams.get());
        if (!featureParams.is_object())
            throw nix::Error("invalid format for %s, expected a dictionary", ourSettings.slurmFeatureParams.name);
        for (auto & feature : requiredFeatures) {
            if (featureParams.contains(feature)) {
                json extraParams = featureParams[feature];
                if (!extraParams.is_object())
                    throw nix::Error("invalid format for feature key %s in %s, expected a dictionary", feature, ourSettings.slurmFeatureParams.name);
                update(req["job"], extraParams);
            }
        }
    }

    /* Input-aware placement: pin the job to the selected node. Applied after
     * all user-supplied merges so a conflicting user value cannot silently
     * override (or be overridden by) the selection. `required_nodes` is the
     * v0.0.43 data_parser field for job_desc req_nodes. */
    if (pinnedNode) {
        if (req["job"].contains("required_nodes"))
            throw nix::Error(
                "nix-scheduler-hook: input-aware selection chose node '%s', but 'required_nodes' is already set "
                "by extra submission parameters; remove it from the extra params or unset candidate-nodes",
                *pinnedNode);
        req["job"]["required_nodes"] = json::array({*pinnedNode});
    }

    while (true) {
        SignalBlocker blockTerm;
        RestClient::Response r = getConn(false)->post("/slurm/" + SLURM_API_VERSION + "/job/submit", req.dump());
        if (r.body == "Authentication failure") {
            throw SlurmAuthenticationError(r.body);
        }
        json response = parseResponse(r);
        if (response["errors"].size() > 0) {
            if (response["errors"][0]["error_number"] == 11) { // resource temporarily unavailable (e.g. max jobs reached)
                interruptibleSleep(5s);
                continue;
            }
            throw SlurmAPIError(nix::fmt("%s (%d): %s",
                response["errors"][0]["description"],
                response["errors"][0]["error_number"],
                response["errors"][0]["error"]));
        }
        int jobIdInt = response["job_id"];
        jobContext.jobId = std::to_string(jobIdInt);
        jobContext.rootPath = nix::fmt("%s/job-%s-%s.root", ourSettings.slurmStateDir.get(), jobContext.jobId, std::string(drvPath.to_string()));
        jobContext.jobStderr = nix::fmt("%s/job-%s-%s.stderr", ourSettings.slurmStateDir.get(), jobContext.jobId, std::string(drvPath.to_string()));
        break;
    }

    waitForJobRunning(jobContext.jobId);

    bool foundBatchHost = false;
    auto sleepTime = 50ms;
    std::string nodeName;
    while (!foundBatchHost) {
        json qresp = apiGet("/slurm/" + std::string(SLURM_API_VERSION) + "/job/" + jobContext.jobId);
        if (qresp["errors"].size() > 0) {
            throw SlurmAPIError(nix::fmt("%s (%d): %s",
                qresp["errors"][0]["description"],
                qresp["errors"][0]["error_number"],
                qresp["errors"][0]["error"]));
        } else if (
            qresp["jobs"].size() == 1 &&
            qresp["jobs"][0].contains("batch_host") &&
            qresp["jobs"][0]["batch_host"] != ""
        ) {
            nodeName = qresp["jobs"][0]["batch_host"].template get<std::string>();
            foundBatchHost = true;
        } else {
            interruptibleSleep(sleepTime);
            if (sleepTime < 1s) sleepTime *= 2;
        }
    }

    json qresp = apiGet("/slurm/" + std::string(SLURM_API_VERSION) + "/node/" + nodeName);
    if (qresp["errors"].size() > 0) {
        throw SlurmAPIError(nix::fmt("%s (%d): %s",
            qresp["errors"][0]["description"],
            qresp["errors"][0]["error_number"],
            qresp["errors"][0]["error"]));
    } else if (qresp["nodes"].size() == 1)
        jobContext.address = qresp["nodes"][0]["address"].template get<std::string>();
    else if (qresp["nodes"].size() > 1)
        throw SlurmAPIError("too many matching nodes returned in query");
    else
        throw SlurmAPIError("no matching nodes returned in query");
}

static uint32_t getJobReturnCode(std::string jobId)
{
    while (true) {
        json qresp = apiGet("/slurmdb/" + std::string(SLURM_API_VERSION) + "/job/" + jobId);
        if (qresp["errors"].size() > 0) {
            throw SlurmAPIError(nix::fmt("%s (%d): %s",
                qresp["errors"][0]["description"],
                qresp["errors"][0]["error_number"],
                qresp["errors"][0]["error"]));
        } else if (qresp["jobs"].size() == 1 && qresp["jobs"][0]["exit_code"]["return_code"]["set"]) {
            return qresp["jobs"][0]["exit_code"]["return_code"]["number"];
        } else {
            interruptibleSleep(50ms);
        }
    }
}

Slurm::Slurm()
{
    if (ourSettings.slurmStateDir.get() == "")
        throw SlurmConfigError("slurm-state-dir setting not configured");
}

int Slurm::waitForJobFinish(nix::StorePath drvPath)
{
    auto jobId = contexts[drvPath].jobId;
    auto sleepTime = 50ms;
    while (true) {
        auto state = getJobState(jobId);
        if (!isLive(state)) {
            if (state != "COMPLETED" && state != "FAILED") {
                using namespace nix;
                printError("NSH Error: unexpected job state %s", state);
                return -1;
            } else
                return getJobReturnCode(jobId);
        } else {
            interruptibleSleep(sleepTime);
            if (sleepTime < 4s) sleepTime *= 2;
        }
    }
}

Slurm::~Slurm()
{
    for (auto & [drvPath, jobContext] : contexts) {
        try {
            if (jobContext.jobId != "" &&  isLive(getJobState(jobContext.jobId))) {
                auto sleepTime = 50ms;
                while (true) {
                    auto resp = getConn(false)->del("/slurm/" + SLURM_API_VERSION + "/job/" + jobContext.jobId);
                    if (resp.code == 200)
                        break;
                    else {
                        std::this_thread::sleep_for(sleepTime);
                        if (sleepTime < 400ms) sleepTime *= 2;
                    }
                }
            }
        } catch (std::exception & e) {
            using namespace nix;
            printError("NSH Error: error during Slurm teardown: %s", e.what());
        }
    }
}
