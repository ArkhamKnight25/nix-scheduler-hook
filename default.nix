{
  clangStdenv,
  lib,
  restclient-cpp,
  openpbs,
  slurm,
  nix,
  boost,
  curl,
  meson,
  cmake,
  ninja,
  pkg-config,
  nlohmann_json
}:
clangStdenv.mkDerivation {
  name = "nix-scheduler-hook";
  src = lib.sourceFilesBySuffices ./. [
    "meson.build"
    ".cc"
    ".hh"
  ];

  nativeBuildInputs = [
    meson
    cmake
    ninja
    pkg-config
  ];

  buildInputs = [
    boost
    curl
    nix.libs.nix-util
    nix.libs.nix-store
    nix.libs.nix-main
    nlohmann_json
    openpbs
    slurm
  ];

  postUnpack = ''
    mkdir $sourceRoot/subprojects
    cp -r ${restclient-cpp} $sourceRoot/subprojects/restclient-cpp
  '';

  installPhase = ''
    mkdir -p $out/bin
    mv src/nsh $out/bin
    mkdir -p $out/lib
    shopt -s extglob
    mv subprojects/restclient-cpp/librestclient_cpp.so!(*p) $out/lib
  '';
}
