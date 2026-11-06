{
  description = "nix-ci-eval: discover a flake's buildable outputs for one system";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    # Not following nixpkgs: iets needs a newer Rust than a stable nixpkgs
    # may carry, and it is tested against its own.
    iets.url = "github:numtide/iets";
  };

  outputs = { self, nixpkgs, iets }:
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
        nix-ci-eval = final.callPackage ./package.nix {
          iets = iets.packages.${final.stdenv.hostPlatform.system}.default;
        };
      };

      packages = forEachSystem (sys:
        let pkgs = pkgsFor sys; in
        {
          nix-ci-eval = pkgs.callPackage ./package.nix {
            iets = iets.packages.${sys}.default;
          };
          # Evaluates everything with Nix, for comparing iets against.
          nix-ci-eval-without-iets = pkgs.callPackage ./package.nix { };
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

          # Serves a two-commit repository over a loopback git daemon and
          # asserts the default fetch pulls only the one commit while
          # --no-shallow pulls both. The daemon is on 127.0.0.1, so this needs
          # no network; git provides both the client and the daemon.
          nix-ci-eval-shallow-fetch-test =
            pkgs.runCommand "nix-ci-eval-shallow-fetch-test"
              {
                nativeBuildInputs = [ pkgs.git ];
                NIX_CI_EVAL_BIN = pkgs.lib.getExe self.packages.${sys}.nix-ci-eval;
              }
              ''
                export HOME=$TMPDIR
                bash ${./test/shallow}/run.sh
                touch $out
              '';

          # Evaluates fixture flakes with import-from-derivation off, which
          # goes through iets, and asserts the output is exactly what a build
          # without iets reports. Starts its own nix-daemon on a throwaway
          # store, because iets writes derivations through one, and serves
          # the flakes over a loopback git daemon, so it needs no network.
          nix-ci-eval-iets-test =
            pkgs.runCommand "nix-ci-eval-iets-test"
              {
                nativeBuildInputs = [ pkgs.git pkgs.jq pkgs.nix ];
                NIX_CI_EVAL_BIN = pkgs.lib.getExe self.packages.${sys}.nix-ci-eval;
                NIX_CI_EVAL_WITHOUT_IETS_BIN = pkgs.lib.getExe self.packages.${sys}.nix-ci-eval-without-iets;
                INTEGRATION_FLAKE = ./test/integration/flake.nix;
              }
              ''
                export HOME=$TMPDIR
                bash ${./test/iets}/run.sh
                touch $out
              '';

          # Evaluates a deep-but-finite flake attribute under an 8 MiB soft
          # stack limit and asserts the worker discovers it rather than
          # overflowing, which asserts the tool raises the evaluation stack the
          # way the `nix` CLI does. Hermetic: the flake has no inputs.
          nix-ci-eval-stack-depth-test =
            pkgs.runCommand "nix-ci-eval-stack-depth-test"
              {
                NIX_CI_EVAL_BIN = pkgs.lib.getExe self.packages.${sys}.nix-ci-eval;
              }
              ''
                export HOME=$TMPDIR
                bash ${./test/stackdepth}/run.sh
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
