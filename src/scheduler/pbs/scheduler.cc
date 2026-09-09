#include "scheduler.hh"
#include "../../settings.hh"
#include "../utils.hh"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <ext/stdio_filebuf.h>
#include <thread>
using namespace std::chrono_literals;

#include <nlohmann/json.hpp>
using namespace nlohmann;

#include <nix/util/fmt.hh>
#include <nix/store/store-open.hh>
#include <nix/store/store-api.hh>
#include <nix/store/derivations.hh>

#include <pbs_error.h>

/* pbs_errno stays 0 for connection-level failures that only set the system
   errno; report whichever carries the actual cause. */
static std::string lastPbsError(int connHandle = -1)
{
    if (pbs_errno != 0) {
        const char * text = pbse_to_txt(pbs_errno);
        std::string basic = text != nullptr ? text : nix::fmt("PBS error %d", pbs_errno);
        if (connHandle >= 0)
            if (auto msg = pbs_geterrmsg(connHandle); msg != nullptr && *msg != '\0')
                return nix::fmt("%s (%s)", basic, msg);
        return basic;
    }
    if (errno != 0)
        return nix::fmt("%s (last system error, no PBS error was propagated)", strerror(errno));
    return "(no error propagated)";
}

static std::string getJobState(int conn, std::string jobId)
{
    attrl attr = {nullptr, ATTR_state, nullptr, nullptr, SET};
    batch_status *status = pbs_statjob(conn, jobId.data(), &attr, "x");
    if (status == nullptr || status->attribs == nullptr) {
        pbs_statfree(status);
        throw PBSQueryError(nix::fmt("Error querying %s for job %s: %s", ATTR_state, jobId, lastPbsError(conn)));
    }
    std::string value = status->attribs->value;
    pbs_statfree(status);
    return value;
}

static void waitForJobRunning(int conn, std::string jobId)
{
    auto sleepTime = 50ms;
    while (true) {
        auto state = getJobState(conn, jobId);
        if (state == "R")
            return;
        else if (state == "F")
            throw PBSDeletedError(jobId);
        interruptibleSleep(sleepTime);
        if (sleepTime < 1s) sleepTime *= 2;
    }
}

static struct attropl *new_attropl()
{
    return new attropl{nullptr, nullptr, nullptr, nullptr, SET};
}

/* Owned copy for attropl resource/value fields: every string reachable from
   the list must outlive pbs_submit and be freeable by free_attropl_list. */
static char * dupString(const std::string & s)
{
    char * p = new char[s.size() + 1];
    memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
    return p;
}

static void free_attropl_list(struct attropl *at_list)
{
    struct attropl *cur, *tmp;
    for (cur = at_list; cur != NULL; cur = tmp) {
        if (cur->resource != nullptr)
            delete[] cur->resource;
        if (cur->value != nullptr)
            delete[] cur->value;
        tmp = cur->next;
        delete cur;
    }
}

PBS::PBS()
{
    /* See sched_util.hh: libpbs dlopens libauth_munge.so at connect time. */
    promoteLibraryToGlobalScope("libpbs.so.0");
    if (ourSettings.pbsHost.get().empty())
        connHandle = pbs_connect(nullptr);
    else
        connHandle = pbs_connect(nix::fmt("%s:%u", ourSettings.pbsHost.get(), ourSettings.pbsPort.get()).c_str());
    if (connHandle == -1)
        throw PBSConnectionError(nix::fmt("Error connecting to PBS server: %s", lastPbsError()));
}

