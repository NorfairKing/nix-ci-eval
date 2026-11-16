{
  description = "Fixture: many jobs over one expensive shared substrate";

  outputs = _:
    let
      # Stands in for what a real flake shares between its outputs: nixpkgs,
      # the overlays, the package set. Expensive to force, and traced, so
      # every process that forces it says so once.
      substrate = builtins.trace "SUBSTRATE-FORCED"
        (builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 300000));

      # Per-job work, so that attributes take long enough for the run to want
      # more than one worker. Without it the queue drains before the pool has
      # any reason to grow and the run says nothing about sharing.
      perJob = i: builtins.foldl' (a: b: a + b) i (builtins.genList (x: x) 400000);

      job = i: derivation {
        name = "job-${toString i}";
        system = "x86_64-linux";
        builder = "/bin/sh";
        args = [ "-c" (toString (substrate + perJob i)) ];
      };
    in
    {
      # The substrate stands between the run and every output, the way a real
      # flake's package set does: reaching any attribute at all forces it.
      checks.x86_64-linux = builtins.seq substrate (builtins.listToAttrs
        (map (i: { name = "j${toString i}"; value = job i; })
          (builtins.genList (x: x) 40)));
    };
}
