#pragma once

#include "settings.hh"

#include <csignal>
#include <dlfcn.h>
#include <nix/util/fmt.hh>
#include <nix/util/file-system.hh>
#include <nix/store/store-api.hh>

#include <boost/algorithm/string/join.hpp>

/* Promote a scheduler client library to the global symbol scope.
 *
 * The NSH plugin is dlopen'd by nix with RTLD_LOCAL, so our library
 * dependencies (libpbs, libslurm) live in a local namespace. Both libraries
 * dlopen their own auth plugins at connect time (e.g. libauth_munge.so /
 * auth_munge.so), whose undefined symbols against the parent library must
 * resolve from the *global* scope — otherwise the auth plugin fails to load
 * and the connection is rejected (PBS surfaces this as PBSE_BADHOST 15010).
 * Re-dlopening the already-loaded library with RTLD_GLOBAL promotes it. */
static void promoteLibraryToGlobalScope(const char * soname)
{
    if (!dlopen(soname, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD))
        dlopen(soname, RTLD_NOW | RTLD_GLOBAL);
}

#define PATH_VAR "PATH=/run/current-system/sw/bin/:/usr/local/bin:/usr/bin:/bin:/nix/var/nix/profiles/default/bin"

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