void PBS::submit(
    nix::StorePath drvPath,
    const nix::BasicDerivation & drv,
    std::string system,
    nix::StringSet requiredFeatures,
    nix::StorePathSet wantedPaths,
    const std::optional<std::string> & pinnedNode)
{
    auto & jobContext = contexts[drvPath];

    auto jobNameStr = nix::fmt("Nix_Build_%s", std::string(drvPath.to_string()));

    // We don't know the jobdir until after the job is running, so use a
    // relative path for the script generation and update it to an absolute
    // path after submission.
    jobContext.rootPath = nix::fmt("%s.root", jobNameStr.data());

    char tmp_template[] = "pbsscrptXXXXXX";
    snprintf(scriptName, sizeof(scriptName), "%s/%s", std::filesystem::temp_directory_path().c_str(), tmp_template);
    int fd = mkstemp(scriptName);
    if (fd == -1)
        throw PBSSubmitError(nix::fmt("Error creating temporary file for PBS script %s", scriptName));
    createdScript = true;
    __gnu_cxx::stdio_filebuf<char> scriptOutBuf(fd, std::ios::out);
    std::ostream scriptOut(&scriptOutBuf);
    scriptOut << genScript(drvPath, jobContext.rootPath, wantedPaths, jobContext.remoteBuilding);
    scriptOut.flush();

    // Attribute chain:
    // k -> N -> (l1/aResBase -> l2 -> l3 -> ...) -> (v1 -> v2 -> v3 -> ...)

    std::string res = "";
    attropl *aResBase = nullptr;
    if (drv.env.count("pbsResources") == 1) {
        json pbsResources = json::parse(drv.env.at("pbsResources"));
        attropl *prev = nullptr;
        for (auto & [key, value] : pbsResources.items()) {
            /* Input-aware placement pins the host via a select resource; a
             * user-supplied node request cannot be merged with it safely. */
            if (pinnedNode && (key == "select" || key == "nodes" || key == "host")) {
                free_attropl_list(aResBase);
                throw PBSSubmitError(nix::fmt(
                    "input-aware selection chose node '%s', but pbsResources requests '%s'; "
                    "remove it from pbsResources or unset candidate-nodes",
                    *pinnedNode, key));
            }
            auto attr = new_attropl();
            attr->name = ATTR_l;
            attr->resource = dupString(key);
            attr->value = dupString(std::string(value));
            if (!aResBase)
                aResBase = attr;
            else if (prev != nullptr)
                prev->next = attr;
            prev = attr;
        }

        auto vars = json::parse(ourSettings.submitEnv.get()).template get<std::vector<std::string>>();
        for (auto & var : vars) {
            auto attr = new_attropl();
            attr->name = ATTR_v;
            attr->resource = nullptr;
            /* Owned copy: `vars` dies at the end of this block, long before
               pbs_submit reads the list (and free_attropl_list deletes value). */
            attr->value = dupString(var);
            attr->op = SET;
            /* pbsResources may be an empty object, leaving prev null. */
            if (!aResBase)
                aResBase = attr;
            else if (prev != nullptr)
                prev->next = attr;
            prev = attr;
        }
    }

    /* Input-aware placement: request the selected host with a select
     * expression (Resource_List.select = 1:host=<node>), prepended to the
     * user's resource list. */
    if (pinnedNode) {
        auto attr = new_attropl();
        attr->name = ATTR_l;
        attr->resource = dupString("select");
        attr->value = dupString(pbsSelectForHost(*pinnedNode));
        attr->next = aResBase;
        aResBase = attr;
    }

    attropl aName = {aResBase != nullptr ? aResBase : nullptr, ATTR_N, nullptr, jobNameStr.data(), SET};
    char kfVal[] = "oe";  // Hush write-strings warning
    attropl aKeepFiles = {&aName, ATTR_k, nullptr, kfVal, SET};

    {
        SignalBlocker blockTerm;
        char *id = pbs_submit(connHandle, &aKeepFiles, scriptName, nullptr, nullptr);
        free_attropl_list(aResBase);
        aName.next = nullptr;
        if (id == nullptr) {
            if (auto err_list = pbs_get_attributes_in_error(connHandle)) {
                auto error = err_list->ecl_attrerr[0];
                throw PBSSubmitError(nix::fmt("Error submitting PBS job: attribute %s is in error: %s", error.ecl_attribute->name, error.ecl_errmsg));
            }
            throw PBSSubmitError(nix::fmt("Error submitting PBS job: %s", lastPbsError(connHandle)));
        }
        jobContext.jobId = id;
    }

    waitForJobRunning(connHandle, jobContext.jobId);

    attrl jobdirAttr = {nullptr, ATTR_jobdir, nullptr, nullptr, SET};
    batch_status *jobdirStatus;
    auto sleepTime = 50ms;
    while (true) {
        jobdirStatus = pbs_statjob(connHandle, jobContext.jobId.data(), &jobdirAttr, nullptr);
        if (jobdirStatus == nullptr) {
            throw PBSQueryError(nix::fmt("Error querying %s for job %s: %s", ATTR_jobdir, jobContext.jobId, lastPbsError(connHandle)));
        } else if (jobdirStatus->attribs == nullptr) {
            pbs_statfree(jobdirStatus);
            interruptibleSleep(sleepTime);
            if (sleepTime < 1s) sleepTime *= 2;
        } else break;
    }
    std::string jobDir = jobdirStatus->attribs->value;
    pbs_statfree(jobdirStatus);

    auto jobIdNum = nix::tokenizeString<nix::Strings>(jobContext.jobId, ".").front();
    jobContext.jobStderr = nix::fmt("%s/%s.e%s", jobDir, jobNameStr, jobIdNum);
    jobContext.rootPath = nix::fmt("%s/%s.root", jobDir, jobNameStr);

    /* The PBS server and execution host are commonly different machines.
     * Input transfer, log streaming, and output retrieval must use the MOM
     * where the job actually runs, not the host running pbs_server. */
    attrl execHostAttr = {nullptr, ATTR_exechost, nullptr, nullptr, SET};
    sleepTime = 50ms;
    batch_status *execHostStatus;
    while (true) {
        execHostStatus = pbs_statjob(connHandle, jobContext.jobId.data(), &execHostAttr, nullptr);
        if (execHostStatus == nullptr) {
            throw PBSQueryError(nix::fmt("Error querying %s for job %s: %s", ATTR_exechost, jobContext.jobId, lastPbsError(connHandle)));
        } else if (execHostStatus->attribs == nullptr) {
            pbs_statfree(execHostStatus);
            interruptibleSleep(sleepTime);
            if (sleepTime < 1s) sleepTime *= 2;
        } else break;
    }
    std::string execHost = execHostStatus->attribs->value;
    pbs_statfree(execHostStatus);

    /* exec_host is a '+'-separated allocation such as
     * "node2/0+node2/1". The batch script runs on its first host. */
    auto hostEnd = execHost.find_first_of("/+");
    jobContext.address = execHost.substr(0, hostEnd);
    if (jobContext.address.empty())
        throw PBSQueryError(nix::fmt("PBS returned an invalid %s value for job %s: '%s'", ATTR_exechost, jobContext.jobId, execHost));
}

