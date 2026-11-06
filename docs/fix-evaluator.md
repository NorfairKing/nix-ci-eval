# Evaluating with fix instead of libnixexpr

[fix](https://github.com/psyclyx/fix) is a from-scratch Nix evaluator written
in Zig: its own parser, its own bytecode compiler, its own lazy VM, and a
parallel generational garbage collector underneath. It is not a wrapper around
the Nix evaluator, and on large evaluations it is considerably faster than
one. This records what happened when it was measured against the evaluator
this tool links today, what swapping them would take, and why that swap is not
worth making yet.

The short version. On bulk evaluation fix is 1.4x to 5.9x faster, depending on
the tree and on how many cores it gets, and exactly as correct: byte-identical
derivation paths over roughly 110,000 attributes. On the workload this tool
actually has, a handful of outputs over an enormous package set, it is
*slower*, because the run is dominated by reaching the first derivation rather
than by the attributes. Separately, it computes wrong derivations for some
flake references without saying so, it ships no library to link against, and
it has no local store, so none of this repository's checks can run under it.

## How the comparison was made

`bench/discover.nix` writes this tool's discovery as one Nix expression: the
same five output types scoped to one system, the same walk into every
attribute set that is not a derivation, the same depth bound. It flattens the
result to `{ "<attr.path>" = "<drvPath>" | null; }`, so two evaluators can be
compared by a byte comparison of their JSON.

That comparison is the whole point. A drvPath hashes the complete derivation
that produced it, inputs and environment and builder, transitively, so two
evaluators agreeing on every drvPath have agreed on everything that fed into
every output. This tool exists to tell a consumer what to build, and an
evaluator that names a derivation `nix build` would never produce is worse
than one that is slow.

`bench/fix-differential.sh` drives that expression through both evaluators on
real flakes and reports the disagreements:

```console
$ bench/fix-differential.sh path/to/fix github:NorfairKing/autodocodec
```

fix's own test suite does the same thing over the whole of nixpkgs
(`test/nixpkgs/outpaths.nix`, about 80,000 derivations) and compares every
drvPath against a reference Nix, which is why the correctness claim below is
believable at all. The shape of that test universe is also why it missed what
is described under "Flake sources" below.

fix 0.3.0 builds unmodified against this repository's pinned nixpkgs 26.05,
which carries the `zig_0_16` it needs.

## Speed

Measured interleaved on one 64-core machine against the pinned nixpkgs, with
other work running on it. fix is parallel and Nix is not, so fix is much the
more sensitive of the two to what else the machine is doing: its bulk numbers
moved by half again between repeats where Nix's barely moved at all. The
ranges below are the spread actually observed, not error bars.

In bulk, through a plain path argument rather than a flake:

| Workload | Attributes | `nix-instantiate` | `fix` | drvPaths |
|---|---|---|---|---|
| `python3Packages` | 10858 (368 threw) | 6.9-7.7s, 1.3 GiB | 1.3-1.9s, 4.1 GiB | byte-identical |
| nixpkgs top level, two levels deep | 99861 (16121 threw) | 69.0-69.2s, 12.6 GiB | 27.7-48.1s, 39.2 GiB | byte-identical |

On flakes, which is what this tool is actually given. Three repeats each, and
these barely moved:

| Flake | Attributes | `nix-instantiate` | `fix` | ratio |
|---|---|---|---|---|
| `git+file:` this repository, pinned rev | 7 | 0.72-0.74s | 0.82-0.89s | 0.81-0.91x |
| `git+file:` autodocodec | 17 | 3.92-3.98s | 4.18-4.50s | 0.88-0.94x |

The second table is the one that decides this, and it points the other way
from the first. Almost every flake this tool sees has a few dozen outputs
hanging off an enormous package set, so the cost is reaching the first
derivation, not evaluating the attributes once the package set is warm. A
faster evaluator does not help with that, and fix's startup, including
refetching the flake's inputs, makes it consistently a little worse. The
fork-and-share-the-warm-evaluator design exists precisely for this shape, and
nothing in fix replaces it.

The memory column is worth noting too. fix trades a great deal of memory for
its parallelism: 39 GiB against 12.6 GiB on the same tree, and its own
documentation warns the full nixpkgs universe peaks around 51 GiB at default
settings. This tool bounds a worker at 4 GiB and restarts it when it goes
over, a mechanism that does not survive the move to one process with one
shared heap. `--gc-budget` would be the replacement knob.

## Correctness

Where the two evaluators are asked the same question in the same way they
agree completely: byte-identical drvPaths over ~110,000 nixpkgs attributes,
and over every output of the flakes tried here. The disagreements are all
about how a flake's *source* reaches the store, and they are silent.

### Flake sources are not registered by default

By default fix keeps fetched flake sources in `~/.cache/fix/...` rather than
the Nix store, so `builtins.getFlake ... .outPath` is a cache directory and
not a store path.

When a flake does `src = ./.`, which is most of them, Nix sees a path already
in the store and re-imports it, producing the doubled `<hash>-<hash>-source`
name that flake packaging is known for. fix sees a path that is not in the
store, imports it once, and gets a different source path. Every derivation
built from that source then has a different drvPath, and fix reports it as
success. Six of this flake's seven outputs disagreed this way.

`--read-write-mode` registers the source in the store and this disagreement
goes away. Any replacement would always pass it.

### The source store path is named differently for path references

`--read-write-mode` is necessary and not sufficient. Nix names a flake's
source store path `source` whatever the flake is called; fix names it after
the directory. For a `path:` or bare-path reference to autodocodec, Nix
produced `l2ggwd0c56wdk00van8j9wgnnb1q09ln-source` and fix produced
`gn73y8vs2n5h706xl1ykf8as5yzddc53-autodocodec`, and the check that uses the
flake root as its `src` got a different drvPath.

Which reference kind you use decides whether this bites:

| Reference | Agreement with `--read-write-mode` |
|---|---|
| `github:owner/repo` | agrees |
| `git+file:///path` | agrees |
| `path:/path`, bare `/path` | **disagrees** |

That is a relief for the deployed case, which passes `git+` and `github:`
references, and a problem for local use and for `test/integration/run.sh`,
which evaluates `path:$here`.

Both of these stayed hidden from fix's own differential because it passes
nixpkgs as a plain `--arg` path, so flake source handling never enters it. An
80,000-derivation differential passing says nothing about the idiom every
flake in the wild uses, which is worth remembering when reading the
compatibility claims.

## The other blockers

### There is no local store

fix implements the Nix worker protocol and nothing else. `local`, `auto` and
chroot roots fail with "need fix's native local-store backend, which is not
implemented yet".

Every check in `flake.nix` runs inside a Nix build sandbox, where `/nix/store`
is read-only and no daemon socket exists. `test/integration/run.sh` works
around this by pointing the tool at a throwaway store with `NIX_STORE_DIR`,
`NIX_STATE_DIR` and friends; CppNix then opens a local store rooted in a temp
directory, with its own SQLite database, and evaluation writes its `.drv`
files there. No daemon, no network, nothing touching the real store.

fix can do neither half of that. It cannot open a local store at all. A `nix
daemon` serving a throwaway store *is* reachable
(`NIX_DAEMON_SOCKET_PATH=<socket> fix eval --store daemon --read-write-mode`
stores a `.drv` through one) but fix computes every store path against a
hardcoded `/nix/store`, while `nix daemon --store "local?root=$T"` serves a
store whose directory is `$T/nix/store`. fix returns `/nix/store/...` paths
for a store that keeps them elsewhere, and says nothing. Making a daemon serve
`/nix/store` from elsewhere needs a bind mount, and inside a build sandbox
`/nix/store` is already mounted read-only.

A writable store is not optional here: `--read-write-mode` is what makes the
drvPaths right, and the dependency-edge walk reads
`queryPathInfo(drv).references`, which needs the `.drv` in a store to read.

So unblocking the checks needs a store-directory setting in fix plus a daemon
started inside each one, or a native local-store backend. The first is much
the smaller, and duplicates nothing: libnixstore, which this tool already
links, keeps doing the store work either way.

### Flake inputs are refetched rather than read from the lock

fix resolves flake inputs through its own fetchers into its own cache, instead
of reading what the lock file already pins into the store. On `validity` it
went to the network and died after two minutes and fourteen seconds with
`fetching 'https://gitlab.horizon-haskell.net/...' failed: FetchGitFailed`,
where this tool finished in 5.9 seconds from the lock. For a CI evaluator that
is a latency problem and a reliability one at once: every input is fetched a
second time, over a transport that is not Nix's, into a cache that is not the
store the build will use.

### Smaller things

`--store unix://<socket>` fails with `StoreUnavailable` against a socket that
`nix store info` talks to happily, while `NIX_DAEMON_SOCKET_PATH` plus
`--store daemon` works on that same socket.

There is no streaming output: `fix eval --json` writes one blob at the end,
where this tool's contract is a line per output as it is discovered.

No substitutability query is exposed, so `isCached` has no answer, and nothing
surfaces a derivation's `input_drvs`, so dependency edges have no source,
though libnixstore can keep doing both.

## What a replacement would look like

The requirement is that nothing in the output changes: streaming NDJSON,
`isCached` per job, dependency edges, the `done` sentinel. Only resource usage
may move. Linking fix rather than shelling out to it is the right call;
shelling out gives up streaming and per-attribute error reporting in the same
breath.

Linking is plausible. fix has a real embedder API, `Engine.init` with a worker
count, `Engine.evaluate`, `stringValue` and attribute navigation, and
`src/integration/expr_api/*.zig` exercises it from an embedder's point of
view. What is missing is a C ABI over it and a static library target in
`build.zig`; today the only `export fn`s in the repository are a SHA256
helper, and `build.zig` produces executables only.

Sketched out:

1. A `libfix.a` plus header, and a `src/capi.zig` shim over `Engine`, in fix.
2. A store-directory setting in fix, so hermetic checks can run against a
   daemon over a throwaway store.
3. This tool's evaluator rewritten on that C API, with libnixstore kept for
   `isCached` and the edge walk, fed by the drvPaths and output paths the
   discovery expression reports.
4. Per-attribute error isolation moved from forked workers into
   `builtins.tryEval` inside the discovery expression.
5. The fork pool replaced by a thread pool over one `Engine`, emitting each
   result as it lands so the stream survives.

A multi-week change across two repositories, the second of them alpha software
under daily development.

## Recommendation

Not yet, and the speed is not the reason either way.

The measurements do not support the change on their own terms. fix wins
decisively on bulk evaluation and loses on the shape of work this tool
actually does, where startup dominates and fix's extra fetching makes startup
worse. There is no performance argument here to weigh the risk against.

And the risk is real. This tool's entire job is to be believed about what a
flake builds, and fix currently answers that question wrongly, silently, for
flake references of a kind we use locally and in our own integration test. Its
extensive differential testing did not catch either case, because the test
universe is rooted somewhere flake source handling never reaches.

Revisit when fix has a local store, at which point the checks can run under it
and the differential can live in CI rather than in a script someone remembers
to run. The harness is committed, so re-measuring is one command.

[docs/iets-evaluator.md](iets-evaluator.md) asks the same question of iets,
which answers it differently: faster on this tool's own workload rather than
only in bulk, and right about flake sources.
