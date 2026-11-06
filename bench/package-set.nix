# One package set flattened to { "<name>" = "<drvPath>" | null; }, so that two
# evaluators can be compared on a bulk workload by a byte comparison of their
# JSON.
#
# bench/discover.nix asks the question this tool actually asks, of a flake.
# This one asks a much larger version of the same question, of a package set
# reached through a plain path, which is where an evaluator's throughput shows
# and where flake source handling does not enter at all. An attribute that
# throws becomes null, as it does there.
{ nixpkgs, attr, system }:
let
  pkgs = import nixpkgs { inherit system; };

  # The empty string means the top level rather than one of its package sets.
  set = if attr == "" then pkgs else pkgs.${attr};

  one = n:
    let
      attempt = builtins.tryEval (
        let v = set.${n}; in
        if (builtins.tryEval (v.type or null)).value or null == "derivation"
        then v.drvPath
        else null
      );
    in
    {
      name = n;
      value = if attempt.success then attempt.value else null;
    };
in
builtins.listToAttrs (map one (builtins.attrNames set))
