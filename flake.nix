{
  description = "nix-ci-eval: discover a flake's buildable outputs for one system";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forEachSystem = nixpkgs.lib.genAttrs systems;
      pkgsFor = sys: import nixpkgs { system = sys; };

      # The dev shell is only built where it is developed, so it stays a
      # single-system output while the tool itself is built for both.
      devSystem = "x86_64-linux";
    in
    {
      # Built with the consumer's package set, so the tool links the same nix
      # as whatever else that package set builds.
      overlays.default = final: _prev: {
        nix-ci-eval = final.callPackage ./package.nix { };
      };

      packages = forEachSystem (sys:
        let pkgs = pkgsFor sys; in
        {
          nix-ci-eval = pkgs.callPackage ./package.nix { };
          default = self.packages.${sys}.nix-ci-eval;
        });

      checks = forEachSystem (sys:
        let pkgs = pkgsFor sys; in
        {
          # Compiles the tool and runs its pure-helper unit tests (via doCheck).
          nix-ci-eval = self.packages.${sys}.nix-ci-eval;

          # Runs the built tool against the hermetic fixture flake and compares
          # its output to the golden, covering the JSON contract end to end. The
          # tool manages its own throwaway store under $TMPDIR, so this needs no
          # daemon and no network.
          nix-ci-eval-integration-test =
            pkgs.runCommand "nix-ci-eval-integration-test"
              {
                nativeBuildInputs = [ pkgs.jq ];
                NIX_CI_EVAL_BIN = pkgs.lib.getExe self.packages.${sys}.nix-ci-eval;
              }
              ''
                export HOME=$TMPDIR
                bash ${./test/integration}/run.sh
                touch $out
              '';
        });

      devShells.${devSystem}.default =
        let pkgs = pkgsFor devSystem; in
        pkgs.mkShell {
          inputsFrom = [ self.packages.${devSystem}.nix-ci-eval ];
          packages = [ pkgs.clang-tools ];
        };
    };
}
