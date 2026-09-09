#pragma once

#include <filesystem>
#include <memory>
#include <optional>

#include <nix/store/store-api.hh>
#include <nix/store/build.hh>
#include <nix/store/store-registration.hh>

#include "settings.hh"
#include "scheduler/scheduler.hh"

namespace nix {

/* Configuration for the `nsh://` store.
 *
 * The old `nsh.conf` keys are folded into the store's query parameters,
 * e.g. `nsh://?job-scheduler=slurm&slurm-state-dir=/root/nsh`. Parsing is
 * delegated to the existing `Settings ourSettings` (see the constructor in
 * nsh-store.cc), so all backend code keeps reading `ourSettings`. */
struct NshStoreConfig : std::enable_shared_from_this<NshStoreConfig>, virtual StoreConfig
{
private:
    /* VTable anchor: mandatory so `NshStoreConfig`'s typeinfo is a strong
     * libstore-visible symbol and cross-.so `dynamic_cast` works. */
    void anchor() override;

public:
    NshStoreConfig(const std::filesystem::path & path, const Params & params);
    NshStoreConfig(const Params & params);

    static const std::string name()
    {
        return "Nix Scheduler Hook Store";
    }

    static std::string doc();

    static StringSet uriSchemes()
    {
        return {"nsh"};
    }

    /* The base implementation returns StoreReference::Auto, on which nix's
     * copy-progress messages hit unreachable() — every concrete store must
     * report a resolved reference. */
    StoreReference getReference() const override
    {
        return {
            .variant =
                StoreReference::Specified{
                    .scheme = *uriSchemes().begin(),
                },
        };
    }

    ref<Store> openStore() const override;
};

/* A build-through store. `nsh://` schedules the build on a compute node via
 * the configured `Scheduler` and lands the resulting outputs in a backing
 * store (the node-local default store, `openStore()`), whose read/write ops
 * this store delegates to so nix's `__build-remote` can copy outputs back. */
struct NshStore : public virtual Store
{
private:
    /* `Store::anchor` is a pure-virtual vtable anchor; defining it here
     * keeps this store's typeinfo a strong libstore-visible symbol so
     * cross-.so `dynamic_cast` works. */
    void anchor() override;

public:
    using Config = NshStoreConfig;

    ref<const Config> config;

    /* Real store where scheduled builds' outputs land and which serves all
     * read/write operations. */
    ref<Store> backing;

    NshStore(ref<const Config>);

    /* Hand nix a builder that offloads to the scheduler. */
    ref<Builder> getBuilder(std::shared_ptr<Store> evalStore = nullptr) override;

    /* Read ops: delegate to the backing store. */
    void queryPathInfoUncached(
        const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override;
    void queryRealisationUncached(
        const DrvOutput & id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override;
    std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) override;
    void narFromPath(const StorePath & path, Sink & sink) override;
    ref<SourceAccessor> getFSAccessor(bool requireValidPath) override;
    std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath & path, bool requireValidPath) override;

    /* Write ops: delegate to the backing store. */
    void addToStore(
        const ValidPathInfo & info, Source & narSource, RepairFlag repair, CheckSigsFlag checkSigs) override;
    StorePath addToStoreFromDump(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod,
        ContentAddressMethod hashMethod,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        RepairFlag repair,
        std::shared_ptr<const Provenance> provenance) override;
    void registerDrvOutput(const Realisation & output) override;

    /* We don't know whether we're trusted; assume we are (like `ssh://`) so
     * `__build-remote` uses the `buildDerivation` inputs overload. */
    std::optional<TrustedFlag> isTrustedClient() override
    {
        return std::nullopt;
    }
};

/* The `Builder` for `nsh://`, handed to nix via `NshStore::getBuilder()`.
 * It submits to the scheduler, copies inputs to the node, waits, and copies
 * outputs into the backing store. */
struct NshBuilder : public Builder
{
    NshStore & nshStore;
    std::shared_ptr<Store> evalStore;
    std::unique_ptr<Scheduler> scheduler;

    NshBuilder(NshStore & nshStore, std::shared_ptr<Store> evalStore);

    /* Build-machine flow (`__build-remote` with a `nsh://` storeUri): nix
     * primes `drv.inputSrcs` with the build's input closure before calling
     * (see build-remote.cc), so those paths are what gets copied to the
     * node and what drives input-aware placement. */
    BuildResult buildDerivation(const StorePath & drvPath, const BasicDerivation & drv, BuildMode buildMode) override;

    /* Whole-graph flow: when `nsh://` is the top-level store (`nix build
     * --store nsh://`), nix hands us the goal directly with no Worker
     * fan-out, so the job script realises the requested derivation AND its
     * still-missing dependencies on the node — the entire graph runs as a
     * single scheduler job. See "Whole-Graph Builds" in the README for the
     * trade-offs. */
    void buildPaths(const std::vector<DerivedPath> & reqs, BuildMode buildMode) override;
    std::vector<KeyedBuildResult>
    buildPathsWithResults(const std::vector<DerivedPath> & reqs, BuildMode buildMode) override;

    void ensurePath(const StorePath & path) override;
    void repairPath(const StorePath & path) override;

private:
    /* Shared implementation. `inputs` are the paths copied to the node
     * before the build; `placementInputs` are the paths scored for
     * input-aware node selection (identical to `inputs` in the
     * build-machine flow, statically derived in the whole-graph flow, where
     * intermediate outputs may not exist anywhere yet and so must not be
     * copied); `wantedOutputs` limits which outputs are waited on and
     * copied back.
     *
     * `remoteBuilding` makes the job a bare reservation and has us drive
     * the build over ssh-ng, so the derivation closure is not copied.
     * `inputs` must then cover everything the node's daemon needs, since
     * it gets the derivation inline. True of the build-machine flow, where
     * nix primes `drv.inputSrcs`; not of the whole-graph flow, which
     * passes an empty `inputs` and so always passes false. */
    BuildResult buildDerivationImpl(
        const StorePath & drvPath,
        const BasicDerivation & drv,
        const StorePathSet & inputs,
        const StorePathSet & placementInputs,
        BuildMode buildMode,
        const StringSet & wantedOutputs,
        bool remoteBuilding);

    /* Statically known store paths a whole-graph build will want on the
     * node: the derivation's input sources plus the known output paths of
     * its direct input derivations. Dependencies that exist nowhere yet
     * simply contribute nothing to any candidate's score. Best-effort. */
    StorePathSet placementInputsFor(const StorePath & drvPath);

    /* Source store for copying inputs/drv closure to the node. */
    ref<Store> srcStore();
    std::shared_ptr<Store> srcStoreCache;
};

} // namespace nix
