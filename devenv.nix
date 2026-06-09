{ pkgs, inputs, ... }:

let
    nsh = pkgs.callPackage ./package/default.nix {
        restclient-cpp = inputs.restclient-cpp;
    };
in {
    packages = nsh.nativeBuildInputs ++ nsh.buildInputs;
}
