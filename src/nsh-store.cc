#include "nsh-store.hh"
#include "scheduler/all.hh"
#include "logging.hh"

#include <atomic>
#include <fstream>
#include <map>
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
    /* Same layered config load as hook mode; a broken nsh.conf must fail
     * store opening loudly instead of being silently ignored. */
    if (auto res = ::readConfig(ourSettings); !res) {
        res.error().addTrace({}, "failed to read nsh configuration");
        throw res.error();
    }
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
    /* This store is a veneer: writes delegate straight to the backing store
     * and bypass this store's own path-info cache. Nix probes a .drv's
     * validity before writing it, so a negative entry cached here would
     * report the path invalid forever after the write (breaking e.g. the
     * whole-graph --store flow at copyClosure). The backing store has its
     * own cache; keep this one disabled. */
    filtered.try_emplace("path-info-cache-size", "0");
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
        "(Slurm REST, libslurm, or PBS). As a build machine store "
        "(`nix.buildMachines` with a `nsh://` storeUri) every derivation "
        "becomes its own scheduler job; as a top-level `--store` the "
        "requested derivation and its missing dependencies are built "
        "together in a single job (see \"Whole-Graph Builds\" in the "
        "README). All nix-scheduler-hook settings (e.g. `job-scheduler`, "
        "`slurm-state-dir`) are given as URL query parameters.";
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
    RepairFlag repair,
    std::shared_ptr<const Provenance> provenance)
{
    return backing->addToStoreFromDump(
        dump, name, dumpMethod, hashMethod, hashAlgo, references, repair, std::move(provenance));
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

/* Publishes each completed line of the node's build log as a build-log
 * result of `act`, the way DerivationBuildingGoal::flushLine publishes a
 * local builder's output. Plain `logger->log(lvlInfo, ...)` messages are
 * not an option: the `nix` CLI runs at lvlNotice on a terminal and its
 * progress bar drops every message above that level, so the log never
 * reached the user of `nix build --store nsh://`. Build-log results are
 * shown regardless of verbosity: as the activity's current line in the
 * progress bar, or in full with -L. Inside a `nix __build-remote` child
 * they are serialised to the parent as JSON; the parent replays them on a
 * mirrored activity and also copies them into the build log for `nix log`. */
struct BuildLogStreambuf : std::streambuf
{
    const Activity & act;
    /* Activities opened by `@nix {...}` lines in the log. An untrusted
     * builder may only open file transfers, but the API wants the map. */
    std::map<ActivityId, Activity> activities;
    std::string line;

    explicit BuildLogStreambuf(const Activity & act)
        : act(act)
    {
    }

    int overflow(int c) override
    {
        if (c == traits_type::eof())
            return c;
        if (c == '\n')
            flushLine();
        else
            line += static_cast<char>(c);
        return c;
    }

    void flushLine()
    {
        /* `@nix { "action": "setPhase", ... }` lines become phase results
         * rather than raw log lines, as they do for a local build. */
        if (!handleJSONLogMessage(line, act, activities, "the derivation builder", false))
            act.result(resBuildLogLine, line);
        line.clear();
    }

    ~BuildLogStreambuf() override
    {
        if (!line.empty())
            flushLine();
    }
};

} // namespace

BuildResult NshBuilder::buildDerivation(const StorePath & drvPath, const BasicDerivation & drv, BuildMode buildMode)
{
    /* Build-machine flow (`__build-remote`): nix primes `drv.inputSrcs` with
     * the build's input closure before calling (see build-remote.cc), so it
     * both gets copied to the node and drives placement; all outputs are
     * built and copied back. */
    StringSet allOutputs;
    for (auto & [name, _] : drv.outputs)
        allOutputs.insert(name);
    return buildDerivationImpl(drvPath, drv, drv.inputSrcs, drv.inputSrcs, buildMode, allOutputs);
}

