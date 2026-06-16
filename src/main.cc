#include <expected>
#include <format>
#include <iostream>
#include <nix/store/store-api.hh>
#include <optional>
#include <thread>
using namespace std::chrono_literals;
#include <memory>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <boost/algorithm/string/join.hpp>

#include <nix/main/shared.hh>
#include <nix/main/plugin.hh>
#include <nix/util/fmt.hh>
#include <nix/store/path.hh>
#include <nix/store/store-open.hh>
#include <nix/store/build-result.hh>
#include <nix/store/build-store.hh>
#include <nix/store/ssh-store.hh>
#include <nix/store/globals.hh>
#include <nix/store/pathlocks.hh>
#include <nix/store/store-dir-config.hh>
#include <nix/store/ssh.hh>
#include <nix/store/local-store.hh>
#include <nix/util/types.hh>
#include <nix/util/serialise.hh>
#include <nix/util/logging.hh>
#include <nix/util/file-descriptor.hh>
#include <nix/util/error.hh>
#include <nix/util/util.hh>
#include <nix/util/hash.hh>
#include <nix/util/signals.hh>
#include <nix/util/signals-impl.hh>
#include <nix/util/finally.hh>
#include <nix/util/processes.hh>
#include <nix/util/environment-variables.hh>
#include <nix/util/config-global.hh>

#include "settings.hh"
#include "sched_util.hh"
#include "build_request.hh"
#include "build_remote_process.hh"
#include "slurm.hh"
#include "pbs.hh"
#include "slurm-native.hh"
#include "logging.hh"

static void handleAlarm(int sig) {}

static std::filesystem::path currentLoad;

static std::string escapeUri(std::string uri)
{
    std::replace(uri.begin(), uri.end(), '/', '_');
    return uri;
}

static void sigHandler(int signo)
{
    /* Only flag the interrupt: throwing from an async signal handler is
       undefined behavior. nix::checkInterrupt() throws nix::Interrupted
       at the next safe point, so the stack still unwinds and destructors
       (job cancellation etc.) run. */
    nix::setInterrupted(true);
}

/* Streambuf that writes the build log to Nix on fd 4. Once Nix has decided
   to tear the hook down it stops draining this pipe while it waits for us
   to exit (SIGKILL after 20s), so writes must never block indefinitely:
   fd 4 is made O_NONBLOCK, and once `abend` is set and the pipe has stayed
   full for ~1s, the remaining log data is dropped. */
struct LogPipeBuf : std::streambuf
{
    explicit LogPipeBuf(std::atomic<bool> & abend) : abend(abend) {}

    std::streamsize xsputn(const char * s, std::streamsize n) override
    {
        if (dropping)
            return n;
        std::streamsize written = 0;
        while (written < n) {
            ssize_t res = write(4, s + written, n - written);
            if (res > 0) {
                written += res;
                stalls = 0;
            } else if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd pfd = {.fd = 4, .events = POLLOUT, .revents = 0};
                if (poll(&pfd, 1, 100) > 0)
                    stalls = 0;
                else if (abend && ++stalls >= 10) {
                    dropping = true;
                    break;
                }
            } else if (res == -1 && errno != EINTR) {
                dropping = true;
                break;
            }
        }
        /* Report success even when dropping, so the stream stays usable. */
        return n;
    }

    int_type overflow(int_type c) override
    {
        if (c != traits_type::eof()) {
            char ch = traits_type::to_char_type(c);
            xsputn(&ch, 1);
        }
        return c;
    }

    std::atomic<bool> & abend;
    int stalls = 0;
    bool dropping = false;
};

struct NSHCli {
    nix::Verbosity verbosity;
};

static std::expected<NSHCli, nix::Error> parseCli(int argc, char ** argv)
{
    if (argc != 2) {
        return std::unexpected(
            nix::Error(std::format("expected exactly one argument, got {}", argc)));
    }

    nix::Verbosity verbosity;
    try {
        verbosity = static_cast<nix::Verbosity>(std::stoi(std::string(argv[1])));
    } catch (const std::exception & e) {
        auto error = nix::Error(e.what());
        error.addTrace({},
            std::format("expected valid integer verbosity argument < {}, got `{}`",
                static_cast<int>(nix::lvlVomit), argv[1]));
        return std::unexpected(error);
    }
    if (verbosity < nix::lvlError || verbosity > nix::lvlVomit) {
        return std::unexpected(nix::Error(
            std::format("expected verbosity between {} and {}, got `{}`",
                static_cast<int>(nix::lvlError), static_cast<int>(nix::lvlVomit),
                static_cast<int>(verbosity))));
    }

    return NSHCli{verbosity};
}

