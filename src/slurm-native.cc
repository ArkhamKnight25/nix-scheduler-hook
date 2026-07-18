#include "slurm-native.hh"
#include "settings.hh"
#include "sched_util.hh"

#include <nlohmann/json.hpp>
using namespace nlohmann;

#include <thread>
using namespace std::chrono_literals;

#include <nix/store/store-open.hh>
#include <nix/store/store-api.hh>
#include <nix/store/derivations.hh>

SlurmNative::SlurmNative()
{
    /* See sched_util.hh: libslurm dlopens its auth/hash plugins at runtime. */
    promoteLibraryToGlobalScope("libslurm.so");
    slurm_init(ourSettings.slurmConf.get() != "" ? ourSettings.slurmConf.get().c_str() : nullptr);
}

void SlurmNative::submit(
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

    job_desc_msg_t job_desc_msg;
    slurm_init_job_desc_msg(&job_desc_msg);

    auto vars = json::parse(ourSettings.submitEnv.get()).template get<std::vector<std::string>>();
    job_desc_msg.environment = new char*[vars.size()];
    Finally freeVars([&] {
        delete[] job_desc_msg.environment;
    });
    for (int i = 0; i < vars.size(); ++i)
        job_desc_msg.environment[i] = vars[i].data();
    job_desc_msg.env_size = vars.size();

    auto script = genScript(drvPath, jobContext.rootPath, wantedPaths);
    job_desc_msg.script = script.data();

    auto submitDir = ourSettings.submitDir.get().string();
    job_desc_msg.work_dir = submitDir.data();

    job_desc_msg.std_err = jobContext.jobStderr.data();

    /* Input-aware placement: req_nodes is libslurm's comma-separated list of
     * required nodes (slurm.h job_desc_msg_t). Must outlive the submit call. */
    std::string pinnedNodeStr;
    if (pinnedNode) {
        pinnedNodeStr = *pinnedNode;
        job_desc_msg.req_nodes = pinnedNodeStr.data();
    }

    if (drv.env.count("slurmNativeConstraints") == 1) {
        json extraParams = json::parse(drv.env.at("slurmNativeConstraints"));
        for (auto & [key, value] : extraParams.items()) {
            if (key == "cpus") {
                if (value > UINT16_MAX)
                    throw SlurmNativeConstraintError(nix::fmt("constraint %s is too large for datatype", key));
                job_desc_msg.cpus_per_task = static_cast<uint16_t>(value);
            } else if (key == "memPerNode") {
                if (value > UINT64_MAX)
                    throw SlurmNativeConstraintError(nix::fmt("constraint %s is too large for datatype", key));
                job_desc_msg.pn_min_memory = static_cast<uint64_t>(value);
            } else if (key == "memPerCPU") {
                if (value > UINT64_MAX)
                    throw SlurmNativeConstraintError(nix::fmt("constraint %s is too large for datatype", key));
                job_desc_msg.pn_min_memory = static_cast<uint64_t>(value) | MEM_PER_CPU;
            } else {
                throw SlurmNativeConstraintError(nix::fmt("unknown constraint %s", key));
            }
        }
    }

    {
        SignalBlocker blockTerm;
        submit_response_msg_t *resp;
        if (slurm_submit_batch_job(&job_desc_msg, &resp)) {
            slurm_free_submit_response_response_msg(resp);
            throw SlurmNativeError("slurm_submit_batch_job");
        } else if (resp->error_code) {
            auto errorCode = resp->error_code;
            slurm_free_submit_response_response_msg(resp);
            throw SlurmNativeError(slurm_strerror(errorCode));
        }
        nativeJobIds[drvPath] = resp->step_id;
        jobContext.jobId = std::to_string(resp->step_id.job_id);
        jobContext.rootPath = nix::fmt("%s/job-%s-%s.root", ourSettings.slurmStateDir.get(), jobContext.jobId, std::string(drvPath.to_string()));
        jobContext.jobStderr = nix::fmt("%s/job-%s-%s.stderr", ourSettings.slurmStateDir.get(), jobContext.jobId, std::string(drvPath.to_string()));
        slurm_free_submit_response_response_msg(resp);
    }

    bool foundBatchHost = false;
    auto sleepTime = 50ms;
    while (!foundBatchHost) {
        job_info_msg_t *resp = nullptr;
        if (slurm_load_job(&resp, nativeJobIds[drvPath], 0) || resp->record_count != 1) {
            slurm_free_job_info_msg(resp);
            throw SlurmNativeError("slurm_load_job");
        } else if (resp->job_array->batch_host) {
            jobContext.address = resp->job_array->batch_host;
            slurm_free_job_info_msg(resp);
            break;
        } else {
            slurm_free_job_info_msg(resp);
            interruptibleSleep(sleepTime);
            if (sleepTime < 1s) sleepTime *= 2;
        }
    }
}

static bool isLive(job_states state)
{
    return (state == JOB_PENDING || state == JOB_RUNNING);
}

static job_states getJobState(slurm_step_id_t jobId)
{
    slurm_selected_step_t jobs = {nullptr, NO_VAL, NO_VAL, jobId };
    job_state_response_msg_t *resp = nullptr;
    if (slurm_load_job_state(1, &jobs, &resp) || resp->jobs_count != 1) {
        slurm_free_job_state_response_msg(resp);
        throw SlurmNativeError("slurm_load_job_state");
    } else {
        job_states state = static_cast<job_states>(JOB_STATE_BASE & resp->jobs->state);
        slurm_free_job_state_response_msg(resp);
        return state;
    }
}

static uint32_t getJobReturnCode(slurm_step_id_t jobId)
{
    job_info_msg_t *resp = nullptr;
    if (slurm_load_job(&resp, jobId, 0) || resp->record_count != 1) {
        slurm_free_job_info_msg(resp);
        throw SlurmNativeError("slurm_load_job");
    } else {
        uint32_t exit_code = resp->job_array->exit_code;
        slurm_free_job_info_msg(resp);
        return exit_code;
    }
}

int SlurmNative::waitForJobFinish(nix::StorePath drvPath)
{
    auto nativeJobId = nativeJobIds[drvPath];
    auto sleepTime = 50ms;
    while (true) {
        auto state = getJobState(nativeJobId);
        if (!isLive(state)) {
            if (state != JOB_COMPLETE && state != JOB_FAILED) {
                using namespace nix;
                printError("NSH Error: unexpected job state %d", state);
                return -1;
            } else
                return getJobReturnCode(nativeJobId);
        } else {
            interruptibleSleep(sleepTime);
            if (sleepTime < 1s) sleepTime *= 2;
        }
    }
}

SlurmNative::~SlurmNative()
{
    for (auto & [drvPath, nativeJobId] : nativeJobIds) {
        /* An exception escaping a destructor calls std::terminate(); one
           failed query must not abort teardown of the remaining jobs. */
        try {
            if (isLive(getJobState(nativeJobId))) {
                if (slurm_kill_job(nativeJobId, SIGTERM, 0) && isLive(getJobState(nativeJobId))) {
                    using namespace nix;
                    printError("error killing job %" PRIu32 ": %s", nativeJobId.job_id, slurm_strerror(errno));
                }
            }
        } catch (std::exception & e) {
            using namespace nix;
            printError("NSH Error: error during SlurmNative teardown: %s", e.what());
        }
    }

    slurm_fini();
}
