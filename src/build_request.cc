#include <format>
#include <ranges>
#include <string>

#include <nix/store/store-api.hh>

#include "build_request.hh"

using nix::Error;
using nix::ref;
using nix::Store;
using nix::StringSet;
using std::expected;
using std::string;
using std::unexpected;

template <>
expected<BuildRequest<string>, Error>
BuildRequest<string>::read(nix::FdSource &source) {
  std::string command;
  try {
    command = nix::readString(source);
  } catch (Error &error) {
    error.addTrace({}, "failed to read command");
    return unexpected(error);
  }

  bool willingToBuildLocally;
  try {
    willingToBuildLocally = nix::readInt(source) > 0;
  } catch (Error &error) {
    error.addTrace({},
                   "failed to read amWilling (willing to build locally) flag");
    return unexpected(error);
  }

  string system;
  try {
    system = nix::readString(source);
  } catch (Error &error) {
    error.addTrace({}, "failed to read system request");
    return unexpected(error);
  }

  std::string derivationPath;
  try {
    derivationPath = nix::readString(source);
  } catch (Error &error) {
    error.addTrace({}, "failed to read derivation path");
    return unexpected(error);
  }

  nix::StringSet systemFeatures;
  try {
    systemFeatures = nix::readStrings<nix::StringSet>(source);
  } catch (Error &error) {
    error.addTrace({}, "failed to read required system features");
    return unexpected(error);
  }

  return BuildRequest(command, willingToBuildLocally, system, systemFeatures,
                      derivationPath);
}

template <>
expected<void, nix::Error>
BuildRequest<std::string>::send(nix::FdSink &sink) const {
  try {
    sink << this->command;
  } catch (nix::Error &error) {
    error.addTrace({}, "failed to write command");
    return unexpected(error);
  }

  try {
    sink << this->willingToBuildLocally;
  } catch (nix::Error &error) {
    error.addTrace({},
                   "failed to write amWilling (willing to build locally) flag");
    return unexpected(error);
  }

  try {
    sink << this->system;
  } catch (nix::Error &error) {
    error.addTrace({}, "failed to write system request");
    return unexpected(error);
  }

  try {
    sink << this->derivationPath;
  } catch (nix::Error &error) {
    error.addTrace({}, "failed to write derivation path");
    return unexpected(error);
  }

  try {
    sink << this->systemFeatures;
  } catch (nix::Error &error) {
    error.addTrace({}, "failed to write requested system features");
    return unexpected(error);
  }

  return {};
}

template <>
expected<BuildRequest<nix::StorePath>, Error>
BuildRequest<string>::validate(ref<Store> store, StringSet availableSystems,
                               StringSet availableSystemFeatures,
                               StringSet systemFeatureRequests) const {
  if (this->command != "try") {
    return unexpected(Error("expected command to be 'try', got `{}`", command));
  }

  if (!availableSystems.contains(this->system)) {
    auto availableSystemsString =
        availableSystems | std::views::join_with(std::string_view{", "}) |
        std::ranges::to<std::string>();
    return unexpected(Error(std::format(
        "derivation requests to build on {}, but the available systems are {}",
        this->system, availableSystemsString)));
  }

  StringSet unavailableFeatures;
  std::ranges::set_difference(
      this->systemFeatures, availableSystemFeatures,
      std::inserter(unavailableFeatures, unavailableFeatures.end()));
  if (!unavailableFeatures.empty()) {
    auto unavailableFeaturesString =
        unavailableFeatures | std::views::join_with(std::string_view{", "}) |
        std::ranges::to<std::string>();
    return unexpected(
        Error(std::format("derivation requests the following system features "
                          "which are unavailable: {}",
                          unavailableFeaturesString)));
  }

  StringSet unsatisfiedFeatureRequests;
  std::ranges::set_difference(systemFeatureRequests, this->systemFeatures,
                              std::inserter(unsatisfiedFeatureRequests,
                                            unsatisfiedFeatureRequests.end()));
  if (!unsatisfiedFeatureRequests.empty()) {
    auto unsatisfiedFeatureRequestsString =
        unsatisfiedFeatureRequests |
        std::views::join_with(std::string_view{", "}) |
        std::ranges::to<std::string>();
    return unexpected(
        Error(std::format("derivation does not request the following mandatory "
                          "system feature requests: {}",
                          unsatisfiedFeatureRequestsString)));
  }

  /* No dummy StorePath: its constructor validates the name and would
   * throw on a default of "". */
  try {
    auto storePath = store->parseStorePath(derivationPath);
    return BuildRequest<nix::StorePath>(
        this->command, this->willingToBuildLocally, this->system,
        this->systemFeatures, std::move(storePath));
  } catch (Error &error) {
    error.addTrace({}, "failed to parse derivation path");
    return unexpected(error);
  }
}
