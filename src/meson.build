cmake = import('cmake')

restclient_proj = cmake.subproject('restclient-cpp')
restclient_dep = restclient_proj.dependency('restclient-cpp')

json_dep = dependency('nlohmann_json')

boost_dep = dependency('boost', modules : ['fusion'])

pbs_dep = dependency('pbs')

compiler = meson.get_compiler('cpp')
slurm_dep = compiler.find_library('slurm', has_headers: ['slurm/slurm.h'])

# Full nix deps for the standalone build-hook executable.
nix_util_dep = dependency('nix-util', method: 'pkg-config')
nix_store_dep = dependency('nix-store', method: 'pkg-config')
nix_main_dep = dependency('nix-main', method: 'pkg-config')

# For the Nix plugin (.so, loaded via `plugin-files`), libnix{util,store,main}
# are already present in the host `nix` process at dlopen time, so we take
# their headers/compile flags but must NOT link them into the module
# (partial_dependency without link_args). Undefined nix symbols are resolved
# from the host at load.
nix_util_hdr = nix_util_dep.partial_dependency(compile_args : true, includes : true)
nix_store_hdr = nix_store_dep.partial_dependency(compile_args : true, includes : true)
nix_main_hdr = nix_main_dep.partial_dependency(compile_args : true, includes : true)

sched_sources = files(
    'settings.cc',
    'slurm.cc',
    'pbs.cc',
    'slurm-native.cc',
)

common_deps = [
    restclient_dep,
    json_dep,
    boost_dep,
    pbs_dep,
    slurm_dep,
]

# Build-hook mode: standalone executable, links nix libs.
executable('nsh', sched_sources + files('main.cc'),
    dependencies : common_deps + [
        nix_util_dep,
        nix_store_dep,
        nix_main_dep
    ])

# Plugin mode: shared module registering the nsh:// store scheme.
shared_module('nsh', sched_sources + files('nsh-store.cc'),
    name_prefix : '',
    dependencies : common_deps + [
        nix_util_hdr,
        nix_store_hdr,
        nix_main_hdr
    ])
