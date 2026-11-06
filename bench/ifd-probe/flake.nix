# One output per way an evaluation can reach into a derivation's output, for
# bench/ifd-probe.sh to ask whether iets refuses exactly what Nix refuses
# with import-from-derivation turned off.
#
# Every IFD shape appears twice: against an output that was built before the
# probe ran and against one that never is (@salt@ is replaced per run).
# Nix with IFD off refuses both, so an evaluator that quietly reads an output
# because it happens to be in the store would show up as a disagreement on
# the built half.
#
# The derivations use only the sandbox's /bin/sh so that the probe has no
# inputs and builds in milliseconds.
{
  outputs = { self }:
    let
      system = "x86_64-linux";
      file = name: content: derivation {
        inherit name system;
        builder = "/bin/sh";
        args = [ "-c" "echo -n '${content}' > $out" ];
      };
      # A derivation whose hash depends on v, so that any way of computing v
      # differently shows up in the drvPath.
      mk = name: v: derivation {
        inherit name system;
        builder = "/bin/sh";
        args = [ "-c" "echo > $out" ];
        v = builtins.hashString "sha256" (builtins.toJSON v);
      };
      built = file "probe-built" "hello";
      builtNix = file "probe-built-nix" "{ a = 1; }";
      builtSet = file "probe-built-set" "{ fromSet = derivation { name = \"s\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; args = [ \"-c\" \"echo > $out\" ]; }; }";
      unbuilt = file "probe-unbuilt-@salt@" "hello";
      unbuiltNix = file "probe-unbuilt-nix-@salt@" "{ a = 1; }";
    in
    {
      # Outside the discovered output types, so the driver can build them.
      prebuild = { inherit built builtNix builtSet; };

      packages.${system} = {
        plainContext = mk "plain-context" "${unbuilt}";
        drvPathString = mk "drv-path-string" unbuilt.drvPath;
        readToFile = mk "read-to-file" (builtins.readFile (builtins.toFile "a" "b"));
        readFileUnbuilt = mk "read-file-unbuilt" (builtins.readFile unbuilt);
        readFileBuilt = mk "read-file-built" (builtins.readFile built);
        importUnbuilt = mk "import-unbuilt" (import unbuiltNix);
        importBuilt = mk "import-built" (import builtNix);
        pathExistsBuilt = mk "path-exists-built" (builtins.pathExists "${built}");
        pathExistsUnbuilt = mk "path-exists-unbuilt" (builtins.pathExists "${unbuilt}");
        hashFileBuilt = mk "hash-file-built" (builtins.hashFile "sha256" built);
        pathBuilt = mk "path-built" "${builtins.path { path = built; name = "p"; }}";
        readDrvFile = mk "read-drv-file" (builtins.readFile unbuilt.drvPath);
        discardedBuilt = mk "discarded-built" (builtins.readFile (builtins.unsafeDiscardStringContext "${built}"));
        tryEvalUnbuilt = mk "try-eval-unbuilt" (builtins.tryEval (builtins.readFile unbuilt)).success;
        tryEvalBuilt = mk "try-eval-built" (builtins.tryEval (builtins.readFile built)).success;
        lazyIfdInMeta = (mk "lazy-meta" 1) // { meta.x = builtins.readFile unbuilt; };
      };

      # A whole output set that only exists through IFD, so the failure lands
      # on the set rather than on one attribute.
      checks.${system} = import builtSet;
    };
}
