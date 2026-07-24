#pragma once

#include "settings.hh"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <thread>

#include <nix/util/fmt.hh>
#include <nix/util/file-system.hh>
#include <nix/util/signals.hh>
#include <nix/store/store-api.hh>

#include <boost/algorithm/string/join.hpp>

inline std::string genScript(nix::StorePath drvPath, std::string rootPath)
{
    auto nixCmdPrefix = ourSettings.remoteNixBinDir.get() != "" ? ourSettings.remoteNixBinDir.get() + "/" : "";

    std::string script =
        "#!/bin/sh\n"
        "while ! %1%nix-store --store '%2%' --query --hash %3%/%4% >/dev/null 2>&1; do sleep 0.1; done;"
        "%1%nix-store --store '%2%' --realise %3%/%4% --quiet --option system-features '%5%' --add-root %6%;"
        "rc=$?;"
        "echo '@nsh done' >&2;"
        "exit $rc";

    auto submitScript = ourSettings.submitScript.get();
    if (submitScript != "")
        script = nix::readFile(submitScript);

    return nix::fmt(
        script,
        nixCmdPrefix,
        ourSettings.remoteStore.get(),
        ourSettings.storeDir.get(),
        std::string(drvPath.to_string()),
        boost::algorithm::join(ourSettings.systemFeatures.get(), " "),
        rootPath
    );
}

/* Blocks SIGTERM for the enclosing scope, so that termination can't slip
   in between submitting a job and recording its id. The destructor restores
   the previous mask, so exception paths can't leave SIGTERM blocked. */
struct SignalBlocker
{
    SignalBlocker()
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &set, &saved))
            throw nix::SysError("blocking SIGTERM");
    }

    ~SignalBlocker()
    {
        pthread_sigmask(SIG_SETMASK, &saved, nullptr);
    }

    SignalBlocker(const SignalBlocker &) = delete;
    SignalBlocker & operator=(const SignalBlocker &) = delete;

    sigset_t saved;
};

/* Sleep that wakes up periodically so a received SIGTERM can interrupt the
   wait: nix::checkInterrupt() throws nix::Interrupted once the signal
   handler has flagged the interrupt. */
inline void interruptibleSleep(std::chrono::milliseconds duration)
{
    using namespace std::chrono_literals;
    while (duration > 0ms) {
        nix::checkInterrupt();
        auto chunk = std::min(duration, std::chrono::milliseconds(100));
        std::this_thread::sleep_for(chunk);
        duration -= chunk;
    }
    nix::checkInterrupt();
}
