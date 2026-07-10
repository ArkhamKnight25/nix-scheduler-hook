#include "scheduler.hh"

#include <string>
#include <exception>

#include <slurm/slurm.h>
#include <slurm/slurm_errno.h>

#include <nix/store/path.hh>

struct SlurmNativeError : public std::runtime_error
{
    explicit SlurmNativeError(const std::string &fun) : std::runtime_error(nix::fmt("error when calling %s: %s", fun, slurm_strerror(errno))) {}
};

struct SlurmNativeConstraintError : public std::runtime_error
{
    explicit SlurmNativeConstraintError(const std::string &s) : std::runtime_error(s) {}
};

class SlurmNative : public Scheduler
{
    std::map<nix::StorePath, slurm_step_id_t> nativeJobIds;
public:
    SlurmNative();
    ~SlurmNative();
    void submit(nix::StorePath drvPath, const nix::BasicDerivation & drv, std::string system, nix::StringSet requiredFeatures, nix::StorePathSet wantedPaths);
    int waitForJobFinish(nix::StorePath);
};
