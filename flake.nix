{
  description = "A Nix store plugin that dispatches builds through a job scheduler.";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs";
    # Nix fork carrying the Phase-2 Builder API (Builder inputs-overloads);
    # NSH is built against it and the VM test nodes run it.
    # For local development against a checkout, override with:
    #   nix build --override-input nix path:/home/amrit/nix-gsoc ...
    nix.url = "github:arkhamknight25/nix?ref=gsoc/builder-virtual-method";
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
        import ./tests.nix {
          inherit nixpkgs pkgs;
          nix = nix.packages.${system}.default;
          nix-scheduler-hook = self.packages.${system}.default;
        }
      );
      packages = eachDefaultSystem (pkgs: system: rec {
        default = nix-scheduler-hook;
        nix-scheduler-hook = pkgs.callPackage ./default.nix {
          restclient-cpp = inputs.restclient-cpp;
          nix = nix.packages.${system}.default;
        };
      });
    };
}
