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

#include <nlohmann/json.hpp>
using namespace nlohmann;

#include <restclient-cpp/connection.h>
#include <restclient-cpp/restclient.h>

#include <nix/store/store-open.hh>
#include <nix/store/store-api.hh>
#include <nix/store/derivations.hh>

constexpr std::string_view SLURM_API_VERSION = "v0.0.43";

static std::shared_ptr<RestClient::Connection> getConn()
{
    static bool init = false;
    static std::shared_ptr<RestClient::Connection> conn;
    if (!init) {
        RestClient::init();
        conn = std::make_shared<RestClient::Connection>(
            nix::fmt("http://%s:%d", ourSettings.slurmApiHost.get(), ourSettings.slurmApiPort.get()));
        RestClient::HeaderFields headers;
        headers["X-SLURM-USER-TOKEN"] = ourSettings.slurmJwtToken.get();
        headers["Content-Type"] = "application/json";
        conn->SetHeaders(headers);
        /* Bound every REST call: a wedged slurmrestd must not be able to
           pin us past Nix's 20s SIGTERM-to-SIGKILL teardown window. */
        conn->SetTimeout(10);
        init = true;
    }
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

void Slurm::submit(nix::StorePath drvPath, std::string system, nix::StringSet requiredFeatures)
{
    auto & jobContext = contexts[drvPath];

    jobContext.rootPath = nix::fmt("%s/job-$SLURM_JOB_ID-%s.root", ourSettings.slurmStateDir.get(), std::string(drvPath.to_string()));
    jobContext.jobStderr = nix::fmt("%s/job-%%j-%s.stderr", ourSettings.slurmStateDir.get(), std::string(drvPath.to_string()));

    json req = {
        {"job", {
            {"name", "Nix Build - " + std::string(drvPath.to_string())},
            {"current_working_directory", ourSettings.submitDir.get().string()},
            {"environment", json::parse(ourSettings.submitEnv.get())},
            {"script", genScript(drvPath, jobContext.rootPath)},
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

    auto store = nix::openStore();
    auto drv = store->readDerivation(drvPath);
    if (drv.env.count("extraSlurmParams") == 1) {
        json extraParams = json::parse(drv.env["extraSlurmParams"]);
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

    auto conn = getConn();
    {
        SignalBlocker blockTerm;
        RestClient::Response r = conn->post("/slurm/" + SLURM_API_VERSION + "/job/submit", req.dump());
        if (r.body == "Authentication failure") {
            throw SlurmAuthenticationError(r.body);
        }
        json response = parseResponse(r);
        if (response["errors"].size() > 0) {
            throw SlurmAPIError(nix::fmt("%s (%d): %s",
                response["errors"][0]["description"],
                response["errors"][0]["error_number"],
                response["errors"][0]["error"]));
        }
        int jobIdInt = response["job_id"];
        jobContext.jobId = std::to_string(jobIdInt);
        jobContext.rootPath = nix::fmt("%s/job-%s-%s.root", ourSettings.slurmStateDir.get(), jobContext.jobId, std::string(drvPath.to_string()));
        jobContext.jobStderr = nix::fmt("%s/job-%s-%s.stderr", ourSettings.slurmStateDir.get(), jobContext.jobId, std::string(drvPath.to_string()));
    }

    bool foundBatchHost = false;
    auto sleepTime = 50ms;
    std::string nodeName;
    while (!foundBatchHost) {
        RestClient::Response qr = conn->get("/slurm/" + SLURM_API_VERSION + "/job/" + jobContext.jobId);
        json qresp = parseResponse(qr);
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

    RestClient::Response qr = conn->get("/slurm/" + SLURM_API_VERSION + "/node/" + nodeName);
    json qresp = parseResponse(qr);
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

static bool isLive(std::string state)
{
    return (state == "PENDING" || state == "RUNNING");
}

static std::string getJobState(std::string jobId)
{
    auto sleepTime = 50ms;
    while (true) {
        RestClient::Response qr = getConn()->get("/slurmdb/" + SLURM_API_VERSION + "/job/" + jobId);
        json qresp = parseResponse(qr);
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

static uint32_t getJobReturnCode(std::string jobId)
{
    while (true) {
        RestClient::Response qr = getConn()->get("/slurmdb/" + SLURM_API_VERSION + "/job/" + jobId);
        json qresp = parseResponse(qr);
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
            if (jobContext.jobId != "" && isLive(getJobState(jobContext.jobId))) {
                getConn()->del("/slurm/" + SLURM_API_VERSION + "/job/" + jobContext.jobId);
            }
        } catch (std::exception & e) {
            using namespace nix;
            printError("NSH Error: error during Slurm teardown: %s", e.what());
        }
    }
}
