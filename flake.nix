{
  description = "A build hook for Nix that dispatches builds through a job scheduler.";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs";
    restclient-cpp = {
      url = "github:mrtazz/restclient-cpp";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, ... }@inputs:
    let
      eachDefaultSystem = function:
        nixpkgs.lib.genAttrs [
          "x86_64-linux"
          "x86_64-darwin"
          "aarch64-linux"
          "aarch64-darwin"
        ] (system:
          let
            pkgs = import nixpkgs { inherit system; };
          in function pkgs system);
    in {
      checks = eachDefaultSystem (pkgs: system:
        import ./tests.nix {
          inherit nixpkgs pkgs;
          nix-scheduler-hook = self.packages.${system}.default;
        }
      );
      packages = eachDefaultSystem (pkgs: system: rec {
        default = nix-scheduler-hook;
        nix-scheduler-hook = pkgs.callPackage ./default.nix {
          restclient-cpp = inputs.restclient-cpp;
          nix = pkgs.nixVersions.nix_2_35;
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
            nixVersions.nix_2_35.libs.nix-util
            nixVersions.nix_2_35.libs.nix-store
            nixVersions.nix_2_35.libs.nix-main
            nlohmann_json
            openpbs
            slurm
          ];
        };
    });
    };
}
