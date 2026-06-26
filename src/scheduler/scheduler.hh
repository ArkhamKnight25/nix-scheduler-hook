#pragma once

#include <string>
#include <utility>
#include <iostream>
#include <ext/stdio_filebuf.h>
#include <array>

#include <nix/store/path.hh>
#include <nix/store/store-open.hh>
#include <nix/store/ssh-store.hh>
#include <nix/store/ssh.hh>
#include <nix/store/derivations.hh>
#include <nix/util/types.hh>
#include <nix/util/logging.hh>
#include <nix/util/signals.hh>

#include "../settings.hh"
#include "../node-selection.hh"

class Scheduler
{
public:
    struct JobContext {
        std::string jobId;
        std::string address;
        std::string storeUri;
        std::string jobStderr;
        std::shared_ptr<nix::SSHMaster::Connection> cmdConn;
        std::shared_ptr<nix::SSHMaster> sshMaster;
        std::string rootPath;
        bool cmdOutInit = false;
        std::shared_ptr<__gnu_cxx::stdio_filebuf<char>> cmdOutBuf;
    };

    Scheduler() {}

    /* Shared teardown: remove the job's temporary files on the node and
     * optionally GC its store. Failures are logged per step so one broken
     * job/file cannot skip the cleanup of the others. */
    virtual ~Scheduler()
    {
        /* Teardown must not be aborted by a pending interrupt: waits are
           done with allowInterrupts=false (checkInterrupt inside Pid::wait
           would throw here on a non-unwind path), and the try/catch is
           per-step so one failure doesn't skip the remaining cleanup. */
        for (auto & [drvPath, jobContext] : contexts) {
            if (!jobContext.sshMaster)
                continue;
            for (auto & file : {jobContext.rootPath, jobContext.jobStderr}) {
                if (file.empty())
                    continue;
                try {
                    nix::Strings rmCmd = {"rm", "-f", file};
                    auto cmd = jobContext.sshMaster->startCommand(std::move(rmCmd));
                    cmd->sshPid.wait(false);
                } catch (std::exception & e) {
                    using namespace nix;
                    printError("NSH Error: error removing '%s' during Scheduler teardown: %s", file, e.what());
                }
            }
            /* GC is routine maintenance; skip it when we are being
               torn down under Nix's 20s SIGKILL deadline. */
            if (ourSettings.collectGarbage.get() && !nix::getInterrupted()) {
                try {
                    auto binDir = ourSettings.remoteNixBinDir.get();
                    nix::Strings gcCmd = {
                        (binDir != "" ? binDir + "/" : "") + "nix-store",
                        "--gc",
                        "--store",
                        ourSettings.remoteStore.get()
                    };
                    auto cmd = jobContext.sshMaster->startCommand(std::move(gcCmd));
                    if (int rc = cmd->sshPid.wait(false)) {
                        using namespace nix;
                        printError("NSH Error: garbage collection failed: %d", rc);
                    }
                } catch (std::exception & e) {
                    using namespace nix;
                    printError("NSH Error: error during Scheduler GC teardown: %s", e.what());
                }
            }
        }
    }

    struct StartBuildNotCalled : public std::runtime_error
    {
        explicit StartBuildNotCalled() : std::runtime_error("startBuild() has not yet been called.") {}
    };

    /* Submits a derivation for building and establishes an ssh connection to
     * the scheduled host.
     *
     * `inputs` is the build's required input closure (from the Phase-2
     * Builder overloads). When candidate-nodes is configured, it drives
     * input-aware placement: the job is pinned to the candidate whose store
     * already holds the most input paths. Hook mode passes no inputs (the
     * hook protocol only reveals them after the job is accepted), so it
     * keeps the scheduler's normal placement.
     * @return Address of the node assigned to the job. */
    std::string startBuild(
        nix::StorePath drvPath,
        const nix::BasicDerivation & drv,
        std::string system,
        nix::StringSet requiredFeatures,
        nix::StorePathSet wantedPaths,
        const nix::StorePathSet & inputs = {})
    {
        contexts[drvPath] = JobContext();
        auto & jobContext = contexts[drvPath];
        auto pinnedNode = selectNodeForInputs(inputs);
        submit(drvPath, drv, system, requiredFeatures, wantedPaths, pinnedNode);
        if (ourSettings.sshUser.get() != "")
            jobContext.storeUri = nix::fmt("ssh-ng://%s@%s:%d", ourSettings.sshUser.get(), jobContext.address, ourSettings.sshPort.get());
        else
            jobContext.storeUri = nix::fmt("ssh-ng://%s:%d", jobContext.address, ourSettings.sshPort.get());
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, nix::fmt("connecting to '%s'", jobContext.storeUri));
        auto baseStoreConfig = nix::resolveStoreConfig(nix::StoreReference::parse(jobContext.storeUri));
        auto sshStoreConfig = std::dynamic_pointer_cast<nix::SSHStoreConfig>(baseStoreConfig.get_ptr());
        jobContext.sshMaster = std::make_shared<nix::SSHMaster>(sshStoreConfig->createSSHMaster(false));

        submitCalled.insert(drvPath);
        return jobContext.address;
    }

    /* Submits a derivation for building. `drv` is the in-memory derivation
     * supplied by the Phase-2 Builder overload, so backends read `drv.env`
     * directly rather than re-opening the store and reading the .drv.
     * `pinnedNode`, when set, is the input-aware placement decision: the
     * backend must request exactly that node from its scheduler (and fail
     * loudly if user-supplied submission parameters conflict). */
    virtual void submit(
        nix::StorePath drvPath,
        const nix::BasicDerivation & drv,
        std::string system,
        nix::StringSet requiredFeatures,
        nix::StorePathSet wantedPaths,
        const std::optional<std::string> & pinnedNode) = 0;

    /* Waits for the submitted job to finish.
     * @return Exit code of job, or -1 if abnormal termination (e.g. cancelled). */
    virtual int waitForJobFinish(nix::StorePath drvPath) = 0;

    std::string getJobId(nix::StorePath drvPath)
    {
        return contexts[drvPath].jobId;
    }

    std::shared_ptr<std::istream> getStderrStream(nix::StorePath drvPath)
    {
        if (!submitCalled.contains(drvPath)) throw StartBuildNotCalled();
        auto & jobContext = contexts[drvPath];
        if (!jobContext.cmdOutInit) {
            /* -F, not -f: the job's stderr file may not exist yet when we
             * attach (e.g. PBS creates the spool file only once the job
             * starts on the MOM); plain -f exits immediately on a missing
             * file, silently killing log streaming for the whole build. */
            nix::Strings tailCmd = {"tail", "-F", jobContext.jobStderr};
            jobContext.cmdConn = jobContext.sshMaster->startCommand(std::move(tailCmd));
            auto cmdOutFd = jobContext.cmdConn->out.release();
            int flags = fcntl(cmdOutFd, F_GETFL, 0);
            fcntl(cmdOutFd, F_SETFL, flags | O_NONBLOCK);
            jobContext.cmdOutBuf = std::make_shared<__gnu_cxx::stdio_filebuf<char>>(cmdOutFd, std::ios::in);
            jobContext.cmdOutInit = true;
        }
        return std::make_shared<std::istream>(jobContext.cmdOutBuf.get());
    }

protected:
    std::map<nix::StorePath, JobContext> contexts;
    std::set<nix::StorePath> submitCalled;
};
