#include "settings.hh"

#include <format>

#include <nix/util/environment-variables.hh>
#include <nix/util/file-system.hh>
#include <nix/util/users.hh>
#include <nix/util/strings.hh>
#include <nix/util/error.hh>
#include <nix/util/executable-path.hh>
#include <nix/store/globals.hh>
#include <nix/store/global-paths.hh>

#define NIX_CONF_DIR "/etc/nix"

using std::expected;
using std::unexpected;

Settings::Settings()
    : confDir(nix::canonPath(nix::getEnvNonEmpty("NIX_CONF_DIR").value_or(NIX_CONF_DIR)))
    , userConfFiles(getUserConfigFiles())
{}
Settings ourSettings;

/* Layered like nix.conf: the system nsh.conf first, then the user files
   (lowest priority first), then the NSH_CONFIG environment variable.
   A missing file is skipped; an unreadable or unparsable one is an error
   (previously such files were silently ignored). */
expected<void, nix::Error> readConfig(nix::AbstractConfig & config)
{
    std::vector<std::filesystem::path> paths;
    paths.push_back(nix::nixConfDir() / "nsh.conf");

    auto files = ourSettings.userConfFiles;
    for (auto file = files.rbegin(); file != files.rend(); file++)
        paths.push_back(*file);

    for (auto & path : paths) {
        if (!nix::pathExists(path))
            continue;

        std::string contents;
        try {
            contents = nix::readFile(path);
        } catch (nix::Error & error) {
            error.addTrace({}, std::format("failed to read config from `{}'", path.string()));
            return unexpected(error);
        }

        try {
            config.applyConfig(contents, path.string());
        } catch (nix::Error & error) {
            error.addTrace({}, std::format("failed to parse config from `{}'", path.string()));
            return unexpected(error);
        }
    }

    auto confEnv = nix::getEnv("NSH_CONFIG");
    if (confEnv.has_value()) {
        try {
            config.applyConfig(confEnv.value(), "NSH_CONFIG");
        } catch (nix::Error & error) {
            error.addTrace({}, "failed to parse config from the NSH_CONFIG environment variable");
            return unexpected(error);
        }
    }

    return {};
}

expected<void, nix::Error> transferSettingsIn(nix::FdSource & source, nix::GlobalConfig & config)
{
    while (true) {
        unsigned int prefix;
        try {
            prefix = nix::readInt(source);
        } catch (nix::Error & error) {
            error.addTrace({}, "failed to read setting prefix integer");
            return unexpected(error);
        }
        if (!prefix) {
            break;
        }

        std::string name;
        try {
            name = nix::readString(source);
        } catch (nix::Error & error) {
            error.addTrace({}, "failed to read setting name");
            return unexpected(error);
        }

        std::string value;
        try {
            value = nix::readString(source);
        } catch (nix::Error & error) {
            error.addTrace({}, "failed to read setting value");
            return unexpected(error);
        }

        try {
            config.set(name, value);
        } catch (nix::Error & error) {
            error.addTrace({}, "failed to set setting value");
            return unexpected(error);
        }
    }

    return {};
}

expected<void, nix::Error> transferSettingsOut(nix::GlobalConfig & config, nix::FdSink & sink)
{
    std::map<std::string, nix::Config::SettingInfo> settings;
    config.getSettings(settings);
    for (const auto & [name, info] : settings) {
        try {
            sink << 1 << name << info.value;
        } catch (nix::Error & error) {
            error.addTrace(
                {}, std::format(
                        "failed to write setting key value pair ({} = {}) to sink",
                        name, info.value));
            return unexpected(error);
        }
    }
    try {
        sink << 0;
    } catch (nix::Error & error) {
        error.addTrace({},
                       "failed to write zero (end of settings indicator) to sink");
        return unexpected(error);
    }

    return {};
}

std::vector<std::filesystem::path> getUserConfigFiles()
{
    // Use the paths specified in NSH_USER_CONF_FILES if it has been defined
    auto confFiles = nix::getEnvOs(OS_STR("NIX_USER_CONF_FILES"));
    if (confFiles.has_value()) {
        return nix::ExecutablePath::parse(*confFiles).directories;
    }

    // Use the paths specified by the XDG spec
    std::vector<std::filesystem::path> files;
    auto dirs = nix::getConfigDirs();
    for (auto & dir : dirs) {
        files.insert(files.end(), dir / "nsh.conf");
    }
    return files;
}
