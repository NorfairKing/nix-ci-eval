# nix-ci-eval's discovery root, as a function of the system and of a flake,
# so that `iets eval-jobs --select` can be asked to walk exactly what this
# tool walks.
#
# This mirrors src/nix_eval.cc's selectExpr: the five output types, scoped to
# one system, each absent one becoming {}. The argument is iets's eval-jobs
# root for a flake with no fragment, which holds the flake's outputs under
# `outputs` rather than merged into the top level.
#
# bench/discover.nix is the same selection written for a plain evaluator; this
# one exists because eval-jobs walks the tree itself and wants only the root.
system: flake:
let
  o = flake.outputs;
in
{
  packages.${system} = o.packages.${system} or { };
  checks.${system} = o.checks.${system} or { };
  devShells.${system} = o.devShells.${system} or { };
  formatter.${system} = o.formatter.${system} or { };
  devShell.${system} = o.devShell.${system} or { };
}
