#include "nsh-store.hh"
#include "slurm.hh"
#include "pbs.hh"
#include "slurm-native.hh"
#include "logging.hh"

#include <atomic>
#include <fstream>
#include <thread>
#include <ext/stdio_filebuf.h>
#include <unistd.h>

#include <nix/store/store-open.hh>
#include <nix/store/store-api.hh>
#include <nix/store/local-store.hh>
#include <nix/store/globals.hh>
#include <nix/store/derivations.hh>
#include <nix/store/realisation.hh>
#include <nix/store/build-result.hh>
#include <nix/store/path-with-outputs.hh>
#include <nix/util/callback.hh>
#include <nix/util/logging.hh>
#include <nix/util/error.hh>
#include <nix/util/strings.hh>
#include <nix/util/experimental-features.hh>

namespace nix {

/* ------------------------------------------------------------------ *
 *  NshStoreConfig
 * ------------------------------------------------------------------ */

void NshStoreConfig::anchor() {}

/* Feed the store's query parameters into the global `Settings ourSettings`
 * so the scheduler backends keep reading their configuration from there.
 * `nsh.conf` (if present) is loaded first and overridden by query params. */
static StoreReference::Params bridgeParamsToSettings(const StoreReference::Params & params)
{
    ::loadConfFile(ourSettings);
    for (const auto & [name, value] : params) {
        try {
            ourSettings.set(name, value);
        } catch (...) {
            /* Not an NSH setting (e.g. a generic StoreConfig key like
             * `trusted`); leave it for StoreConfig to handle. */
        }
    }
    /* Strip the NSH-specific keys so StoreConfig doesn't warn about them. */
    StoreReference::Params filtered = params;
    std::map<std::string, AbstractConfig::SettingInfo> known;
    ourSettings.getSettings(known, /*overriddenOnly=*/false);
    for (const auto & [nshKey, _] : known)
        filtered.erase(nshKey);
    return filtered;
}

NshStoreConfig::NshStoreConfig(const std::filesystem::path & /*path*/, const Params & params)
    : StoreConfig(bridgeParamsToSettings(params), FilePathType::Native)
{
}

NshStoreConfig::NshStoreConfig(const Params & params)
    : NshStoreConfig(std::filesystem::path{}, params)
{
}

std::string NshStoreConfig::doc()
{
    return
        "This store dispatches builds through an HPC job scheduler "
        "(Slurm REST, libslurm, or PBS). It is used as a build machine "
        "store (`nix.buildMachines` with a `nsh://` storeUri), not as a "
        "top-level `--store`. All nix-scheduler-hook settings (e.g. "
        "`job-scheduler`, `slurm-state-dir`) are given as URL query "
        "parameters.";
}

ref<Store> NshStoreConfig::openStore() const
{
    return make_ref<NshStore>(ref{shared_from_this()});
}

/* ------------------------------------------------------------------ *
 *  NshStore
 * ------------------------------------------------------------------ */

void NshStore::anchor() {}

NshStore::NshStore(ref<const Config> config)
    : Store{*config}
    /* Open the genuine local store as the backing store. We pass "auto"
     * explicitly rather than the no-arg openStore(), which resolves the
     * global `store` setting: when nsh:// is itself the `--store`/default,
     * that would re-open nsh:// and recurse until the stack overflows.
     * "auto" always denotes the node-local daemon/local store. */
    , config{config}
    , backing{nix::openStore("auto")}
{
}

ref<Builder> NshStore::getBuilder(std::shared_ptr<Store> evalStore)
{
    return make_ref<NshBuilder>(*this, std::move(evalStore));
}

void NshStore::queryPathInfoUncached(
    const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept
{
    try {
        auto info = backing->queryPathInfo(path);
        callback(std::shared_ptr<const ValidPathInfo>(info.get_ptr()));
    } catch (InvalidPath &) {
        callback(nullptr);
    } catch (...) {
        callback.rethrow();
    }
}

void NshStore::queryRealisationUncached(
    const DrvOutput & id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept
{
    backing->queryRealisation(id, std::move(callback));
}

std::optional<StorePath> NshStore::queryPathFromHashPart(const std::string & hashPart)
{
    return backing->queryPathFromHashPart(hashPart);
}

void NshStore::narFromPath(const StorePath & path, Sink & sink)
{
    backing->narFromPath(path, sink);
}

ref<SourceAccessor> NshStore::getFSAccessor(bool requireValidPath)
{
    return backing->getFSAccessor(requireValidPath);
}

std::shared_ptr<SourceAccessor> NshStore::getFSAccessor(const StorePath & path, bool requireValidPath)
{
    return backing->getFSAccessor(path, requireValidPath);
}

void NshStore::addToStore(
    const ValidPathInfo & info, Source & narSource, RepairFlag repair, CheckSigsFlag checkSigs)
{
    backing->addToStore(info, narSource, repair, checkSigs);
}

StorePath NshStore::addToStoreFromDump(
    Source & dump,
    std::string_view name,
    FileSerialisationMethod dumpMethod,
    ContentAddressMethod hashMethod,
    HashAlgorithm hashAlgo,
    const StorePathSet & references,
    RepairFlag repair)
{
    return backing->addToStoreFromDump(dump, name, dumpMethod, hashMethod, hashAlgo, references, repair);
}

void NshStore::registerDrvOutput(const Realisation & output)
{
    backing->registerDrvOutput(output);
}

/* ------------------------------------------------------------------ *
 *  NshBuilder
 * ------------------------------------------------------------------ */

/* Temporary PoC tracing: the builder runs inside `nix __build-remote`,
 * whose stderr only reaches the user after completion, so a hang is
 * invisible. Writing to /dev/console makes progress show up live on the
 * VM test driver's log. Remove once the plugin path is stable. */
static void trace(const std::string & msg)
{
    try {
        std::ofstream con("/dev/console");
        con << "NSH-TRACE: " << msg << std::endl;
    } catch (...) {
    }
}

static std::unique_ptr<Scheduler> makeScheduler()
{
    const auto & js = ourSettings.jobScheduler.get();
    if (js == "slurm")
        return std::make_unique<Slurm>();
    if (js == "slurm-native")
        return std::make_unique<SlurmNative>();
    if (js == "pbs")
        return std::make_unique<PBS>();
    throw Error("nix-scheduler-hook: unsupported job scheduler '%s'", js);
}

NshBuilder::NshBuilder(NshStore & nshStore, std::shared_ptr<Store> evalStore)
    : nshStore(nshStore)
    , evalStore(std::move(evalStore))
    , scheduler(makeScheduler())
{
}

ref<Store> NshBuilder::srcStore()
{
    if (evalStore)
        return ref<Store>(evalStore);
    if (!srcStoreCache)
        srcStoreCache = nix::openStore();
    return ref<Store>(srcStoreCache);
}

namespace {

/* Forwards each completed line to nix's logger. The builder runs inside a
 * `nix __build-remote` child whose raw stderr is not captured into the parent's
 * build log; logger messages are serialised back to the parent and do arrive. */
struct LoggerStreambuf : std::streambuf
{
    std::string line;
    int overflow(int c) override
    {
        if (c == traits_type::eof())
            return c;
        if (c == '\n') {
            logger->log(lvlInfo, line);
            line.clear();
        } else
            line += static_cast<char>(c);
        return c;
    }
    ~LoggerStreambuf() override
    {
        if (!line.empty())
            logger->log(lvlInfo, line);
    }
};

} // namespace

BuildResult NshBuilder::buildDerivation(
    const StorePath & drvPath, const BasicDerivation & drv, const StorePathSet & inputs, BuildMode buildMode)
{
    if (buildMode != bmNormal)
        throw Unsupported("nix-scheduler-hook only supports normal builds");

    auto src = srcStore();
    auto & backing = *nshStore.backing;

    std::string system = drv.platform;
    StringSet requiredFeatures;
    if (auto i = drv.env.find("requiredSystemFeatures"); i != drv.env.end())
        requiredFeatures = tokenizeString<StringSet>(i->second);

    /* Output paths the remote-building script polls for; CA outputs have no
     * known path up front, so they can't be waited on this way. */
    StorePathSet wantedPaths;
    for (auto & [name, output] : drv.outputsAndOptPaths(backing))
        if (output.second)
            wantedPaths.insert(*output.second);

    /* 1. Submit to the scheduler, select a node, open an SSH connection. */
    trace("buildDerivation start: " + std::string(drvPath.to_string()));
    std::string host;
    {
        Activity act(*logger, lvlTalkative, actUnknown, "submitting build to scheduler");
        /* The inputs drive input-aware placement (candidate-nodes) before
         * the job is allocated; copying them happens after, in step 2. */
        host = scheduler->startBuild(drvPath, drv, system, requiredFeatures, wantedPaths, inputs);
    }
    trace("submitted, host=" + host + " job=" + scheduler->getJobId(drvPath));

    std::string storeUri;
    if (ourSettings.sshUser.get() != "")
        storeUri = fmt("ssh-ng://%s@%s:%d", ourSettings.sshUser.get(), host, ourSettings.sshPort.get());
    else
        storeUri = fmt("ssh-ng://%s:%d", host, ourSettings.sshPort.get());

    Activity startedJobAct(
        *logger, lvlInfo, actUnknown, fmt("started job %s on %s", scheduler->getJobId(drvPath), host));

    std::shared_ptr<Store> nodeStore;
    {
        Activity act(*logger, lvlTalkative, actUnknown, fmt("connecting to '%s'", storeUri));
        StoreReference::Params params = {{"remote-store", ourSettings.remoteStore.get()}};
        if (ourSettings.remoteNixBinDir.get() != "")
            params["remote-program"] = ourSettings.remoteNixBinDir.get() + "/nix-daemon";
        trace("opening node store " + storeUri);
        nodeStore = nix::openStore(storeUri, params);
        nodeStore->connect();
    }
    trace("node store connected");

    /* 2. Copy the build inputs and the derivation closure to the node. */
    auto substitute = settings.getWorkerSettings().buildersUseSubstitutes ? Substitute : NoSubstitute;
    {
        Activity act(*logger, lvlTalkative, actUnknown, fmt("copying dependencies to '%s'", storeUri));
        copyPaths(*src, *nodeStore, inputs, NoRepair, NoCheckSigs, substitute);
        trace("inputs copied");
        copyClosure(*src, *nodeStore, StorePathSet{drvPath}, NoRepair, NoCheckSigs, substitute);
        trace("drv closure copied");
    }

    /* 3. Stream the node's build log to the invoking build via the logger. */
    std::atomic<bool> cmdAbend = false;
    std::thread cmdOutThread([&]() {
      /* handleOutput can throw (log size limit); an exception escaping a
       * thread body calls std::terminate and kills __build-remote with no
       * diagnostics. Contain it: log streaming is best-effort. */
      try {
        trace("log thread: attaching stderr stream");
        auto cmdOutIs = scheduler->getStderrStream(drvPath);
        trace("log thread: stream attached");
        LoggerStreambuf logBuf;
        std::ostream logOs(&logBuf);

        bool gotTerminator = false;
        while (!gotTerminator && !cmdAbend) {
            std::string data;
            char c;
            while (cmdOutIs->get(c))
                data += c;
            if (!data.empty())
                gotTerminator = handleOutput(logOs, data);
            else {
                std::this_thread::yield();
                cmdOutIs->clear();
            }
        }
        if (cmdAbend) {
            std::string data;
            char c;
            while (cmdOutIs->get(c))
                data += c;
            if (!data.empty())
                handleOutput(logOs, data);
        }
        logOs.flush();
      } catch (std::exception & e) {
        trace(std::string("log thread: exception: ") + e.what());
      }
    });

    /* 4. Wait for the job to finish. */
    trace("waiting for job finish");
    int rc;
    try {
        rc = scheduler->waitForJobFinish(drvPath);
        trace("job finished rc=" + std::to_string(rc));
    } catch (std::exception & e) {
        cmdAbend = true;
        cmdOutThread.join();
        throw BuildError(
            BuildResult::Failure::TransientFailure,
            "error while waiting for job %s: %s",
            scheduler->getJobId(drvPath),
            e.what());
    }

    if (rc != 0) {
        cmdAbend = true;
        cmdOutThread.join();
        if (rc == -1)
            throw BuildError(
                BuildResult::Failure::TransientFailure,
                "job %s abnormally terminated",
                scheduler->getJobId(drvPath));
        throw BuildError(
            BuildResult::Failure::PermanentFailure,
            "builder for '%s' failed with exit code %d",
            nshStore.printStorePath(drvPath),
            rc);
    }

    /* The job's exit code from the scheduler is authoritative; don't gate
     * completion on the log thread having seen the '@nsh done' sentinel —
     * if the tail stream died (or the sentinel was lost), the thread would
     * otherwise spin forever and this join would hang the whole build. */
    cmdAbend = true;
    cmdOutThread.join();
    trace("log thread joined");

    /* 5. Copy the outputs from the node into the backing store and build the
     * BuildResult. `__build-remote` copies them back out afterwards. */
    BuildResult::Success success{.status = BuildResult::Success::Built};
    StorePathSet missingPaths;
    std::vector<Realisation> caRealisations;

    if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations) && !drv.type().hasKnownOutputPaths()) {
        for (auto & [outputName, _] : drv.outputs) {
            DrvOutput id{drvPath, outputName};
            auto r = nodeStore->queryRealisation(id);
            if (!r)
                throw BuildError(
                    BuildResult::Failure::OutputRejected,
                    "realisation for output '%s' not found on the build node",
                    outputName);
            success.builtOutputs.insert({outputName, *r});
            missingPaths.insert(r->outPath);
            caRealisations.push_back(Realisation{*r, id});
        }
    } else {
        auto outputPaths = drv.outputsAndOptPaths(backing);
        for (auto & [outputName, hopefullyOutputPath] : outputPaths) {
            assert(hopefullyOutputPath.second);
            if (!backing.isValidPath(*hopefullyOutputPath.second))
                missingPaths.insert(*hopefullyOutputPath.second);
        }
    }

    if (!missingPaths.empty()) {
        Activity act(*logger, lvlTalkative, actUnknown, fmt("copying outputs from '%s'", storeUri));
        /* The invoking nix already holds the build locks on these outputs;
         * without marking them held, addToStore would deadlock on them.
         * Same workaround as nix's own build-remote. */
        if (auto * localBacking = dynamic_cast<LocalStore *>(&backing))
            for (auto & path : missingPaths)
                localBacking->locksHeld.insert(backing.printStorePath(path));
        trace("copying outputs back");
        copyPaths(*nodeStore, backing, missingPaths, NoRepair, NoCheckSigs, NoSubstitute);
    }
    for (auto & realisation : caRealisations)
        backing.registerDrvOutput(realisation);

    trace("buildDerivation done");
    BuildResult result;
    result.inner = std::move(success);
    return result;
}

std::vector<KeyedBuildResult> NshBuilder::buildPathsWithResults(
    const std::vector<DerivedPath> & reqs, const StorePathSet & inputs, BuildMode buildMode)
{
    std::vector<KeyedBuildResult> results;
    for (auto & req : reqs) {
        auto * built = std::get_if<DerivedPath::Built>(&req.raw());
        if (!built)
            throw Unsupported("nix-scheduler-hook can only build derivations, not '%s'", req.to_string(nshStore));
        auto drvPath = built->drvPath->getBaseStorePath();
        auto drv = srcStore()->readDerivation(drvPath);
        BuildResult res;
        try {
            res = buildDerivation(drvPath, drv, inputs, buildMode);
        } catch (BuildError & e) {
            res.inner = static_cast<BuildResult::Failure &>(e);
        }
        results.emplace_back(std::move(res), req);
    }
    return results;
}

void NshBuilder::buildPaths(const std::vector<DerivedPath> & reqs, BuildMode buildMode)
{
    for (auto & result : buildPathsWithResults(reqs, StorePathSet{}, buildMode))
        result.tryThrowBuildError();
}

std::vector<KeyedBuildResult>
NshBuilder::buildPathsWithResults(const std::vector<DerivedPath> & reqs, BuildMode buildMode)
{
    return buildPathsWithResults(reqs, StorePathSet{}, buildMode);
}

BuildResult NshBuilder::buildDerivation(const StorePath & drvPath, const BasicDerivation & drv, BuildMode buildMode)
{
    return buildDerivation(drvPath, drv, StorePathSet{}, buildMode);
}

void NshBuilder::ensurePath(const StorePath & path)
{
    if (!nshStore.isValidPath(path))
        throw Unsupported("nix-scheduler-hook cannot substitute path '%s'", nshStore.printStorePath(path));
}

void NshBuilder::repairPath(const StorePath & path)
{
    throw Unsupported("operation 'repairPath' is not supported by the nix-scheduler-hook store");
}

/* ------------------------------------------------------------------ *
 *  Registration — this static replaces the old main().
 * ------------------------------------------------------------------ */

static RegisterStoreImplementation<NshStoreConfig> regNshStore;

} // namespace nix
