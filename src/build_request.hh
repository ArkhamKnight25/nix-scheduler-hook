#pragma once

#include <expected>
#include <string>

#include <nix/store/derivations.hh>
#include <nix/store/path.hh>
#include <nix/store/store-api.hh>
#include <nix/util/error.hh>
#include <nix/util/serialise.hh>

class BuildRequestHeaderWithStoreInfo;

/* The hook request as it arrives on the wire. The build inputs and wanted
 * outputs are NOT part of this: nix only sends them after the hook has
 * accepted the build, so they are read separately at accept time. */
class BuildRequestHeader final {
public:
  std::string command;
  bool willingToBuildLocally;
  std::string system;
  nix::StringSet systemFeatures;
  std::string derivationPath;

  explicit BuildRequestHeader(std::string command,
                                    bool willingToBuildLocally,
                                    std::string system,
                                    nix::StringSet systemFeatures,
                                    std::string derivationPath)
      : command{std::move(command)},
        willingToBuildLocally{willingToBuildLocally}, system{std::move(system)},
        systemFeatures{std::move(systemFeatures)},
        derivationPath{std::move(derivationPath)} {}

  static auto read(nix::FdSource &source)
      -> std::expected<BuildRequestHeader, nix::Error>;
  auto send(nix::FdSink &sink) const -> std::expected<void, nix::Error>;

  /// `availableSystems` is a set rather than a single system: one cluster
  /// can serve several system types (see the `systems` setting).
  auto validate(const nix::StringSet &availableSystems,
                const nix::StringSet &availableSystemFeatures,
                const nix::StringSet &systemFeatureRequests) const
      -> std::expected<void, nix::Error>;

  /// Parse the derivation path against `store` and read the derivation,
  /// producing the full request the schedulers consume.
  auto addStoreInfo(nix::ref<nix::Store> store) const
      -> std::expected<BuildRequestHeaderWithStoreInfo, nix::Error>;

  // move
  BuildRequestHeader(BuildRequestHeader &&) noexcept = default;
  BuildRequestHeader &operator=(BuildRequestHeader &&) noexcept = default;

  // copy
  BuildRequestHeader(const BuildRequestHeader &) = delete;
  BuildRequestHeader &operator=(const BuildRequestHeader &) = delete;
};

/* A request whose derivation path has been parsed and whose derivation has
 * been read from the store. */
class BuildRequestHeaderWithStoreInfo final {
public:
  std::string command;
  bool willingToBuildLocally;
  std::string system;
  nix::StringSet systemFeatures;
  nix::StorePath derivationPath;
  nix::Derivation derivation;

  explicit BuildRequestHeaderWithStoreInfo(std::string command, bool willingToBuildLocally,
                        std::string system, nix::StringSet systemFeatures,
                        nix::StorePath derivationPath,
                        nix::Derivation derivation)
      : command{std::move(command)},
        willingToBuildLocally{willingToBuildLocally}, system{std::move(system)},
        systemFeatures{std::move(systemFeatures)},
        derivationPath{std::move(derivationPath)},
        derivation{std::move(derivation)} {}

  // move
  BuildRequestHeaderWithStoreInfo(BuildRequestHeaderWithStoreInfo &&) noexcept = default;
  BuildRequestHeaderWithStoreInfo &operator=(BuildRequestHeaderWithStoreInfo &&) noexcept = default;

  // copy
  BuildRequestHeaderWithStoreInfo(const BuildRequestHeaderWithStoreInfo &) = delete;
  BuildRequestHeaderWithStoreInfo &operator=(const BuildRequestHeaderWithStoreInfo &) = delete;
};
