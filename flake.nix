{
  description = "A Nix store plugin that dispatches builds through a job scheduler.";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs";
    # Nix fork carrying the Store/Builder separation ("Separate
    # building/scheduling from storage", cherry-picked from NixOS/nix);
    # NSH is built against it and the VM test nodes run it.
    # For local development against a checkout, override with:
    #   nix build --override-input determinate/nix path:/home/lisanna/nix ...
    determinate.url = "https://flakehub.com/f/DeterminateSystems/determinate/3";
    determinate.inputs.nix.url = "github:lisanna-dettwyler/nix/detnix-builder-store";
    restclient-cpp = {
      url = "github:mrtazz/restclient-cpp";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, nix, ... }@inputs:
    let
      eachDefaultSystem = function:
        nixpkgs.lib.genAttrs [
          "x86_64-linux"
          "aarch64-linux"
        ] (system:
          let
            pkgs = import nixpkgs { inherit system; };
          in function pkgs system);
    in {
      checks = eachDefaultSystem (pkgs: system:
        import ./nix/tests.nix {
          inherit nixpkgs pkgs;
          # nix-cli, not default: default is nix-everything, which pulls the
          # fork's whole test pipeline into every CI run. The VM nodes only
          # need a runnable nix.
          nix = inputs.determinate.inputs.nix.packages.${system}.nix-cli;
          nix-scheduler-hook = self.packages.${system}.default;
        }
      );
      packages = eachDefaultSystem (pkgs: system: rec {
        default = nix-scheduler-hook;
        nix-scheduler-hook = pkgs.callPackage ./nix/package.nix {
          restclient-cpp = inputs.restclient-cpp;
          nix = inputs.determinate.inputs.nix.packages.${system}.default;
        };
      });
      devShells = eachDefaultSystem (pkgs: system: {
        default = pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
          packages = with pkgs; [
            clang-tools
            meson
            ninja
            cmake
            pkg-config
            boost
            curl
            inputs.determinate.inputs.nix.packages.${system}.default.libs.nix-util
            inputs.determinate.inputs.nix.packages.${system}.default.libs.nix-store
            inputs.determinate.inputs.nix.packages.${system}.default.libs.nix-main
            nlohmann_json
            openpbs
            slurm
          ];
        };
    });
    };
}
