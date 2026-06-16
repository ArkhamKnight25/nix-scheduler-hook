#pragma once

#include <expected>
#include <string>

#include <nix/store/path.hh>
#include <nix/store/store-api.hh>
#include <nix/util/error.hh>
#include <nix/util/serialise.hh>

template <typename PathType = std::string> struct BuildRequest {
  std::string command;
  bool willingToBuildLocally;
  std::string system;
  nix::StringSet systemFeatures;
  PathType derivationPath;

  explicit BuildRequest(std::string command, bool willingToBuildLocally,
                        std::string system, nix::StringSet systemFeatures,
                        PathType derivationPath)
      : command(std::move(command)),
        willingToBuildLocally(std::move(willingToBuildLocally)),
        system(std::move(system)), systemFeatures(std::move(systemFeatures)),
        derivationPath(std::move(derivationPath)) {}

  /* These are only defined (as explicit specializations) for
   * PathType = std::string, the wire representation. No `requires`
   * clauses: clang mangles constrained declarations differently from
   * their unconstrained explicit specializations, breaking the link. */
  static std::expected<BuildRequest<std::string>, nix::Error>
  read(nix::FdSource &source);
  std::expected<void, nix::Error> send(nix::FdSink &sink) const;

  /// `availableSystems` is a set rather than a single system: one cluster
  /// can serve several system types (see the `systems` setting).
  std::expected<BuildRequest<nix::StorePath>, nix::Error>
  validate(nix::ref<nix::Store> store, nix::StringSet availableSystems,
           nix::StringSet availableSystemFeatures,
           nix::StringSet systemFeatureRequests) const;
};
