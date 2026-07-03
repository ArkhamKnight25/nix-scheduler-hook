{ inputs, ... }:

let
    nsh = inputs.nsh.packages.x86_64-linux.default;
in {
    packages = nsh.nativeBuildInputs ++ nsh.buildInputs;
}
