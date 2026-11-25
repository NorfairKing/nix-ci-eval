{
  # A hermetic fixture with no inputs: every output is a bare `derivation`, so
  # the flake evaluates offline against a throwaway store and needs no nixpkgs.
  # It is shaped to exercise every branch of nix-ci-eval's discovery.
  description = "nix-ci-eval integration fixture";

  outputs =
    _:
    let
      system = "x86_64-linux";

      drv =
        name: args:
        derivation {
          inherit name system;
          builder = "/bin/sh";
          args = [ "-c" args ];
        };

      # beta references alpha's output, so alpha is one of beta's input
      # derivations: that is the edge dependency discovery must find.
      alpha = drv "alpha" "echo alpha > $out";
      beta = drv "beta" "echo ${alpha} > $out";
    in
    {
      packages.${system} = {
        inherit alpha beta;
        # A nested attribute set: the discovered name is several path
        # components (nested.thing), which the consumer joins back with a dot.
        nested.thing = drv "nested-thing" "echo nested > $out";
        # Takes a moment to evaluate, so every edge is reported while it is still
        # being evaluated, unless a lone worker happens to pick it up first.
        # That is what lets the test assert that edges are streamed rather than
        # batched to the end: without it, the last few jobs can arrive in one
        # read, and every edge after them.
        slow =
          let
            range = builtins.genList (i: i) 8000;
            sum = builtins.foldl' (acc: _: acc + builtins.foldl' builtins.add 0 range) 0 range;
          in
          drv "slow" "echo ${toString sum} > $out";
      };

      checks.${system} = {
        gamma = drv "gamma" "echo gamma > $out";
        # Depends on a package, and checks are discovered before packages, so
        # this edge becomes known while the attribute it names has not been
        # reported yet. That is the order a consumer has to survive.
        delta = drv "delta" "echo ${alpha} > $out";
        # Fails to evaluate: nix-ci-eval isolates it as an error line and keeps
        # going rather than aborting the run.
        broken = throw "this attribute fails to evaluate";
      };

      devShells.${system}.default = drv "devshell" "echo devshell > $out";

      # A single-output type: reported as [formatter, <system>], no name.
      formatter.${system} = drv "formatter" "echo formatter > $out";
    };
}
