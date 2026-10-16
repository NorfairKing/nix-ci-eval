# Evaluating with fix instead of libnixexpr

[fix](https://github.com/psyclyx/fix) is a from-scratch Nix evaluator written
in Zig: its own parser, its own bytecode compiler, its own lazy VM, and a
parallel generational garbage collector underneath. It is not a wrapper around
the Nix evaluator, and it is faster than one. This records what happened when
it was measured against the evaluator this tool links today, what it would
take to swap them, and why that swap is not ready.

The short version: evaluation itself is 2.5x to 5.9x faster and, where the two
are comparable, exactly as correct — byte-identical derivation paths over
roughly 110,000 attributes. Everything around the evaluator is the problem.
fix ships no library, only an executable; it has no local-store backend, so
none of this repository's checks can run under it; and by default it computes
the wrong derivation for the single most common shape a flake takes, without
saying so.

## How the comparison was made

`bench/discover.nix` writes this tool's discovery as one Nix expression: the
same five output types scoped to one system, the same walk into every
attribute set that is not a derivation, the same depth bound. It flattens the
result to `{ "<attr.path>" = "<drvPath>" | null; }`, so two evaluators can be
compared by a byte comparison of their JSON.

That comparison is the whole point. A drvPath hashes the complete derivation
that produced it — inputs, environment and builder, transitively — so two
evaluators agreeing on every drvPath have agreed on everything that fed into
every output. This tool exists to tell a consumer what to build, and an
evaluator that names a derivation `nix build` would never produce is worse
than one that is slow.

`bench/fix-differential.sh` drives that expression through both evaluators on
real flakes and reports the disagreements. fix's own test suite does the same
thing over the whole of nixpkgs (`test/nixpkgs/outpaths.nix`, about 80,000
derivations) and compares every drvPath against a reference Nix, which is why
the correctness claim below is believable at all.

fix 0.3.0 builds unmodified against this repository's pinned nixpkgs 26.05,
which carries the `zig_0_16` it needs.

## What it is good at

| Workload | Attributes | `nix-instantiate` | `fix` | drvPaths |
|---|---|---|---|---|
| `python3Packages` | 10858 (368 threw) | 7.7s, 1.0 GiB | 1.3s | byte-identical |
| nixpkgs top level, two levels deep | ~99000 | 69.2s, 12.6 GiB | 27.7s, 39.1 GiB | byte-identical |
| this flake's discovery root | 7 | 0.73s | 0.85s | byte-identical |

Measured on one machine against the pinned nixpkgs, interleaved, once. Enough
to see an order of magnitude and not enough to publish.

The memory column is the interesting one. fix trades a great deal of memory
for the parallelism: 39 GiB against 12.6 GiB on the same tree, and its own
documentation warns that the full nixpkgs universe peaks around 51 GiB at
default settings. This tool currently bounds a worker at 4 GiB and restarts it
when it goes over, which is a mechanism that does not survive the move to one
process with a shared heap. `--gc-budget` is the replacement knob.

The small-flake row shows where the speed does not help. Most flakes this tool
sees have a handful of outputs over an enormous package set, so the run is
dominated by reaching the first derivation, not by the attributes themselves.
The fork-and-share-the-warm-evaluator design exists for exactly that case, and
a faster evaluator does not make it unnecessary.

## Why it is not ready

### Flake sources do not go in the store

This is the one that matters. fix keeps fetched flake sources in
`~/.cache/fix/...` rather than the Nix store, so `builtins.getFlake ... .outPath`
is a cache directory and not a store path.

The consequence is not a missing feature but a wrong answer. When a flake does
`src = ./.` — which is most of them — Nix sees a path that is already in the
store and re-imports it, producing the doubled `<hash>-<hash>-source` name
that flake packaging is known for. fix sees a path that is not in the store,
imports it once, and gets a different source path. Every derivation built from
that source then has a different drvPath, and fix reports it as success.

On a pinned revision of this very repository, fix said
`snpkm8b2l1rfl34cj22kp02msimmb98x-nix-ci-eval-0.0.0.drv` where Nix said
`9jijvhpv6m7k0klq8k5gp3nz129sikhk-nix-ci-eval-0.0.0.drv`. Six of this flake's
seven outputs disagreed.

`--read-write-mode` registers the source in the store and the disagreement
goes away completely. So this is a default to be avoided rather than a wall,
but it is worth dwelling on how it stayed hidden: fix's whole-nixpkgs
differential passes nixpkgs as a plain path argument, so flake source handling
never enters it. An 80,000-derivation differential passing says nothing about
the idiom every flake in the wild uses.

### There is no local store

fix implements the Nix worker protocol and nothing else. `local`, `auto` and
chroot roots fail with "need fix's native local-store backend, which is not
implemented yet".

Every check in `flake.nix` runs inside a Nix build sandbox, where
`/nix/store` is read-only and no daemon socket exists.
`test/integration/run.sh` works around this by pointing the tool at a
throwaway store with `NIX_STORE_DIR`, `NIX_STATE_DIR` and friends; CppNix then
opens a local store rooted in a temp directory, with its own SQLite database,
and evaluation writes its `.drv` files there. No daemon, no network, nothing
touching the real store.

fix can do neither half of that:

- It cannot open a local store at all, so the throwaway store is out.
- A `nix daemon` serving a throwaway store *is* reachable —
  `NIX_DAEMON_SOCKET_PATH=<socket> fix eval --store daemon --read-write-mode`
  stores a `.drv` through one — but fix computes every store path against a
  hardcoded `/nix/store`, while `nix daemon --store "local?root=$T"` serves a
  store whose directory is `$T/nix/store`. fix returns `/nix/store/...` paths
  for a store that keeps them somewhere else, and says nothing. Making the
  daemon serve `/nix/store` from elsewhere needs a bind mount, and inside a
  build sandbox `/nix/store` is already mounted read-only.

A writable store is not optional for us: `--read-write-mode` is what makes the
drvPaths right, and the dependency-edge walk reads
`queryPathInfo(drv).references`, which needs the `.drv` in a store to read.

So the unblocking move is a store-directory setting in fix plus a daemon
started inside each check, or a native local-store backend. The first is much
the smaller of the two, and duplicates nothing: libnixstore, which this tool
already links, keeps doing the store work either way.

### Flake inputs are refetched rather than read from the lock

fix resolves flake inputs through its own fetchers and its own cache instead
of reading what the lock file already pins into the store. On
`~/src/validity` it went to the network and died after two minutes and
fourteen seconds with
`fetching 'https://gitlab.horizon-haskell.net/...' failed: FetchGitFailed`,
where this tool finished in 5.9 seconds from the lock. For a CI evaluator
that is both a latency problem and a reliability one: every input is fetched
a second time, over a transport that is not Nix's, into a cache that is not
the store the build will use.

### Smaller things

`--store unix://<socket>` fails with `StoreUnavailable` against a socket that
`nix store info` talks to happily, while `NIX_DAEMON_SOCKET_PATH` plus
`--store daemon` works on the same socket.

There is no streaming output: `fix eval --json` writes one blob at the end,
where this tool's contract is a line per output as it is discovered.

There is no substitutability query exposed, so `isCached` has no answer, and
no way to read a derivation's `input_drvs`, so dependency edges have no
source — although both of those are things libnixstore can keep doing.

## What a replacement would look like

The user-facing requirement is that nothing in the output changes: streaming
NDJSON, `isCached` per job, dependency edges, the `done` sentinel. Only
resource usage may move. Linking fix rather than shelling out to it is also a
requirement, and it is the right one — shelling out would give up streaming
and per-attribute error reporting in the same breath.

Linking is plausible. fix has a real embedder API — `Engine.init` with a
worker count, `Engine.evaluate`, `stringValue`, attribute navigation — and
`src/integration/expr_api/*.zig` is a test suite that exercises it from an
embedder's point of view. What is missing is a C ABI over it and a static
library target in `build.zig`; today the only `export fn`s in the whole
repository are a SHA256 helper, and `build.zig` produces executables only.

Sketched out, the work is:

1. A `libfix.a` plus header, and a `src/capi.zig` shim over `Engine`, in fix.
2. A store-directory setting in fix, so hermetic checks can run against a
   daemon over a throwaway store.
3. This tool's evaluator rewritten on that C API, with libnixstore kept for
   `isCached` and the edge walk, fed by drvPaths and output paths the
   discovery expression reports.
4. Per-attribute error isolation moved from forked workers into
   `builtins.tryEval` inside the discovery expression.
5. The fork pool replaced by a thread pool over one `Engine`, emitting each
   result as it lands so the stream survives.

That is a multi-week change spanning two repositories, the second of which is
alpha software under daily development.

## Recommendation

Not yet, and not because of the speed.

The thing to weigh is not 2.5x against the effort. It is that this tool's
entire job is to be believed about what a flake builds, and the evaluator
under consideration currently answers that question wrongly, silently, for
the commonest flake there is — and its own extensive differential testing did
not notice, because of where the test universe is rooted. `--read-write-mode`
closes that particular hole, and the agreement afterwards is total and
convincing. But the hole existing at all is the argument for waiting until
there is a local-store backend and the hermetic checks can run under fix,
rather than for adopting it on the strength of a drvPath comparison that
cannot run in CI.

Revisit when fix grows a local store. The differential harness is committed,
so the measurement is a single command away:

```console
$ bench/fix-differential.sh path/to/fix <flake-ref>...
```
