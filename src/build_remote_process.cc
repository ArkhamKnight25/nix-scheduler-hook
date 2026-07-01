#include <format>

#include "build_remote_process.hh"
#include "settings.hh"

using nix::FdSource;
using nix::StringSet;
using std::expected;
using std::optional;
using std::string;
using std::unexpected;
using std::filesystem::path;

namespace filesystem = std::filesystem;

// without these printError, etc. wouldn't work, since they assume that we
// are in the nix namespace
using nix::fmt;
using nix::logger;
using nix::lvlChatty;
using nix::lvlDebug;
using nix::lvlError;
using nix::lvlInfo;
using nix::lvlNotice;
using nix::lvlTalkative;
using nix::lvlVomit;
using nix::lvlWarn;

NixBuildRemoteProcess::~NixBuildRemoteProcess() {
  if (pid != -1) {
    try {
      pid.kill();
    } catch (nix::Error &error) {
      error.addTrace(
          {}, std::format(
                  "failure while killing nix remote builder process (pid = {})",
                  pid_t(pid)));
      nix::warn("[nsh] %s", error.what());
    }
  }
}

static expected<path, nix::Error> resolveNixBinary() {
  auto pathCandidate = nix::getEnvNonEmpty("NIX_BIN_DIR");
  if (pathCandidate.has_value() && filesystem::exists(*pathCandidate)) {
    return path(*pathCandidate) / "nix";
  }

  auto pathString = nix::getEnvNonEmpty("PATH");
  if (!pathString.has_value()) {
    return unexpected(
        nix::Error("NIX_BIN_DIR is empty or non-existend or doesn't have a "
                   "nix binary in it, PATH is empty or non-existend"));
  }

  auto paths = nix::tokenizeString<nix::Strings>(*pathString, ":");
  for (const auto &p : paths) {
    auto pathCandidate = path(p) / "nix";
    if (filesystem::exists(pathCandidate)) {
      return pathCandidate;
    }
  }

  return unexpected(
      nix::Error("NIX_BIN_DIR is empty or non-existend or doesn't have a nix "
                 "binary in it, PATH is non-empty but none of its components "
                 "have a nix binary in it"));
}

expected<void, nix::Error> execNixBuildRemote() {
  auto nixBinaryPath = resolveNixBinary();
  if (!nixBinaryPath.has_value()) {
    nixBinaryPath.error().addTrace({}, "failed to resolve nix binary");
    return unexpected(nixBinaryPath.error());
  }

  nix::Strings buildRemoteArgs{nixBinaryPath->filename().string(),
                               "__build-remote",
                               std::to_string(nix::verbosity)};
  if (execv(nixBinaryPath->native().c_str(),
            nix::stringsToCharPtrs(buildRemoteArgs).data()) != 0) {
    auto error = nix::Error(strerror(errno));
    error.addTrace({}, std::format("failed to exec `{} __build-remote {}`",
                                   nixBinaryPath->native(),
                                   std::to_string(nix::verbosity)));
    return unexpected(error);
  }

  return {};
}

expected<void, nix::Error> execNixBuildRemoteLegacy() {
  auto nixBinaryPath = resolveNixBinary();
  if (!nixBinaryPath.has_value()) {
    nixBinaryPath.error().addTrace({}, "failed to resolve nix binary");
    return unexpected(nixBinaryPath.error());
  }

  auto nixBuildRemotePath =
      filesystem::canonical(*nixBinaryPath).parent_path().parent_path() /
      "libexec" / "nix" / "build-remote";

  nix::Strings legacyArgs{nixBuildRemotePath.filename().string(),
                          std::to_string(nix::verbosity)};
  if (execv(nixBuildRemotePath.native().c_str(),
            nix::stringsToCharPtrs(legacyArgs).data()) != 0) {
    auto error = nix::Error(strerror(errno));
    error.addTrace({}, std::format("failed to exec `{} __build-remote {}`",
                                   nixBinaryPath->native(),
                                   std::to_string(nix::verbosity)));
    return unexpected(error);
  }

  return {};
}

expected<NixBuildRemoteProcess, nix::Error>
NixBuildRemoteProcess::start(const BuildRequestHeader &request,
                             FdSource &parentStdin) {
  nix::Pipe pipe;
  try {
    pipe.create();
  } catch (nix::Error &error) {
    error.addTrace({}, "failed to create pipe to nix remote builder process");
    return unexpected(error);
  }

  auto execNixBuildRemoteFull = [&]() {
    pipe.writeSide = -1;
    if (dup2(pipe.readSide.get(), STDIN_FILENO) == -1)
      throw nix::Error("failed to redirect pipe to build-remote stdin");

    auto resultStandard = execNixBuildRemote();
    auto _ = execNixBuildRemoteLegacy();

    resultStandard.error().addTrace(
        {}, "failed to execute nix remote builder both with `nix "
            "__build-remote` and the legacy `libexec/nix/build-remote` "
            "(error trace "
            "follows the `nix __build_remote` failure)");
    throw resultStandard;
  };

  pid_t pid;
  try {
    pid = nix::startProcess(execNixBuildRemoteFull);
  } catch (nix::Error &error) {
    error.addTrace({}, "failed to start nix remote builder process");
    return unexpected(error);
  }
  // construct the owner here, so the destructor kills the child if any of
  // the transfers below fail
  auto process = NixBuildRemoteProcess(nix::Pid(pid), std::move(pipe));

  process.pipe.readSide = -1;
  nix::FdSink sink(process.pipe.writeSide.get());

  auto result = transferSettingsOut(nix::globalConfig, sink);
  if (!result.has_value()) {
    result.error().addTrace(
        {}, "failed to transfer settings through pipe to nix remote builder");
    return unexpected(result.error());
  }

  result = request.send(sink);
  if (!result.has_value()) {
    result.error().addTrace({}, "failed to send build request");
    return unexpected(result.error());
  }
  try {
    sink.flush();
  } catch (nix::Error &error) {
    error.addTrace({},
                   "failed to flush pipe to the nix remote builder process");
    return unexpected(error);
  }

  // Forward inputs and wanted outputs from parent
  nix::StringSet inputs;
  try {
    inputs = nix::readStrings<nix::StringSet>(parentStdin);
  } catch (nix::Error &error) {
    error.addTrace(
        {}, "failed to read inputs to forward from calling nix build process");
    return unexpected(error);
  }

  nix::StringSet wantedOutputs;
  try {
    wantedOutputs = nix::readStrings<nix::StringSet>(parentStdin);
  } catch (nix::Error &error) {
    error.addTrace({}, "failed to read wanted outputs to forward from calling "
                       "nix build process");
    return unexpected(error);
  }

  try {
    sink << inputs;
  } catch (nix::Error &error) {
    error.addTrace({}, "failed to write inputs to forward from calling nix "
                       "build process to nix remote builder pipe");
    return unexpected(error);
  }

  try {
    sink << wantedOutputs;
  } catch (nix::Error &error) {
    error.addTrace({},
                   "failed to write wanted outputs to forward from calling nix "
                   "build process to nix remote builder pipe");
    return unexpected(error);
  }

  try {
    sink.flush();
  } catch (nix::Error &error) {
    error.addTrace({},
                   "failed to flush pipe to the nix remote builder process");
    return unexpected(error);
  }

  return process;
}

expected<int, nix::Error> NixBuildRemoteProcess::wait() {
  int returnCode;
  try {
    returnCode = this->pid.wait();
  } catch (nix::Error &error) {
    error.addTrace({}, "failed to wait for nix remote builder process");
    return unexpected(error);
  }
  return returnCode;
}
