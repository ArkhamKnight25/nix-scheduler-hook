#pragma once

#include "settings.hh"

#include <csignal>
#include <nix/util/fmt.hh>
#include <nix/util/file-system.hh>
#include <nix/store/store-api.hh>

#include <boost/algorithm/string/join.hpp>

static std::string genScript(nix::StorePath drvPath, std::string rootPath)
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

static void blockSignals()
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &set, nullptr))
        throw nix::SysError("blocking SIGTERM");
}

static void unblockSignals()
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    if (pthread_sigmask(SIG_UNBLOCK, &set, nullptr))
        throw nix::SysError("unblocking SIGTERM");
}
