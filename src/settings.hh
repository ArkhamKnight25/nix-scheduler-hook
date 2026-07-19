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

    nix::Setting<nix::Strings> candidateNodes {
        this,
        {},
        "candidate-nodes",
        "Ordered list of scheduler node names eligible for input-aware placement. "
        "When set, NSH queries each node's Nix store for the build's required input "
        "closure before submitting, and pins the job to the node that already holds "
        "the most input paths. Earlier entries win ties. Empty (the default) "
        "disables input-aware placement and lets the scheduler place jobs normally. "
        "Each node's store must be reachable over SSH (see ssh-user, ssh-port, "
        "remote-store, remote-nix-bin-dir) before the job is allocated."
    };

    nix::Setting<std::string> nodeStoreAddresses {
        this,
        "",
        "node-store-addresses",
        "Optional JSON dictionary mapping a scheduler node name (as listed in "
        "candidate-nodes) to the host address used to query its Nix store over SSH, "
        "for clusters where the scheduler's node name is not a resolvable address. "
        "Nodes not listed use their scheduler name as the address."
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