int main(int argc, char **argv)
{
try {
    /* makeJSONLogger returns a unique_ptr as of nix 2.35; the global logger
     * is still a raw pointer, and the hook lives for one build only. */
    nix::logger = nix::makeJSONLogger(nix::getStandardError()).release();

    /* Ensure we don't get any SSH passphrase or host key popups. */
    unsetenv("DISPLAY");
    unsetenv("SSH_ASKPASS");

    auto cli = parseCli(argc, argv);
    if (!cli) {
        cli.error().addTrace({}, "failed to parse cli arguments");
        using namespace nix;
        printError("NSH Error: %s", cli.error().what());
        std::cerr << "# decline-permanently\n";
        return 1;
    }
    nix::verbosity = cli->verbosity;

    nix::FdSource source(STDIN_FILENO);

    /* Read the parent's settings. */
    if (auto res = transferSettingsIn(source, nix::globalConfig); !res) {
        res.error().addTrace({}, "failed to read and transfer settings from parent nix process");
        using namespace nix;
        printError("NSH Error: %s", res.error().what());
        std::cerr << "# decline-permanently\n";
        return 1;
    }

    /* Nix probes the hook (and winds it down between builds) by closing
       our stdin; an unreadable or non-"try" request just means there is
       no more work, so exit quietly rather than declining. */
    auto buildRequestUnvalidated = BuildRequest<std::string>::read(source);
    if (!buildRequestUnvalidated || buildRequestUnvalidated->command != "try")
        return 0;

    nix::initLibStore();
    nix::initPlugins();
    auto store = nix::openStore();

    /* Ensure destructors are called if terminated by Nix */
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigHandler;
    if (sigaction(SIGTERM, &act, 0))
        throw nix::SysError("assigning handler for SIGTERM");

    /* Nix delivers SIGTERM to our whole process group, so the ssh helper
       processes die before we finish unwinding; a write to one of their
       pipes would then raise SIGPIPE, whose default action terminates the
       process without running any destructor. Ignore it so such writes
       fail with EPIPE and become ordinary errors (initNix() shields
       regular nix processes the same way, but we only run initLibStore()). */
    act.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &act, 0))
        throw nix::SysError("ignoring SIGPIPE");

    /* It would be more appropriate to use $XDG_RUNTIME_DIR, since
        that gets cleared on reboot, but it wouldn't work on macOS. */
    if (auto localStore = store.dynamic_pointer_cast<nix::LocalFSStore>())
        currentLoad = localStore->config.stateDir.get() / "current-load";
    else
        currentLoad = std::filesystem::path{nix::settings.nixStateDir} / "current-load";

    if (auto res = readConfig(ourSettings); !res) {
        res.error().addTrace({}, "failed to read nsh configuration");
        using namespace nix;
        printError("NSH Error: %s", res.error().what());
        std::cerr << "# decline-permanently\n";
        return 1;
    }

    auto buildRequest = buildRequestUnvalidated->validate(
        store,
        ourSettings.systems.get(),
        ourSettings.systemFeatures.get(),
        ourSettings.mandatorySystemFeatures.get());
    if (!buildRequest) {
        {
            using namespace nix;
            printError("NSH: cannot handle this build: %s", buildRequest.error().what());
        }
        try {
            nix::Activity act(*nix::logger, nix::lvlInfo, nix::actUnknown, "falling back to normal build hook");
            auto remoteBuilder = NixBuildRemoteProcess::start(*buildRequestUnvalidated, source);
            if (!remoteBuilder) {
                remoteBuilder.error().addTrace({}, "failed to start nix remote builder process");
                throw remoteBuilder.error();
            }
            auto returnCode = remoteBuilder->wait();
            if (!returnCode) {
                returnCode.error().addTrace({}, "failed to wait for nix remote builder to finish");
                throw returnCode.error();
            }
            return *returnCode;
        } catch (nix::Interrupted &) {
            throw;
        } catch (std::exception & e) {
            using namespace nix;
            printError("NSH Error: unable to fallback to normal build hook: %s", e.what());
            std::cerr << "# decline\n";
            return 0;
        }
    }

    nix::StorePath drvPath = buildRequest->derivationPath;
    auto & neededSystem = buildRequest->system;
    auto & requiredFeatures = buildRequest->systemFeatures;

    std::unique_ptr<Scheduler> scheduler;
    try {
        if (ourSettings.jobScheduler.get() == "slurm") {
            scheduler = std::make_unique<Slurm>();
        } else if (ourSettings.jobScheduler.get() == "slurm-native") {
            scheduler = std::make_unique<SlurmNative>();
        } else if (ourSettings.jobScheduler.get() == "pbs") {
            scheduler = std::make_unique<PBS>();
        } else {
            using namespace nix;
            printError("NSH Error: unsupported job scheduler %s", ourSettings.jobScheduler.get());
            std::cerr << "# decline-permanently\n";
            return 0;
        }
    } catch (std::exception & e) {
        using namespace nix;
        printError("NSH Error: %s", e.what());
        std::cerr << "# decline-permanently\n";
        return 0;
    }

    nix::StringSet inputs;
    nix::StringSet wantedOutputs;

    if (ourSettings.earlyAccept.get()) {
        std::cerr << "# accept\n" << ourSettings.jobScheduler.get() << "\n";
        inputs = nix::readStrings<nix::StringSet>(source);
        wantedOutputs = nix::readStrings<nix::StringSet>(source);
    }

    auto drv = store->readDerivation(drvPath);
    nix::StorePathSet wantedPaths;
    for (auto & [name, output] : drv.outputsAndOptPaths(*store)) {
        wantedPaths.insert(*output.second);
    }

    std::string host;
    try {
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, "submitting build to scheduler");
        host = scheduler->startBuild(drvPath, drv, neededSystem, requiredFeatures, wantedPaths);
    } catch (nix::Interrupted &) {
        throw;
    } catch (std::exception & e) {
        auto errorMsg = nix::fmt("error when attempting to build derivation on %s: %s", ourSettings.jobScheduler.get(), e.what());
        if (ourSettings.earlyAccept.get()) {
            // We can't decline because we already accepted, throw an exception
            throw nix::Error(errorMsg);
        } else {
            using namespace nix;
            printError(errorMsg);
            std::cerr << "# decline-permanently\n";
            return 0;
        }
    }
    nix::Activity startedJobAct(*nix::logger, nix::lvlInfo, nix::actUnknown, nix::fmt("started job %s on %s", scheduler->getJobId(drvPath), host));

    std::string storeUri;
    if (ourSettings.sshUser.get() != "")
        storeUri = nix::fmt("ssh-ng://%s@%s:%d", ourSettings.sshUser.get(), host, ourSettings.sshPort.get());
    else
        storeUri = nix::fmt("ssh-ng://%s:%d", host, ourSettings.sshPort.get());

    std::shared_ptr<nix::Store> sshStore;
    {
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, nix::fmt("connecting to '%s'", storeUri));
        try {
            nix::StoreReference::Params params = {{"remote-store", ourSettings.remoteStore.get()}};
            if (ourSettings.remoteNixBinDir.get() != "")
                params["remote-program"] = ourSettings.remoteNixBinDir.get() + "/nix-daemon";
            sshStore = nix::openStore(storeUri, params);
            sshStore->connect();
        } catch (nix::Interrupted &) {
            throw;
        } catch (std::exception & e) {
            auto msg = nix::chomp(nix::drainFD(5, {.block = false}));
            auto errorMsg = nix::fmt("cannot build on '%s': %s%s", storeUri, e.what(), msg.empty() ? "" : ": " + msg);
            if (ourSettings.earlyAccept.get()) {
                // We can't decline because we already accepted, throw an exception
                throw nix::Error(errorMsg);
            } else {
                using namespace nix;
                printError(errorMsg);
                std::cerr << "# decline\n";
                return 0;
            }
        }
    }

    if (!ourSettings.earlyAccept.get()) {
        std::cerr << "# accept\n" << storeUri << "\n";
        inputs = nix::readStrings<nix::StringSet>(source);
        wantedOutputs = nix::readStrings<nix::StringSet>(source);
    }

    mkdir(currentLoad.c_str(), 0777);

    nix::AutoCloseFD uploadLock;
    {
        auto setUpdateLock = [&](auto && fileName) {
            uploadLock = nix::openLockFile(currentLoad / (escapeUri(fileName) + ".upload-lock"), true);
        };
        try {
            setUpdateLock(storeUri);
        } catch (nix::SysError & e) {
            if (e.errNo != ENAMETOOLONG) {
                using namespace nix;
                printError(e.what());
                throw;
            }
            // Try again hashing the store URL so we have a shorter path
            auto h = nix::hashString(nix::HashAlgorithm::MD5, storeUri);
            setUpdateLock(h.to_string(nix::HashFormat::Base64, false));
        }
    }

    {
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, nix::fmt("waiting for the upload lock to '%s'", storeUri));

        auto old = signal(SIGALRM, handleAlarm);
        alarm(15 * 60);
        if (!nix::lockFile(uploadLock.get(), nix::LockType::ltWrite, true)) {
            using namespace nix;
            printError("NSH Error: somebody is hogging the upload lock for '%s', continuing...");
        }
        alarm(0);
        signal(SIGALRM, old);
    }

    auto substitute = nix::settings.getWorkerSettings().buildersUseSubstitutes ? nix::Substitute : nix::NoSubstitute;

    {
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, nix::fmt("copying dependencies to '%s'", storeUri));
        try {
            nix::copyPaths(*store, *sshStore, store->parseStorePathSet(inputs), nix::NoRepair, nix::NoCheckSigs, substitute);
        } catch (nix::Interrupted &) {
            throw;
        } catch (std::exception & e) {
            throw nix::Error("error when attempting to copy build dependencies: %s", e.what());
        }

        // We only want to copy the derivation closure in the non-remote building case. Otherwise, build remotely.
        if (!ourSettings.remoteBuilding.get()) {
            nix::StringSet rootDrv;
            rootDrv.insert(store->printStorePath(drvPath));
            try {
                nix::copyClosure(*store, *sshStore, store->parseStorePathSet(rootDrv), nix::NoRepair, nix::NoCheckSigs, nix::SubstituteFlag::NoSubstitute);
            } catch (nix::Interrupted &) {
                throw;
            } catch (std::exception & e) {
                throw nix::Error("error when attempting to copy root derivation closure: %s", e.what());
            }
        }
    }

    if (ourSettings.remoteBuilding.get()) {
        auto drv = store->readDerivation(drvPath);

        // We always use ssh-ng, so we always know if we're trusted or not
        bool trusted = *sshStore->isTrustedClient();

        std::optional<nix::BuildResult> optResult;

        if (trusted || drv.type().isCA()) {
            if (!drv.inputDrvs.map.empty())
                drv.inputSrcs = store->parseStorePathSet(inputs);
            /* buildDerivation lives on the Builder interface as of the
             * Phase-2 API; ssh-ng (RemoteStore) is always a BuildStore.
             * The inputs were already copied above, so use the plain
             * overload rather than the inputs one. */
            auto buildStore = std::dynamic_pointer_cast<nix::BuildStore>(sshStore);
            if (!buildStore)
                throw nix::Error("store '%s' does not support building", storeUri);
            optResult = buildStore->getBuilder()->buildDerivation(drvPath, static_cast<const nix::BasicDerivation &>(drv));
            auto & result = *optResult;
            if (auto * failureP = result.tryGetFailure()) {
                if (nix::settings.keepFailed)
                    nix::warn("The failed build directory was kept on the remote builder due to `--keep-failed`.%s");
                throw nix::Error(
                    "build of '%s' on '%s' failed: %s", store->printStorePath(drvPath), storeUri, failureP->message());
            }
        } else {
            throw nix::Error("cannot build with remote build mode, we are not a trusted client. Add the SSH user to the trusted-users setting on the remote.");
        }
    }

    uploadLock = -1;

    if (!ourSettings.remoteBuilding.get()) {
        std::atomic<bool> cmdAbend = false;
        std::atomic<bool> cmdOutDone = false;
        std::atomic<bool> cmdOutFailed = false;

        std::thread cmdOutThread([&]() {
            /* An exception escaping a thread calls std::terminate(), skipping
            all cleanup, so trap everything and report failure instead. */
            try {
                auto cmdOutIs = scheduler->getStderrStream(drvPath);

                // The invoking Nix process listens on fd 4 for the build log
                // See https://github.com/NixOS/nix/blob/master/src/libstore/unix/build/hook-instance.cc#L61
                fcntl(4, F_SETFL, fcntl(4, F_GETFL, 0) | O_NONBLOCK);
                LogPipeBuf logBuf(cmdAbend);
                std::ostream logOs(&logBuf);

                bool gotTerminator = false;
                while (!gotTerminator && !cmdAbend) {
                    std::string data;
                    char c;
                    while (cmdOutIs->get(c)) {
                        data += c;
                    }
                    if (data != "") {
                        gotTerminator = handleOutput(logOs, data);
                    } else {
                        std::this_thread::yield();
                        cmdOutIs->clear();
                    }
                }
                if (cmdAbend) {
                    // Drain in the case of abnormal termination
                    std::string data;
                    char c;
                    while (cmdOutIs->get(c)) {
                        data += c;
                    }
                    if (data != "") {
                        handleOutput(logOs, data);
                    }
                }
            } catch (std::exception & e) {
                using namespace nix;
                printError("NSH Error: build log thread: %s", e.what());
                cmdOutFailed = true;
            } catch (...) {
                cmdOutFailed = true;
            }
            cmdOutDone = true;
        });

        /* Join cmdOutThread on every exit path: destroying a joinable
        std::thread calls std::terminate(), which on stack unwinding would
        kill the process before the scheduler's destructor is reached. */
        Finally joinCmdOutThread([&]() {
            cmdAbend = true;
            if (cmdOutThread.joinable())
                cmdOutThread.join();
        });

        int rc;
        try {
            rc = scheduler->waitForJobFinish(drvPath);
        } catch (nix::Interrupted &) {
            throw;
        } catch (std::exception & e) {
            using namespace nix;
            printError("NSH Error: error while waiting for job %s termination: %s", scheduler->getJobId(drvPath), e.what());
            cmdAbend = true;
            cmdOutThread.join();
            return 1;
        }
        if (rc == -1) {
            using namespace nix;
            printError("NSH Error: job %s abnormally terminated.", scheduler->getJobId(drvPath));
            cmdAbend = true;
            cmdOutThread.join();
            return 1;
        } else if (rc) {
            // Build failed, so no more work to do
            using namespace nix;
            printError("build failed with exit code %d", rc);
            cmdAbend = true;
            cmdOutThread.join();
            return rc;
        }

        /* The terminator can never arrive if the ssh/tail stream died, so
        bound the wait rather than joining unconditionally. */
        for (int i = 0; i < 100 && !cmdOutDone; ++i)
            interruptibleSleep(100ms);
        cmdAbend = true;
        cmdOutThread.join();

        if (cmdOutFailed)
            return 1;
    } else {
        int rc;
        try {
            rc = scheduler->waitForJobFinish(drvPath);
        } catch (nix::Interrupted &) {
            throw;
        } catch (std::exception & e) {
            using namespace nix;
            printError("NSH Error: error while waiting for job %s termination: %s", scheduler->getJobId(drvPath), e.what());
            return 1;
        }
        if (rc == -1) {
            using namespace nix;
            printError("NSH Error: job %s abnormally terminated.", scheduler->getJobId(drvPath));
            return 1;
        } else if (rc) {
            // Job script failed, so no more work to do
            using namespace nix;
            printError("job script failed with exit code %d", rc);
            return rc;
        }
    }

    using namespace nix;
    std::set<Realisation> missingRealisations;
    StorePathSet missingPaths;
    if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations) && !drv.type().hasKnownOutputPaths()) {
        for (auto & outputName : wantedOutputs) {
            /* Realisations are keyed by drv path + output name as of the
             * 2.35 API (previously by static output hash). */
            auto thisOutputId = DrvOutput{drvPath, outputName};
            if (!store->queryRealisation(thisOutputId)) {
                debug("missing output %s", outputName);
                auto r = sshStore->queryRealisation(thisOutputId);
                if (!r) {
                    using namespace nix;
                    printError("NSH Error: realisation for output %s not found on remote store", outputName);
                    return 1;
                }
                missingRealisations.insert({*r, thisOutputId});
                missingPaths.insert(r->outPath);
            }
        }
    } else {
        auto outputPaths = drv.outputsAndOptPaths(*store);
        for (auto & [outputName, hopefullyOutputPath] : outputPaths) {
            assert(hopefullyOutputPath.second);
            if (!store->isValidPath(*hopefullyOutputPath.second))
                missingPaths.insert(*hopefullyOutputPath.second);
        }
    }

    if (!missingPaths.empty()) {
        Activity act(*logger, lvlTalkative, actUnknown, fmt("copying outputs from '%s'", storeUri));
        if (auto localStore = store.dynamic_pointer_cast<LocalStore>())
            for (auto & path : missingPaths)
                localStore->locksHeld.insert(store->printStorePath(path)); /* FIXME: ugly */
        copyPaths(*sshStore, *store, missingPaths, NoRepair, NoCheckSigs, NoSubstitute);
    }

    // XXX: Should be done as part of `copyPaths`
    for (auto & realisation : missingRealisations) {
        // Should hold, because if the feature isn't enabled the set
        // of missing realisations should be empty
        experimentalFeatureSettings.require(Xp::CaDerivations);
        store->registerDrvOutput(realisation);
    }
} catch (nix::Interrupted &) {
    /* SIGTERM from Nix: cleanup already happened while unwinding. */
    return 0;
} catch (std::exception & e) {
    /* Without a matching handler the runtime terminates WITHOUT unwinding,
       so destructors (job cancellation etc.) would be skipped. */
    using namespace nix;
    printError("NSH Error: %s", e.what());
    return 1;
}

    return 0;
}
