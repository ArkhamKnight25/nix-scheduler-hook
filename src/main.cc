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

std::filesystem::path getBuildRemoteFromNixBin(std::filesystem::path nixBin)
{
    if (std::filesystem::is_symlink(nixBin))
        nixBin = std::filesystem::read_symlink(nixBin);
    return nixBin.parent_path().parent_path() / "libexec" / "nix" / "build-remote";
}

struct FallbackHookInstance
{
    FallbackHookInstance(
      int amWilling,
      std::string neededSystem,
      std::string drvPath,
      nix::StringSet requiredFeatures,
      nix::FdSource & source
    ) {
        toHook.create();

        pid = nix::startProcess([&]() {
            if (dup2(toHook.readSide.get(), STDIN_FILENO) == -1)
                throw nix::SysError("redirecting NSH's toHook to build-remote's STDIN");

            std::filesystem::path nixBinPath = "nix";
            auto nixBinDirOpt = nix::getEnvNonEmpty("NIX_BIN_DIR");
            if (nixBinDirOpt)
                nixBinPath = std::filesystem::path(*nixBinDirOpt) / "nix";

            nix::Strings args{nixBinPath.filename().string(), "__build-remote", std::to_string(nix::verbosity)};
            execvp(nixBinPath.native().c_str(), nix::stringsToCharPtrs(args).data());

            // If nix __build-remote doesn't work, try the legacy libexec/nix/build-remote symlink
            std::filesystem::path buildRemotePath;
            if (nixBinDirOpt) {
                std::filesystem::path nixBin = std::filesystem::path(*nixBinDirOpt) / "nix";
                if (std::filesystem::exists(nixBin))
                    buildRemotePath = getBuildRemoteFromNixBin(nixBin);
            }
            else if (auto pathOpt = nix::getEnvNonEmpty("PATH")) {
                auto paths = nix::tokenizeString<nix::Strings>(*pathOpt, ":");
                for (auto path : paths) {
                    std::filesystem::path nixBin = std::filesystem::path(path) / "nix";
                    if (std::filesystem::exists(nixBin)) {
                        buildRemotePath = getBuildRemoteFromNixBin(nixBin);
                        break;
                    }
                }
            }
            if (!buildRemotePath.empty()) {
                nix::Strings args2{buildRemotePath.filename().string(), std::to_string(nix::verbosity)};
                execv(buildRemotePath.native().c_str(), nix::stringsToCharPtrs(args2).data());
            }

            throw nix::SysError("executing normal build hook");
        });

        toHook.readSide = -1;

        sink = nix::FdSink(toHook.writeSide.get());
        std::map<std::string, nix::Config::SettingInfo> settings;
        nix::globalConfig.getSettings(settings);
        for (auto & setting : settings)
            sink << 1 << setting.first << setting.second.value;
        sink << 0;

        sink << "try" << amWilling << neededSystem << drvPath << requiredFeatures;
        sink.flush();

        auto inputs = nix::readStrings<nix::StringSet>(source);
        auto wantedOutputs = nix::readStrings<nix::StringSet>(source);

        sink << inputs << wantedOutputs;
        sink.flush();
    }

    int wait()
    {
        return pid.wait();
    }

    ~FallbackHookInstance()
    {
        if (pid != -1) {
            pid.kill();
            pid.wait();
        }
    }

    nix::Pipe toHook;
    nix::Pid pid;
    nix::FdSink sink;
};

