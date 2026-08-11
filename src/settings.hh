#pragma once

#include <filesystem>
#include <string>

#include <nix/util/configuration.hh>
#include <nix/util/types.hh>

struct Settings : public nix::Config
{
    Settings();

    std::filesystem::path confDir;
    std::vector<std::filesystem::path> userConfFiles;

    nix::Setting <std::string> jobScheduler {
        this,
        "slurm",
        "job-scheduler",
        "Which job scheduler to use, available choices are 'slurm', 'slurm-native', and 'pbs'."
    };

    nix::Setting<nix::StringSet> systems {
        this,
        {"x86_64-linux"},
        "systems",
        "The system types of this cluster, jobs requiring a different system will not be routed to the scheduler."
    };

    nix::Setting<nix::StringSet> systemFeatures {
        this,
        {"nsh"},
        "system-features",
        "Optional system features supported by the machines in the cluster. The default value is 'nsh'. Can be used to force derivations to build only via nix-scheduler-hook by adding 'nsh' as a required system feature."
    };

    nix::Setting<nix::StringSet> mandatorySystemFeatures {
        this,
        {},
        "mandatory-system-features",
        "System features that the derivations must require in order to be built on the cluster."
    };

    nix::Setting<std::string> storeDir {
        this,
        "/nix/store",
        "store-dir",
        "The logical remote Nix store directory. Only change this if you know what you're doing."
    };

    nix::Setting<std::string> remoteStore {
        this,
        "auto",
        "remote-store",
        "The store URL to be used on the remote machine. Should be set to 'auto' if using the nix-daemon."
    };

    nix::Setting<std::string> remoteNixBinDir {
        this,
        "",
        "remote-nix-bin-dir",
        "Path to the Nix bin directory to use on the remote system. This should be a shared location on your cluster. Useful for when your cluster does not have Nix installed."
    };

    nix::Setting<bool> collectGarbage {
        this,
        false,
        "collect-garbage",
        "Run nix store gc on the remote-store after each job completes."
    };

    nix::Setting<std::filesystem::path> submitScript {
        this,
        "",
        "submit-script",
        "Path to a file containing the actual script submitted to the scheduler, normally you shouldn't need to change this."
    };

    nix::Setting<std::string> submitEnv {
        this,
        "[\"PATH=/run/current-system/sw/bin/:/usr/local/bin:/usr/bin:/bin:/nix/var/nix/profiles/default/bin\"]",
        "submit-env",
        "JSON list of VAR=value strings representing the environment of the job."
    };

    nix::Setting<std::filesystem::path> submitDir {
        this,
        "/tmp",
        "submit-dir",
        "Working directory for the job. Only applies to the slurm schedulers."
    };

    nix::Setting<unsigned> sshPort {
        this,
        22,
        "ssh-port",
        "SSH port for connecting to the remote cluster nodes."
    };

    nix::Setting<std::string> sshUser {
        this,
        "",
        "ssh-user",
        "SSH user for connecting to the remote cluster nodes."
    };

    nix::Setting<bool> earlyAccept {
        this,
        false,
        "early-accept",
        "Accept the build before submitting the job, allows submission of more than one pending job at once at the cost of losing the ability to retry."
    };

    nix::Setting<bool> remoteBuilding {
        this,
        false,
        "remote-building",
        "Build over SSH rather than as part of the job script. Avoids copying the entire derivation closure to the remote. The job script just becomes a reservation system and will exit once the outputs exist. The SSH user must be a trusted user on the remotes."
    };

    nix::Setting<bool> slurmBatchStateUpdate {
        this,
        false,
        "slurm-batch-state-update",
        "Perform state updating in batches from a single build hook instance, rather than each instance individually querying the endpoint."
    };

    nix::Setting<std::string> slurmConf {
        this,
        "",
        "slurm-conf",
        "Path to slurm.conf, used by the slurm-native scheduler implementation. If unset, Slurm will attempt to locate it automatically."
    };

    nix::Setting <std::string> slurmStateDir {
        this,
        "",
        "slurm-state-dir",
        "Where to store temporary files on the cluster that are used during execution. It is recommended to use a location in your home directory for security reasons."
    };

    nix::Setting<std::string> slurmApiHost {
        this,
        "localhost",
        "slurm-api-host",
        "Hostname or address of the Slurm REST API endpoint."
    };

    nix::Setting<unsigned int> slurmApiPort {
        this,
        6820,
        "slurm-api-port",
        "Port to use for the Slurm REST API endpoint."
    };

    nix::Setting<std::string> slurmJwtToken {
        this,
        "",
        "slurm-jwt-token",
        "JWT token for authentication to the Slurm REST API."
    };

    nix::Setting<unsigned int> slurmApiTimeout {
        this,
        60,
        "slurm-api-timeout",
        "Timeout in seconds for Slurm REST API requests, protecting against stalled connections. Set to 0 to disable."
    };

    nix::Setting<std::string> slurmExtraJobSubmissionParams {
        this,
        {},
        "slurm-extra-submission-params",
        "Extra parameters to set in the /job/submit API request, as a JSON dictionary that will be merged with the 'job' value."
    };

    nix::Setting<std::string> slurmSystemParams {
        this,
        {},
        "slurm-system-params",
        "Extra parameters to set in the /job/submit API request on a per-system basis. JSON dictionary mapping systems to a dictionary that will be merged with the 'job' value."
    };

    nix::Setting<std::string> slurmFeatureParams {
        this,
        {},
        "slurm-feature-params",
        "Extra parameters to set in the /job/submit API request on a per-feature basis. JSON dictionary mapping systems to a dictionary that will be merged with the 'job' value."
    };

    nix::Setting<std::string> pbsHost {
        this,
        "",
        "pbs-host",
        "Hostname or address of the host running the PBS server."
    };

    nix::Setting<unsigned int> pbsPort {
        this,
        15001,
        "pbs-port",
        "Port that the PBS server is listening on."
    };
};

void loadConfFile(nix::AbstractConfig & config);

std::vector<std::filesystem::path> getUserConfigFiles();

extern Settings ourSettings;