int PBS::waitForJobFinish(nix::StorePath drvPath)
{
    auto & jobContext = contexts[drvPath];
    auto sleepTime = 50ms;
    while (true) {
        auto state = getJobState(connHandle, jobContext.jobId);
        if (state == "F") {
            attrl exitAttr = {nullptr, ATTR_exit_status, nullptr, nullptr, SET};
            batch_status *exitStatus = pbs_statjob(connHandle, jobContext.jobId.data(), &exitAttr, "x");
            if (exitStatus == nullptr || exitStatus->attribs == nullptr) {
                pbs_statfree(exitStatus);
                throw PBSQueryError(nix::fmt("Error querying %s for job %s: %s", ATTR_exit_status, jobContext.jobId, lastPbsError(connHandle)));
            }
            auto value = std::atoi(exitStatus->attribs->value);
            pbs_statfree(exitStatus);
            /* The context must survive until ~Scheduler: erasing it here
             * would skip the shared teardown (temp-file removal, remote GC)
             * on the happy path. ~PBS below handles finished jobs. */
            return value;
        }
        interruptibleSleep(sleepTime);
        if (sleepTime < 1s) sleepTime *= 2;
    }
}

PBS::~PBS()
{
    if (createdScript)
        unlink(scriptName);

    for (auto & [drvPath, jobContext] : contexts) {
        if (jobContext.jobId == "")
            continue;
        /* Only delete jobs that are still queued/running; contexts now
         * outlive job completion (see waitForJobFinish) so finished jobs
         * appear here too and must not be deleted from history. */
        try {
            if (getJobState(connHandle, jobContext.jobId) != "F")
                pbs_deljob(connHandle, jobContext.jobId.data(), nullptr);
        } catch (std::exception & e) {
            /* State query failed; attempt the delete anyway (an unknown or
             * finished job makes it a harmless no-op error). */
            pbs_deljob(connHandle, jobContext.jobId.data(), nullptr);
        }
    }

    pbs_disconnect(connHandle);
}
