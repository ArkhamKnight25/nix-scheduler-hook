#include "pbs.hh"
#include "settings.hh"
#include "sched_util.hh"

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

static std::string getJobState(int conn, std::string jobId)
{
    attrl attr = {nullptr, ATTR_state, nullptr, nullptr, SET};
    batch_status *status = pbs_statjob(conn, jobId.data(), &attr, "x");
    if (status == nullptr || status->attribs == nullptr)
        throw PBSQueryError(nix::fmt("Error querying %s for job %s: %d", ATTR_state, jobId, pbs_errno));
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
        std::this_thread::sleep_for(sleepTime);
        if (sleepTime < 1s) sleepTime *= 2;
    }
}

static struct attropl *new_attropl()
{
    return new attropl{nullptr, nullptr, nullptr, nullptr, SET};
}

static void free_attropl_list(struct attropl *at_list)
{
    struct attropl *cur, *tmp;
    for (cur = at_list; cur != NULL; cur = tmp) {
        if (cur->resource != nullptr)
            delete cur->resource;
        if (cur->value != nullptr)
            delete cur->value;
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
        throw PBSConnectionError(nix::fmt("Error connecting to PBS server: %d", pbs_errno));
}

void PBS::submit(
    nix::StorePath drvPath,
    const nix::BasicDerivation & drv,
    std::string system,
    nix::StringSet requiredFeatures,
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
    scriptOut << genScript(drvPath, jobContext.rootPath);
    scriptOut.flush();

    // Attribute chain:
    // v -> k -> N -> (l1/aResBase -> l2 -> l3 -> ...)

    std::string res = "";
    attropl *aResBase = nullptr;
    if (drv.env.count("pbsResources") == 1) {
        json pbsResources = json::parse(drv.env.at("pbsResources"));
        attropl *prev = nullptr;
        for (auto & [key, value] : pbsResources.items()) {
            /* Input-aware placement pins the host via a select resource; a
             * user-supplied node request cannot be merged with it safely. */
            if (pinnedNode && (key == "select" || key == "nodes" || key == "host"))
                throw PBSSubmitError(nix::fmt(
                    "input-aware selection chose node '%s', but pbsResources requests '%s'; "
                    "remove it from pbsResources or unset candidate-nodes",
                    *pinnedNode, key));
            auto attr = new_attropl();
            attr->name = ATTR_l;
            attr->resource = new char[key.size() + 1];
            strncpy(attr->resource, key.data(), key.size());
            attr->resource[key.size()] = '\0';
            std::string strValue(value);
            attr->value = new char[strValue.size() + 1];
            strncpy(attr->value, strValue.data(), strValue.size());
            attr->value[strValue.size()] = '\0';
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
        auto dupString = [](const std::string & s) {
            char * p = new char[s.size() + 1];
            memcpy(p, s.data(), s.size());
            p[s.size()] = '\0';
            return p;
        };
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
    char pathVar[] = PATH_VAR;
    attropl aVariableList = {&aKeepFiles, ATTR_v, nullptr, pathVar, SET};

    blockSignals();
    char *id = pbs_submit(connHandle, &aVariableList, scriptName, nullptr, nullptr);
    free_attropl_list(aResBase);
    aName.next = nullptr;
    if (id == nullptr) {
        if (auto err_list = pbs_get_attributes_in_error(connHandle)) {
            auto error = err_list->ecl_attrerr[0];
            throw PBSSubmitError(nix::fmt("Error submitting PBS job: attribute %s is in error: %s", error.ecl_attribute->name, error.ecl_errmsg));
        }
        throw PBSSubmitError(nix::fmt("Error submitting PBS job: %s", pbs_geterrmsg(connHandle)));
    }
    jobContext.jobId = id;
    unblockSignals();

    waitForJobRunning(connHandle, jobContext.jobId);

    attrl jobdirAttr = {nullptr, ATTR_jobdir, nullptr, nullptr, SET};
    batch_status *jobdirStatus;
    auto sleepTime = 50ms;
    while (true) {
        jobdirStatus = pbs_statjob(connHandle, jobContext.jobId.data(), &jobdirAttr, nullptr);
        if (jobdirStatus == nullptr) {
            throw PBSQueryError(nix::fmt("Error querying %s for job %s: %d", ATTR_jobdir, jobContext.jobId, pbs_errno));
        } else if (jobdirStatus->attribs == nullptr) {
            pbs_statfree(jobdirStatus);
            std::this_thread::sleep_for(sleepTime);
            if (sleepTime < 1s) sleepTime *= 2;
        } else break;
    }
    std::string jobDir = jobdirStatus->attribs->value;
    pbs_statfree(jobdirStatus);

    auto jobIdNum = nix::tokenizeString<nix::Strings>(jobContext.jobId, ".").front();
    jobContext.jobStderr = nix::fmt("%s/%s.e%s", jobDir, jobNameStr, jobIdNum);
    jobContext.rootPath = nix::fmt("%s/%s.root", jobDir, jobNameStr);

    attrl serverAttr = {nullptr, ATTR_server, nullptr, nullptr, SET};
    sleepTime = 50ms;
    batch_status *serverStatus;
    while (true) {
        serverStatus = pbs_statjob(connHandle, jobContext.jobId.data(), &serverAttr, nullptr);
        if (serverStatus == nullptr) {
            throw PBSQueryError(nix::fmt("Error querying %s for job %s: %d", ATTR_server, jobContext.jobId, pbs_errno));
        } else if (serverStatus->attribs == nullptr) {
            pbs_statfree(serverStatus);
            std::this_thread::sleep_for(sleepTime);
            if (sleepTime < 1s) sleepTime *= 2;
        } else break;
    }
    jobContext.address = serverStatus->attribs->value;
    pbs_statfree(serverStatus);
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
            if (exitStatus == nullptr || exitStatus->attribs == nullptr)
                throw PBSQueryError(nix::fmt("Error querying %s for job %s: %d", ATTR_exit_status, jobContext.jobId, pbs_errno));
            auto value = std::atoi(exitStatus->attribs->value);
            pbs_statfree(exitStatus);
            /* The context must survive until ~Scheduler: erasing it here
             * would skip the shared teardown (temp-file removal, remote GC)
             * on the happy path. ~PBS below handles finished jobs. */
            return value;
        }
        std::this_thread::sleep_for(sleepTime);
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
