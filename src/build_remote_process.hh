#pragma once

#include <expected>

#include <nix/util/config-global.hh>
#include <nix/util/environment-variables.hh>
#include <nix/util/error.hh>
#include <nix/util/processes.hh>
#include <nix/util/serialise.hh>

#include "build_request.hh"

/// Spawns and manages the standard Nix build-remote hook process.
/// Used as a fallback when nix-scheduler-hook declines to handle a build.
class NixBuildRemoteProcess {
public:
  ~NixBuildRemoteProcess();

  NixBuildRemoteProcess(NixBuildRemoteProcess &&) = default;
  NixBuildRemoteProcess &operator=(NixBuildRemoteProcess &&) = default;

  // Non-copyable: owns unique process handle and pipe file descriptors
  NixBuildRemoteProcess(const NixBuildRemoteProcess &) = delete;
  NixBuildRemoteProcess &operator=(const NixBuildRemoteProcess &) = delete;

  static std::expected<NixBuildRemoteProcess, nix::Error>
  start(const BuildRequest<std::string> &request,
        nix::FdSource &parentStdin);

  std::expected<int, nix::Error> wait();

private:
  NixBuildRemoteProcess(nix::Pid pid, nix::Pipe pipe)
      : pid(std::move(pid)), pipe(std::move(pipe)) {};

  nix::Pid pid;
  /* Kept open for the lifetime of the child: build-remote's stdin must not
   * see EOF while the build is still running. */
  nix::Pipe pipe;
};