BuildResult NshBuilder::buildDerivationImpl(
    const StorePath & drvPath,
    const BasicDerivation & drv,
    const StorePathSet & inputs,
    const StorePathSet & placementInputs,
    BuildMode buildMode,
    const StringSet & wantedOutputs)
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
        if (output.second && wantedOutputs.contains(name))
            wantedPaths.insert(*output.second);

    /* 1. Submit to the scheduler, select a node, open an SSH connection. */
    trace("buildDerivation start: " + std::string(drvPath.to_string()));
    std::string host;
    {
        Activity act(*logger, lvlTalkative, actUnknown, "submitting build to scheduler");
        /* placementInputs drive input-aware placement (candidate-nodes)
         * before the job is allocated; copying `inputs` happens after, in
         * step 2. */
        host = scheduler->startBuild(drvPath, drv, system, requiredFeatures, wantedPaths, placementInputs);
    }
    trace("submitted, host=" + host + " job=" + scheduler->getJobId(drvPath));

    std::string storeUri;
    if (ourSettings.sshUser.get() != "")
        storeUri = fmt("ssh-ng://%s@%s:%d", ourSettings.sshUser.get(), host, ourSettings.sshPort.get());
    else
        storeUri = fmt("ssh-ng://%s:%d", host, ourSettings.sshPort.get());

    /* The build's activity, alive until the outputs are back. The progress
     * bar renders it from the fields as "building <name> on <host> (job
     * <id>)" and shows the latest log line under it; the log thread in
     * step 3 publishes the node's build log as its results. */
    auto jobId = scheduler->getJobId(drvPath);
    Activity buildAct(
        *logger,
        lvlInfo,
        actBuild,
        fmt("building '%s' on '%s' (job %s)", nshStore.printStorePath(drvPath), host, jobId),
        Logger::Fields{nshStore.printStorePath(drvPath), fmt("%s (job %s)", host, jobId), 1, 1});

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
        /* Closure, not just the listed paths: `drv.inputSrcs` is only the
         * full input closure when build-remote rewrote it (inputDrvs
         * non-empty); for a resolved or source-only derivation it holds the
         * direct inputs, whose references must still reach the node. */
        copyClosure(*src, *nodeStore, inputs, NoRepair, NoCheckSigs, substitute);
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
        BuildLogStreambuf logBuf(buildAct);
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

    /* Every path out of the wait joins the log thread (a joinable
     * std::thread's destructor calls std::terminate) and then stops the
     * tail: left running, its ssh would linger until the scheduler is torn
     * down, and the teardown's unlink of the stderr file would make it
     * complain on our stderr. */
    auto finishLogStream = [&]() {
        cmdAbend = true;
        cmdOutThread.join();
        scheduler->stopStderrStream(drvPath);
    };

    /* 4. Wait for the job to finish. */
    trace("waiting for job finish");
    int rc;
    try {
        rc = scheduler->waitForJobFinish(drvPath);
        trace("job finished rc=" + std::to_string(rc));
    } catch (std::exception & e) {
        finishLogStream();
        throw BuildError(
            BuildResult::Failure::TransientFailure,
            "error while waiting for job %s: %s",
            scheduler->getJobId(drvPath),
            e.what());
    }

    if (rc != 0) {
        finishLogStream();
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
    finishLogStream();
    trace("log thread joined");

    /* 5. Copy the outputs from the node into the backing store and build the
     * BuildResult. `__build-remote` copies them back out afterwards. */
    BuildResult::Success success{.status = BuildResult::Success::Built};
    StorePathSet missingPaths;
    std::vector<Realisation> caRealisations;

    if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations) && !drv.type().hasKnownOutputPaths()) {
        for (auto & [outputName, _] : drv.outputs) {
            if (!wantedOutputs.contains(outputName))
                continue;
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
            if (!wantedOutputs.contains(outputName))
                continue;
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
        /* Closure, not just the outputs: in the whole-graph flow an output
         * can reference intermediate outputs that exist only on the node,
         * and the backing store refuses to register a path whose references
         * are missing. In the build-machine flow the references are already
         * valid locally, so the closure copy degenerates to the same set. */
        copyClosure(*nodeStore, backing, missingPaths, NoRepair, NoCheckSigs, NoSubstitute);
    }
    for (auto & realisation : caRealisations)
        backing.registerDrvOutput(realisation);

    trace("buildDerivation done");
    BuildResult result;
    result.inner = std::move(success);
    return result;
}

StorePathSet NshBuilder::placementInputsFor(const StorePath & drvPath)
{
    StorePathSet paths;
    try {
        auto src = srcStore();
        auto drv = src->readDerivation(drvPath);
        for (auto & p : drv.inputSrcs)
            paths.insert(p);
        for (auto & [inputDrvPath, inputNode] : drv.inputDrvs.map) {
            auto inputDrv = src->readDerivation(inputDrvPath);
            for (auto & [name, output] : inputDrv.outputsAndOptPaths(*src))
                if (output.second && inputNode.value.contains(name))
                    paths.insert(*output.second);
        }
    } catch (std::exception & e) {
        /* Best-effort: with no usable hint the submit simply falls back to
         * the scheduler's own placement. */
        logger->log(
            lvlTalkative,
            fmt("cannot derive placement inputs for '%s': %s", nshStore.printStorePath(drvPath), e.what()));
    }
    return paths;
}

std::vector<KeyedBuildResult>
NshBuilder::buildPathsWithResults(const std::vector<DerivedPath> & reqs, BuildMode buildMode)
{
    /* Whole-graph flow (`--store nsh://`): the node realises each requested
     * derivation together with its still-missing dependencies inside one
     * scheduler job. */
    std::vector<KeyedBuildResult> results;
    for (auto & req : reqs) {
        auto * built = std::get_if<DerivedPath::Built>(&req.raw());
        if (!built)
            throw Unsupported("nix-scheduler-hook can only build derivations, not '%s'", req.to_string(nshStore));
        auto drvPath = built->drvPath->getBaseStorePath();
        auto drv = srcStore()->readDerivation(drvPath);

        logger->log(
            lvlInfo,
            fmt("building '%s' and its missing dependencies as a single scheduler job", req.to_string(nshStore)));

        /* Honor the request's outputs spec: only the requested outputs are
         * waited on and copied back. */
        StringSet wantedOutputs;
        for (auto & [name, _] : drv.outputs)
            if (built->outputs.contains(name))
                wantedOutputs.insert(name);

        /* No input closure is copied to the node (intermediate outputs may
         * not exist anywhere yet); placement is scored on what is
         * statically known. */
        BuildResult res;
        try {
            res = buildDerivationImpl(
                drvPath, drv, StorePathSet{}, placementInputsFor(drvPath), buildMode, wantedOutputs);
        } catch (BuildError & e) {
            res.inner = static_cast<BuildResult::Failure &>(e);
        }
        results.emplace_back(std::move(res), req);
    }
    return results;
}

void NshBuilder::buildPaths(const std::vector<DerivedPath> & reqs, BuildMode buildMode)
{
    for (auto & result : buildPathsWithResults(reqs, buildMode))
        result.tryThrowBuildError();
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
