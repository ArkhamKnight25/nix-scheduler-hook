# Nix Scheduler Hook

This is a build hook that allows Nix builds to be forwarded to clusters running a job scheduler by submitting each build as its own job. It assumes that you can access the cluster nodes over SSH, e.g. `ssh <address>`, so you will amost certainly need to configure an SSH key for your system's root user. SSH is used for copying the dependencies and results to and from the job host, and for streaming the build log. Settings are managed with an `nsh.conf` file in your Nix configuration directory, e.g. `/etc/nix/nsh.conf` or `~/.config/nix/nsh.conf`, using the same `key = value` format that `nix.conf` uses. Note that if you installed Nix in multi-user mode (daemon) and wish to use a configuration under a user account, you should use `/root/.config/nix/nsh.conf`.

General settings:

- `job-scheduler`: Which job scheduler to use, available choices are 'slurm', 'slurm-native', and 'pbs'. Default: `slurm`.
- `systems`: The system types of this cluster, jobs requiring a different system will not be routed to the scheduler. Default: `x86_64-linux`.
- `system-features`: Optional system features supported by the machines in the cluster. Can be used to force derivations to build only via nix-scheduler-hook by adding 'nsh' as a required system feature. Default: `nsh`.
- `mandatory-system-features`: System features that the derivations must require in order to be built on the cluster. Default: (empty).
- `store-dir`: The logical remote Nix store directory. Only change this if you know what you're doing. Default: `/nix/store`.
- `remote-store`: The store URL to be used on the remote machine. See: [https://nix.dev/manual/nix/latest/store/types/](https://nix.dev/manual/nix/latest/store/types/). Default: `auto`.
- `remote-nix-bin-dir`: Path to the Nix bin directory to use on the remote system. This should be a shared location on your cluster. Useful for when your cluster does not have Nix installed (see below).
- `collect-garbage`: Run `nix-store --gc` on the `remote-store` after each job completes. Default: `false`.
- `submit-script`: Path to a file containing the actual script submitted to the scheduler, normally you shouldn't need to change this. See the section **Modifying the Submit Script** below.
- `ssh-port`: SSH port for connecting to the remote cluster nodes. Default: `22`.
- `ssh-user`: SSH user for connecting to the remote cluster nodes. If unset, it will be as if the user component was not specified, falling back to the relevant SSH config.
- `early-accept`: Accept the build before submitting the job, allows submission of more than one pending job at once at the cost of losing the ability to retry. Default: `false`.
- `remote-building`: Build over SSH rather than as part of the job script. Avoids copying the entire derivation closure to the remote. The job script just becomes a reservation system and will exit once the outputs exist. The SSH user must be a trusted user on the remotes.
- `candidate-nodes`: Ordered, whitespace-separated list of scheduler node names eligible for input-aware placement (see below). Default: (empty, disabled).
- `node-store-addresses`: Optional JSON dictionary mapping a scheduler node name from `candidate-nodes` to the host address used to query its Nix store over SSH, for clusters where the scheduler's node name is not a resolvable address, e.g. `{"gpu01": "gpu01.cluster.internal"}`. Nodes not listed use their scheduler name as the address. Default: (empty).

## Input-Aware Placement

When NSH runs as a Nix store plugin (the `nsh://` store), Nix hands it the build's required input closure *before* the job is submitted. If `candidate-nodes` is set, NSH uses this to choose the compute node instead of letting the scheduler place the job blindly:

1. For every node in `candidate-nodes`, NSH connects to the node's Nix store over SSH (`ssh-ng`, reusing `ssh-user`, `ssh-port`, `remote-store` and `remote-nix-bin-dir`) and asks which of the required input paths are already valid there. This happens before job allocation, so every candidate store must be queryable up front; nothing is copied at this stage.
2. Each node is scored by the number of required input paths it already holds.
3. The highest-scoring node wins; ties go to the node listed earliest in `candidate-nodes`. The job is then pinned to that node (Slurm REST: `required_nodes`; Slurm native: `job_desc_msg_t.req_nodes`; PBS: `Resource_List.select = 1:host=<node>`), and any inputs it is still missing are copied as usual once the job is allocated.
4. Fallback: if `candidate-nodes` is empty, the build has no inputs, every candidate scores zero, or every candidate store is unreachable, the job is submitted without a node preference and the scheduler places it normally. A single unreachable candidate is skipped with a warning and never fails the build.

The chosen node and per-node scores are logged (`NSH: input-aware scheduling: selected node ...`). A user-supplied node request in the extra submission parameters (`required_nodes` for Slurm REST, `select`/`nodes`/`host` in `pbsResources` for PBS) conflicts with input-aware placement and is rejected with an error rather than silently merged.

Input-aware placement only applies in plugin mode: the legacy build-hook protocol (see **Installation**) reveals the input list only after the hook has accepted the build, so hook-mode submissions keep the scheduler's normal placement. Both modes remain supported.

## Whole-Graph Builds (`--store nsh://`)

The `nsh://` store can be used in two ways, with different job granularity:

- **As a build machine** (`nix.buildMachines` with a `nsh://` storeUri): nix's own scheduler walks the build graph and dispatches each derivation separately, so every derivation becomes its own scheduler job and independent parts of the graph run in parallel across the cluster. This is the primary mode, and the one input-aware placement was designed around: each derivation is a fresh placement decision.
- **As the top-level store** (`nix build --store 'nsh://'`): nix hands NSH the requested goal directly, with no per-derivation fan-out. NSH submits **one scheduler job** for the requested derivation and copies its derivation closure to the assigned node; the job realises the derivation there, building any still-missing dependencies inside the same job. Only the requested outputs are copied back to the local store; intermediate outputs remain in the node's store, where later input-aware placement decisions can find them.

Whole-graph mode fits reserving a single node to build a closure end-to-end. Compared to the build-machine flow it trades away:

- Cluster-level parallelism: the graph builds on one node (with that node's local parallelism).
- Per-derivation gating: only the requested derivation's `system` and `requiredSystemFeatures` shape the job; its dependencies build on the node unvetted, so a dependency needing a different system simply fails inside the job.
- Failure attribution and log separation: a failing dependency fails the whole job, and the build log is a single stream for the entire graph.

Input-aware placement still works in whole-graph mode, scored on what is statically known: the derivation's input sources and the known output paths of its direct dependencies. A node that already holds parts of the graph from earlier builds wins; dependencies that exist nowhere yet contribute nothing to any score.

## Supported Job Schedulers

### Slurm

Slurm is supported through its [REST API](https://slurm.schedmd.com/rest.html). This requires `slurmrestd` to be running on the cluster and for you to have a valid JWT token for your account. At a minimum, you should set `slurm-jwt-token` in `nsh.conf` to your JWT token.

The current settings available for Slurm are:

- `slurm-state-dir` (required): Where to store temporary files on the cluster that are used during execution. It is recommended to use a location in your home directory for security reasons.
- `slurm-api-host`: Hostname or address of the Slurm REST API endpoint. Default: `localhost`.
- `slurm-api-port`: Port to use for the Slurm REST API endpoint. Default: `6820`.
- `slurm-jwt-token` (required if using Slurm): JWT token for authentication to the Slurm REST API.
- `slurm-extra-submission-params`: Extra parameters to set in the `/job/submit` API request, as a JSON dictionary that will be merged with the 'job' value in the [`job_submit_req`](https://slurm.schedmd.com/rest_api.html#v0.0.44_job_submit_req) object.
- `slurm-system-params`: Extra parameters to set in the /job/submit API request on a per-system basis. JSON dictionary mapping systems to a dictionary that will be merged with the 'job' value. Takes precedence over `slurm-extra-submission-params`. Example: `{"x86_64-linux": {"constraints": "x86"}, "aarch64-linux": {"constraints": "arm"}}`.
- `slurm-feature-params`: Extra parameters to set in the /job/submit API request on a per-feature basis. JSON dictionary mapping systems to a dictionary that will be merged with the 'job' value. Takes precedence over `slurm-extra-submission-params` and `slurm-system-params`. Example: `{"gpu": {"constraints": "gpu"}}`.
- `submit-env`: JSON list of VAR=value strings representing the environment of the job. Default: `[PATH=/run/current-system/sw/bin/:/usr/local/bin:/usr/bin:/bin:/nix/var/nix/profiles/default/bin]`.
- `submit-dir`: Working directory for the job. Only applies to the slurm schedulers. Default: `/tmp`.
- `slurm-batch-state-update`: Perform state updating in batches from a single build hook instance, rather than each instance individually querying the endpoint. Default: `false`.

A basic merge is performed on the `-params` values, with special handling for the `constraints` string value to ensure it is also merged with `&`. JSON objects are merged recursively, top-level arrays are concatenated, and other values are overwritten according to the order of precedence.

Extra job parameters to control things like required CPU count and memory (in megabytes) can also be specified on a per-derivation basis. For Slurm, this can be set in the `extraSlurmParams` attribute of a derivation, and it functions exactly like the `slurm-extra-submission-params` setting, but takes precedence over it. For example:

```nix
runCommand "myjob" {
  extraSlurmParams = builtins.toJSON {
    cpus_per_task = 4;
    memory_per_node = {
      set = true;
      number = 1024;
    };
  };
} ''
echo "Hello Slurm!" > $out
''
```

If `slurmrestd` is not available, you can use the 'slurm-native' scheduler instead, which uses libslurm. The settings available are:

- `slurm-state-dir` (required): Where to store temporary files on the cluster that are used during execution. It is recommended to use a location in your home directory for security reasons.
- `slurm-conf`: Path to slurm.conf. If unset, Slurm will attempt to locate it automatically.

It is suggested to use `slurm-native` on a machine that has already been configured as a Slurm submit host, so that the proper configuration and authentication mechanisms are already in place. You can use `nixpkgs#nixStatic` (see below) to submit your Nix jobs with NSH from a login node without Nix installed.

Using Slurm through the REST API allows the most flexibility with specifying job parameters. When using the native version, the following job constraints can be specified on a per-derivation basis through the `slurmNativeConstraints` attribute:

- `cpus`: The number of CPUs required by the job.
- `memPerNode`: The minimum real memory in megabytes required for the node the job runs on.
- `memPerCPU`: The minimum real memory in megabytes required for each CPU. Mutually exclusive with `memPerNode`.

Example:

```nix
runCommand "myjob" {
  slurmNativeConstraints = builtins.toJSON {
    cpus = 4;
    memPerNode = 8192;
  };
} ''
echo "Hello Slurm Native!" > $out
''
```

### PBS

PBS is supported through libpbs, so you may have to recompile NSH against an older version depending on what your cluster is running.

The current settings available for PBS are:

- `pbs-host`: Hostname or address of the host running the PBS server. Default: `PBS_SERVER` value from `pbs.conf`.
- `pbs-port`: Port that the PBS server is listening on. Default: `15001`.

If `pbs-host` is left unspecified, values for both the host and port are taken from `pbs.conf`.

Job resources can be specified on a per-derivation basis via the `pbsResources` derivation attribute. All values should be strings. For example:

```nix
runCommand "myjob" {
  pbsResources = builtins.toJSON {
    ncpus = "4";
    mem = "1gb";
  };
} ''
echo "Hello PBS!" > $out
''
```

## Installation

NSH is available in nixpkgs as `nix-scheduler-hook` as of [8ef2f76](https://github.com/NixOS/nixpkgs/commit/8ef2f769e98b2e59ed4affdb42544285626eb605).

Edit your `nix.conf` and set `build-hook = /path/to/nix-scheduler-hook/bin/nsh` (e.g., on non-NixOS, install it like you would any other package and use `/home/you/.nix-profile/bin/nsh` or `/nix/var/nix/profiles/default/bin`). On NixOS, you can do `nix.settings.build-hook = ${pkgs.nix-scheduler-hook}/bin/nsh`.

## Fallback to Normal Build Hook

If NSH would decline a build, instead of simply declining, it attempts to launch the normal build hook and forwards it the build details. The normal build hook will then either accept or decline the build.

## Usage on Clusters Without Nix Installed

It is possible to use this hook to submit jobs to clusters without Nix installed, it just requires a small amount of one-time setup.

Start on a machine that *does* have Nix installed and that can connect to a cluster login node to access your home directory. Download the package `nixStatic` on this machine, being sure to specify a system matching the cluster. This is especially important if, for example, you are on a Mac but your cluster is running Linux.

```bash
nix build --system x86_64-linux nixpkgs#nixStatic
```

Next, copy Nix Static from your machine to your home directory on the cluster (or a shared location you have access to).

```bash
scp -r ./result login.example.com:/home/you/nix-static
```

Configure your `nsh.conf` file with the following settings:

```conf
remote-store = /local/store
remote-nix-bin-dir = /home/you/nix-static/bin
```

This will cause NSH to invoke the Nix Static binaries on the remote machine when performing a build and copying dependencies and results. See below notes on best practices for setting `remote-store`.

## Managing Which Derivations Get Built on the Cluster

The `system-features` and `mandatory-system-features` configuration settings can be used to filter which derivations are sent to the cluster and which are built through other means (fallback or locally). `nsh` is the default value of the `system-features` setting. If you want to force a derivation to build on your cluster, you can add `nsh` as a `requiredSystemFeatures`.

```nix
runCommand "myjob" {
  requiredSystemFeatures = [ "nsh" ];
} ''
echo "Hello Slurm!" > $out
''
```

By default, all derivations are opportunistically sent to NSH to be built on the cluster. If you want to prevent all but certain derivations from building on your cluster, you can additionally make use of the `mandatory-system-features` NSH setting. By default it is empty. If you set it to `nsh`, this will make all derivations which don't have `nsh` as a `requiredSystemFeatures` (e.g., everything in nixpkgs) build either through the fallback (regular) remote building hook or locally, and not on the cluster. This allows you to be selective about what gets sent to the cluster and what uses your own local resources for building.

## Modifying the Submit Script

You can set the `submit-script` configuration option to a file path containing a script template for job submission. This can be useful if you need to e.g. launch Nix inside of a container instead of running natively on the system. The following is the default script:

```bash
#!/bin/sh
while ! %1%nix-store --store '%2%' --query --hash %3%/%4% >/dev/null 2>&1; do sleep 0.1; done;
%1%nix-store --store '%2%' --realise %3%/%4% --quiet --option system-features '%5%' --add-root %6%;
rc=$?;
echo '@nsh done' >&2;
exit $rc
```

Note the use of format specifiers to pass various settings:

1. `remote-nix-bin-dir`, if set
2. `remote-store`
3. `store-dir`
4. Derivation store path to build
5. `system-features`
6. Garbage Collector root path

## Known Limitations

Because of https://github.com/NixOS/nix/issues/14760, it is impossible for NSH to clean up any outstanding jobs if the build gets manually cancelled, e.g. with ctrl-c. This has been fixed in upstream as of [5d7c091](https://github.com/NixOS/nix/commit/5d7c09105991c08ecf2bdfb9bba1ed8442c0a5d4).

Some recent versions of Nix do not respect the `build-hook` option in `nix.conf`, requiring you to pass NSH via `--option` instead. This issue has been fixed in upstream as of [0e3a620](https://github.com/NixOS/nix/commit/0e3a6203747b6c3c24dec34cb3df5b829bf47100).

It is not possible to set `nix.settings.build-hook` on NixOS when using Lix. The `nix.conf` validation step will fail complaining that `build-hook` is a deprecated setting. It is still possible to use NSH with Lix through `--option build-hook` on the command-line, although fallback to the regular build hook is broken.

It is [recommended](https://discourse.nixos.org/t/how-do-i-best-use-nix-to-create-a-development-environment-on-an-hpc-cluster-without-the-possibility-of-system-wide-installation/71096/2) to use a `remote-store` location that is *not* on a shared filesystem, for performance reasons and to not exhaust your file count quota. Note that using a location in `/tmp` will not work because Nix disallows stores to exist in world-writable locations. Using a location in `/run/user/<uid>/` is not recommended as it is possible (although unlikely) for the job to start immediately and complete before a store connection is established to the remote, leaving a small window of time during which the contents of `/run/user/<uid>/` could be cleaned up. This could happen if a prior build of the same derivation was interrupted, leaving the derivation file available in the store for the new job to use immediately. It is safest to use a location backed by a local disk, and to make use of the `collect-garbage = true` NSH option to clean up after every job.
