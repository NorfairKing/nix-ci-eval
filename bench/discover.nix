# nix-ci-eval's discovery, written as a single Nix expression so that any
# evaluator can be asked to perform it.
#
# The root matches src/nix_eval.cc's selectExpr (the five output types, scoped
# to one system, each absent one becoming {}), and the walk matches the
# coordinator's: recurse into every attribute set that is not a derivation,
# bounded by the same depth.
#
# The result is flattened to { "<attr.path>" = "<drvPath>" | null; } rather
# than left as a tree, because two evaluators are then comparable by a byte
# comparison of the JSON. An attribute that throws becomes null; nix-ci-eval
# reports it as an `error` line instead, and the difference does not matter to
# a differential as long as both evaluators are asked the same question.
{ flake, system }:
let
  f = builtins.getFlake flake;

  root = {
    packages.${system} = f.packages.${system} or { };
    checks.${system} = f.checks.${system} or { };
    devShells.${system} = f.devShells.${system} or { };
    formatter.${system} = f.formatter.${system} or { };
    devShell.${system} = f.devShell.${system} or { };
  };

  # [ref:maxAttrPathDepth]
  maxDepth = 12;

  isDerivation = v: (builtins.tryEval (v.type or null)).value or null == "derivation";

  recurse = depth: prefix: value:
    let
      attempt = builtins.tryEval value;
      v = attempt.value;
    in
    if !attempt.success then
      [{ name = prefix; value = null; }]
    else if isDerivation v then
      [{ name = prefix; value = (builtins.tryEval v.drvPath).value or null; }]
    else if builtins.isAttrs v && depth < maxDepth then
      builtins.concatLists (map
        (n: recurse (depth + 1) (if prefix == "" then n else prefix + "." + n) v.${n})
        (builtins.attrNames v))
    else
      [ ];
in
builtins.listToAttrs (recurse 0 "" root)