int main(int argc, char **argv)
{
try {
    nix::logger = nix::makeJSONLogger(nix::getStandardError());

    /* Ensure we don't get any SSH passphrase or host key popups. */
    unsetenv("DISPLAY");
    unsetenv("SSH_ASKPASS");

    if (argc != 2)
        throw nix::UsageError("called without required arguments");

    nix::verbosity = (nix::Verbosity) std::stoll(argv[1]);

    nix::FdSource source(STDIN_FILENO);

    /* Read the parent's settings. */
    while (nix::readInt(source)) {
        auto name = nix::readString(source);
        auto value = nix::readString(source);
        nix::settings.set(name, value);
    }

    try {
        auto s = nix::readString(source);
        if (s != "try")
            return 0;
    } catch (nix::EndOfFile &) {
        return 0;
    }

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

    /* It would be more appropriate to use $XDG_RUNTIME_DIR, since
        that gets cleared on reboot, but it wouldn't work on macOS. */
    if (auto localStore = store.dynamic_pointer_cast<nix::LocalFSStore>())
        currentLoad = localStore->config.stateDir.get() / "current-load";
    else
        currentLoad = std::filesystem::path{nix::settings.nixStateDir} / "current-load";

    int amWilling = nix::readInt(source);

    ::loadConfFile(ourSettings);

    auto neededSystem = nix::readString(source);
    nix::StorePath drvPath = store->parseStorePath(nix::readString(source));
    auto requiredFeatures = nix::readStrings<nix::StringSet>(source);

    bool tryFallback = false;

    if (!ourSettings.systems.get().contains(neededSystem)) {
        using namespace nix;
        printError("needed system %s does not match our systems %s", neededSystem, boost::algorithm::join(ourSettings.systems.get(), ", "));
        tryFallback = true;
    }

    auto systemFeatures = ourSettings.systemFeatures.get();
    for (auto & feature : requiredFeatures) {
        if (systemFeatures.find(feature) == systemFeatures.end()) {
            using namespace nix;
            printError("required feature %s not available, available features:", feature);
            for (auto & f : systemFeatures) {
                printError(f);
            }
            tryFallback = true;
        }
    }

    auto mandatorySystemFeatures = ourSettings.mandatorySystemFeatures.get();
    for (auto & feature : mandatorySystemFeatures) {
        if (requiredFeatures.find(feature) == requiredFeatures.end()) {
            using namespace nix;
            printError("derivation does not require mandatory feature %s, required features:", feature);
            for (auto & f : requiredFeatures) {
                printError(f);
            }
            tryFallback = true;
        }
    }

    if (tryFallback) {
        try {
            nix::Activity act(*nix::logger, nix::lvlInfo, nix::actUnknown, "falling back to normal build hook");
            return FallbackHookInstance(amWilling, neededSystem, store->printStorePath(drvPath), requiredFeatures, source).wait();
        } catch (nix::Interrupted &) {
            throw;
        } catch (std::exception & e) {
            using namespace nix;
            printError("NSH Error: unable to fallback to normal build hook: %s", e.what());
            std::cerr << "# decline\n";
            return 0;
        }
    }

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

    if (ourSettings.earlyAccept.get())
        std::cerr << "# accept\n" << ourSettings.jobScheduler.get() << "\n";

    std::string host;
    try {
        nix::Activity act(*nix::logger, nix::lvlTalkative, nix::actUnknown, "submitting build to scheduler");
        host = scheduler->startBuild(drvPath, neededSystem, requiredFeatures);
    } catch (nix::Interrupted &) {
        throw;
    } catch (std::exception & e) {
        auto errorMsg = nix::fmt("NSH Error: error when attempting to build derivation on %s: %s", ourSettings.jobScheduler.get(), e.what());
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
            auto errorMsg = nix::fmt("NSH Error: cannot build on '%s': %s%s", storeUri, e.what(), msg.empty() ? "" : ": " + msg);
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

    if (!ourSettings.earlyAccept.get())
        std::cerr << "# accept\n" << storeUri << "\n";

    auto inputs = nix::readStrings<nix::StringSet>(source);
    auto wantedOutputs = nix::readStrings<nix::StringSet>(source);

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
            using namespace nix;
            printError("NSH Error: error when attempting to copy build dependencies: %s", e.what());
            std::cerr << "# decline-permanently\n";
            return 0;
        }
        nix::StringSet rootDrv;
        rootDrv.insert(store->printStorePath(drvPath));
        try {
            nix::copyClosure(*store, *sshStore, store->parseStorePathSet(rootDrv), nix::NoRepair, nix::NoCheckSigs, nix::SubstituteFlag::NoSubstitute);
        } catch (nix::Interrupted &) {
            throw;
        } catch (std::exception & e) {
            using namespace nix;
            printError("NSH Error: error when attempting to copy root derivation closure: %s", e.what());
            std::cerr << "# decline-permanently\n";
            return 0;
        }
    }

    uploadLock = -1;

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

    using namespace nix;
    auto drv = store->readDerivation(drvPath);
    auto outputHashes = staticOutputHashes(*store, drv);
    std::set<Realisation> missingRealisations;
    StorePathSet missingPaths;
    if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations) && !drv.type().hasKnownOutputPaths()) {
        for (auto & outputName : wantedOutputs) {
            auto thisOutputHash = outputHashes.at(outputName);
            auto thisOutputId = DrvOutput{thisOutputHash, outputName};
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
