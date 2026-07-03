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
  # Filtered source: only the files meson consumes, so edits to docs, CI,
  # or the nix files themselves don't change the source hash and force
  # rebuilds of the package and every VM test image built from it.
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../meson.build
      (lib.fileset.fileFilter (file: file.hasExt "cc" || file.hasExt "hh") ../src)
    ];
  };

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

  doCheck = true;

  postUnpack = ''
    mkdir $sourceRoot/subprojects
    cp -r ${restclient-cpp} $sourceRoot/subprojects/restclient-cpp
  '';

  installPhase = ''
    mkdir -p $out/bin $out/lib
    mv nsh $out/bin
    mv nsh.so $out/lib
    shopt -s extglob
    mv subprojects/restclient-cpp/librestclient_cpp.so!(*p) $out/lib
  '';
}
