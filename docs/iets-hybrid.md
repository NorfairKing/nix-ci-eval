# Evaluating with iets when IFD is off

With `--no-import-from-derivation`, nix-ci-eval evaluates with
[iets](https://github.com/numtide/iets) and hands whatever iets cannot
evaluate to its Nix workers. Without that flag, nothing changes: Nix
evaluates everything, as before. NixCI passes the flag exactly when a
repository sets `allow-import-from-derivation = false`.
[iets-evaluator.md](iets-evaluator.md) has the measurements this rests on.

Output is unchanged: the same lines, the same jobs and drvPaths, the same
errors word for word, the same dependency edges. `test/iets/run.sh` asserts
that against a build without iets, in CI.

## Why only with IFD off

iets never builds during evaluation. With IFD off Nix refuses exactly the
same attributes, so iets loses nothing there. `test/iets/ifd-probe.nix`
reaches into derivation outputs every way that matters, against outputs
that are built and ones that are not, and the test asserts the two agree on
all of it.

## How a run goes

1. **Decide.** iets is used when IFD is off, the build has an iets
   (`-Diets=`), the flake is a `git` input with nothing iets would not
   honour (`submodules`, `lfs`, a `narHash`, ...), and the store is served
   by a Nix daemon. Otherwise the run is Nix's, and stderr says why.
2. **Check out.** iets fetches no root flake, and a `path:` root would lose
   `self.rev`, `self.lastModified` and `self.revCount`. So this tool runs
   `git` itself: `git init`, `git fetch` of the rev (or ref, or HEAD), and a
   detached checkout, in a temporary directory removed at exit. A remote
   repository is fetched shallow unless `--no-shallow`, as Nix's fetch is.
   Credentials come the way NixCI already supplies them, in the URL or in
   `GIT_SSH_COMMAND`. Error messages never include the URL, which can hold
   a token.
3. **Evaluate.** `iets eval-jobs` runs on `git+file://<checkout>?rev=<sha>`
   with `--check-cache-status` and `--eval-store unix://<socket>`, so the
   `.drv` files land in the store this tool reads edges from.
4. **Translate.** Each iets job becomes a `job` line and is registered for
   edge discovery, exactly as a Nix worker's result is.
5. **Hand errors to Nix.** An iets error is never reported. Its attribute
   goes onto the Nix workers' queue, and they evaluate it from the original
   flake reference, so the error a consumer sees is the one Nix writes.
   Workers are forked only when there is something for them, so a run
   without errors never starts Nix.
6. **Fall back whole.** If iets exits unsuccessfully or writes something this
   tool cannot read, Nix walks the whole flake and only attributes iets did
   not already report are reported. iets's own refusal to parse past 50,000
   levels of nesting is one way in.

The last line on stderr says how many jobs iets reported and how many
attributes it handed to Nix. On a flake whose attributes all evaluate,
anything handed to Nix points at iets and Nix disagreeing.

## The walk

`iets eval-jobs --force-recurse` has no depth bound and never finishes on a
set that contains itself. So the `--select` expression marks every set that
is not a derivation with `recurseForDerivations`, down to the coordinator's
depth bound, and throws past it. The throw is an iets error, so Nix
evaluates that path and reports it as too deep, as it always has.

## Testing

- `meson test -C builddir`: the routing, the git and iets arguments, and the
  reading of iets lines.
- `nix build .#checks.x86_64-linux.nix-ci-eval-iets-test`: starts a
  nix-daemon on a throwaway store and a loopback git daemon, then compares
  this tool with and without iets over the integration fixture (remote and
  local), the IFD probe, a set that contains itself, and a flake iets
  refuses to parse. It also asserts that iets did the evaluating in each
  case, so a run that quietly fell back does not pass for one that did not.
- `bench/iets-differential.sh` over real flakes, with `NIX_CI_EVAL_BIN`
  pointing at `nix build .#nix-ci-eval-without-iets`.

## Not covered yet

A private `git+ssh` input fetched by iets with the worker's
`GIT_SSH_COMMAND`. That needs NixCI's E2E setup, so it is tested there.
