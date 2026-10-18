# Replacing this tool with iets

[iets](https://github.com/numtide/iets) is a re-implementation of the Nix
client in Rust: its own parser, its own compiler to an IR, its own parallel
lazy evaluator, and its own fetchers. It is not an evaluator looking for a
frontend, it is a whole client, and one of its subcommands is `eval-jobs`,
which walks a jobset and prints one JSON line per derivation the way
nix-eval-jobs does. That is this tool's job. So where
[docs/fix-evaluator.md](fix-evaluator.md) asked whether another evaluator
could be swapped in under this C++ wrapper, the question here is whether the
wrapper is needed at all.

The short version. On the workload this tool actually has, iets is 4x to 5x
faster at two thirds of the memory, and exactly as correct: byte-identical
derivation paths over every output of three real flakes and over 57,927
nixpkgs attributes, with `isCached` agreeing too. It honours `NIX_STORE_DIR`,
so the hermetic checks can run under it, which is what killed fix. What it
cannot do is fetch a remote root flake, evaluate anything that needs
import-from-derivation, bound its own walk, or report transitive dependency
edges. None of those is an evaluator problem; the first is a caller problem
and the rest are frontend problems, and a replacement frontend is a Rust
program rather than this C++ one, because iets ships a Rust library and no C
ABI.

## How the comparison was made

`bench/jobs-root.nix` is this tool's discovery root written as a function of
the system and of a flake, matching `src/nix_eval.cc`'s `selectExpr`: the five
output types, scoped to one system, each absent one becoming `{}`. Handing it
to `iets eval-jobs --select` with `--force-recurse` asks iets to walk exactly
what this tool walks, because `--force-recurse` recurses into every attribute
set rather than only the ones marked `recurseForDerivations`.

`bench/iets-differential.sh` drives both tools over real flakes and compares
the attribute path to drvPath maps they produce:

```console
$ bench/iets-differential.sh path/to/iets 'git+file:///path/to/repo?rev=SHA'
```

A drvPath hashes the complete derivation that produced it, inputs and
environment and builder, transitively, so two tools agreeing on every drvPath
have agreed on everything that fed into every output. The attribute sets are
compared as well: a tool that discovered a different set of outputs disagrees
just as much as one that reached a different derivation.

`bench/package-set.nix` asks a much larger version of the same question of a
package set reached through a plain path, where throughput shows and flake
source handling never enters. That is the bulk column below.

iets 0.1.0 at `79c4346` builds unmodified from its own flake. Its reference
Nix is 2.34.8, which is the `nix` this machine and this repository's pinned
nixpkgs both carry, so the differential compares against the version iets
aims at.

## Speed

Measured on one 64-core machine, three interleaved repeats each, with other
work running on it. These barely moved, so the ranges below are the whole
observed spread. iets's own caches are turned off (`--no-cache`), because a
CI evaluator meets each revision once; leaving them on took between 0.02s and
0.2s off the `eval-jobs` runs, and no more than that.

On flakes, which is what this tool is given. nix-ci-eval here is doing its
full job (instantiating, `isCached`, dependency edges); iets is instantiating
and answering `--check-cache-status` but has no edges to report, which is the
gap discussed below:

| Flake | Outputs | nix-ci-eval | iets | ratio |
|---|---|---|---|---|
| this repository | 7 | 1.38-1.41s, 264-269 MiB | 0.34-0.35s, 210 MiB | 4.0-4.1x |
| autodocodec | 17 | 5.21-5.65s, 1.01-1.21 GiB | 1.03-1.06s, 633 MiB | 5.0-5.3x |
| validity | 50 | 5.93-7.86s, 0.99-1.00 GiB | 1.29-2.17s, 679 MiB | 2.7-4.6x |

The first `validity` run with instantiation took 58.5s against a store that
had never held those `.drv` files, and 2.17s then 1.29s afterwards. Writing a
closure through the daemon and querying substituters is work neither tool
avoids, and it is the first run that pays for it; the range above is the two
warm runs. The 4x to 5x figure is the one to carry away.

In bulk, through a plain path argument rather than a flake:

| Workload | Attributes | `nix-instantiate` | `iets eval` | drvPaths |
|---|---|---|---|---|
| `python3Packages` | 11223 (986 threw) | 6.7-7.7s, 1.19 GiB | 1.00-1.03s, 1.62-1.66 GiB | byte-identical |
| `haskellPackages` | 19523 (11601 threw) | 9.6-9.7s, 1.91 GiB | 1.69-1.72s, 2.62-2.66 GiB | byte-identical |
| nixpkgs top level | 27181 (4388 threw) | 45.1-46.0s, 8.78 GiB | 8.1-8.3s, 10.14 GiB | byte-identical |

Both tables point the same way, which is what makes this different from the
fix measurement. There, bulk evaluation was much faster and the flake
workload was slightly *slower*, because that workload is dominated by
reaching the first derivation rather than by the attributes, and fix made
startup worse by refetching the flake's inputs. iets reads the inputs its
lock pins, starts in milliseconds, and is faster on both shapes.

Memory goes the other way in bulk, as it does for any parallel evaluator
trading space for threads: about 1.2x to 1.4x of Nix's over a package set.
On flakes it is lower than this tool's, because this tool forks a worker per
core and iets defaults to four. Both figures are the peak resident size of
the largest single process, so both understate a pool.

Worker scaling on `validity`, which has 50 outputs:

| Workers | 1 | 4 | 8 | 16 |
|---|---|---|---|---|
| iets | 1.82s | 1.04s | 1.02s | 1.22s |

One iets worker already beats this tool's whole pool, which took 4.72s
restricted to one worker and about 5s with all of them. The default of four
is right for this shape and more does not help: 50 outputs over one warm
package set do not divide further.

Separately, a repeat `iets eval` of a package set it had already evaluated
answered in 0.01s from its tracing cache. Nothing in the `eval-jobs` runs hit
that cache, so it is not in the numbers above, and a CI evaluator seeing each
revision once would not hit it either.

## Correctness

Byte-identical drvPaths everywhere the two were asked the same question:

- every output of this repository, autodocodec and validity, pinned by rev
  through `git+file://`, 74 attributes in total;
- 57,927 nixpkgs attributes over the three package sets above;
- the same 7 outputs evaluated in a throwaway store under `NIX_STORE_DIR`
  with no daemon reachable, against this tool run the same way;
- the same 7 outputs from a `git clone --depth 1` of this repository pinned
  to its own HEAD, matching the full checkout exactly.

`isCached` agrees too: `iets eval-jobs --check-cache-status` gave the same
answer as this tool for every output of this repository, including the one
that is not built.

This is where fix fell down and iets does not. fix left a fetched flake's
source outside the store, named a `path:` reference's source tree after its
directory where Nix names it `source`, and reported the resulting wrong
derivations as success. iets names the root's store path `source` as Nix
does, computes it against `NIX_STORE_DIR` rather than a hardcoded
`/nix/store`, and agreed with Nix on every reference kind tried here.

iets's own parity testing is extensive and, unlike fix's, reaches flakes: a
differential fuzzer against Nix 2.34.8, the nix-conformance suite, and a
whole-package-set comparison of `iets eval-jobs` against nix-eval-jobs 2.35.2
over [repkgs](https://github.com/Mic92/repkgs). Its
`docs/reference/divergences.md` lists what it knowingly does differently,
and the entries are specific enough to check, which is how two of the
problems below were found before they were measured.

## What a replacement cannot do yet

### A root flake must be local

iets fetches no root flake. `github:`, `git+https:`, registry references and
remote tarballs are all refused, loudly:

```console
$ iets eval-jobs --flake github:NorfairKing/autodocodec ...
error: flake reference 'github:NorfairKing/autodocodec' is a 'github' input;
only local paths are supported (iets cannot fetch a root flake)
```

A local path, or a checkout pinned by rev with `git+file:///p?rev=SHA`, is
taken, and remote *inputs* named in the lock are fetched or substituted as
usual. So the deployed shape changes: whoever calls this tool would clone the
repository first and hand over a `git+file://` reference.

That is less of a loss than it sounds. The caller's clone can be
`--depth 1`, which is what `--shallow` buys today through Nix's fetcher, and
a shallow clone pinned by rev gave identical drvPaths here. It costs
`self.revCount` in exactly the cases where `--shallow` already costs it, and
it retires `--no-shallow`, `shallowIfRemoteGit` and
`test/shallow` along with them. But it is a change to the caller, not to this
tool, and it has to be agreed before any of the rest is worth starting.

Two smaller things come with it. iets reads a rev-pinned checkout's
`flake.nix` and `flake.lock` from the working tree rather than from the
commit, which is the same file when CI pins a clone to its own HEAD and is
not in general. And iets requires the lock to be complete and to match
`flake.nix`, where Nix would lock the difference in memory; a flake with a
stale lock fails rather than evaluating.

### No import from derivation

This tool allows IFD by default, so that an output which only resolves
through it is still discovered. iets never builds during evaluation:

```json
{"attr":"packages.x86_64-linux.viaIfd","error":"error: import from derivation
is not supported: '/nix/store/...-gen.drv' is not built", ...}
```

It is a per-attribute error, not a silent wrong answer, and the attribute is
still reported. But a flake that needs IFD loses the outputs behind it, and
`--import-from-derivation` becomes a flag with nothing behind it.

### The walk has no depth bound

This tool bounds its walk because an attribute set can contain itself. Given
`rec { deeper = { inherit deeper; }; }` under `packages.<system>`, it reports
an error at the bound and finishes. `iets eval-jobs --force-recurse` printed
nothing for 30 seconds and had to be killed.
nix-eval-jobs behaves the same way, so this is inherited rather than new, and
a frontend that keeps its own walk keeps its own bound.

### Nesting deeper than 50,000 levels is refused

iets counts nesting while parsing and refuses a file past 50,000 levels, so
that no input can crash it. Nix parses 400,000. `test/stackdepth` asserts
this tool evaluates a 200,000-level nested list, which is the regression test
for raising a worker's stack the way the `nix` CLI does; iets fails it at any
`ulimit`, because the cap is a count and not a stack size.

### There is no local store, and no transitive dependency edges

iets implements the Nix worker protocol and has no store of its own.
Evaluation itself needs no store: `--no-instantiate` computes every drvPath
with nothing but `NIX_STORE_DIR`, which is how the hermetic comparison above
ran. Writing the `.drv` files needs a daemon, and `--eval-store local` is
refused with `eval-jobs needs a Nix daemon`. Pointing `NIX_STORE_DIR` at a
throwaway directory while the daemon serves `/nix/store` fails with
`path "<dir>/...drv" is not in the Nix store`, which is the same mismatch fix
had, except that iets says so instead of answering wrongly. (A cosmetic bug
sits next to it: the `storeDir` key in each job line reports `/nix/store`
whatever `NIX_STORE_DIR` says.)

That matters because of the dependency edges, which are this tool's output
and not nix-eval-jobs'. `--show-input-drvs` gives each job's *direct* input
derivations and needs instantiation to do it. The edges this tool reports are
transitive and between discovered outputs, which means walking the input
closure of intermediate derivations that no job line names. From the command
line that needs the `.drv` files in a store, so it needs a daemon, so the
hermetic integration test could not reproduce it.

From the library it needs nothing at all. iets keeps every derivation the run
created in `Evaluator::drv.drvs`, and `api::drvshow::show(ev, roots, true)`
walks that whole input-drv closure from memory; its own `nix derivation show
-r` answers from there and leaves the store untouched. `EdgeDiscovery` in
`src/pure.hh` is already pure and store-agnostic: it asks for a path's
references and is told them. Feeding it from an in-memory derivation table
rather than from `queryPathInfo` would make the edges cheaper *and* hermetic,
which is the one place where this swap improves the design rather than
trading against it.

### Linking means writing this tool in Rust

iets is a Rust crate with a documented embedder API: `Session`, `EvalConfig`,
`api::evaluate`, a safe value walk mapped onto Nix's `nix_api_value.h`, and
working examples. What it does not have is a C ABI or a static library
target. So "link iets instead of libnixexpr" means a Rust frontend, and this
repository's C++ becomes the parts worth keeping translated: the walk, the
depth bound, the edge discovery, the line protocol, which are the files
already free of Nix and already unit-tested.

Shelling out to `iets eval-jobs` is more plausible than it was for fix, since
it streams a line per attribute as it finds them (half a second between the
first line and the last on `validity`) and isolates per-attribute errors
itself. It
still gives up the edges unless libnixstore stays for them, which would leave
this tool linking Nix in order to use a tool that exists to not link Nix.

The output contract otherwise survives either route: `type`, `attrPath`,
`isCached` and a `done` sentinel are all a translation of lines iets already
prints, and its exit code plus its stderr summary say whether the stream was
complete.

## Recommendation

Worth prototyping, which is more than fix earned, and not worth swapping yet.

The measurements support it on their own terms, in both directions at once:
4x to 5x faster on the flake-shaped workload this tool actually has, 5x to 7x
in bulk, less memory on flakes, and byte-identical derivations over
everything tried, hermetic runs included. The design would get simpler in the
one place it is currently awkward, because dependency edges would come from
an in-memory derivation table instead of a store that has to exist.

Against that: iets is 0.1.0, its author calls it LLM-ware and "still very
fresh", and this tool's entire job is to be believed about what a flake
builds. The gaps are also not all iets's to close. Whether the caller can
clone before evaluating is a decision about NixCI, not about iets, and
nothing else here is worth starting until it is made. IFD is a real
capability loss that no frontend can paper over.

In order:

1. Settle whether the caller clones. If it does not, stop here: iets cannot
   fetch a root flake and that is not a gap it intends to close.
2. Put `bench/iets-differential.sh` in CI over a handful of flakes, so the
   agreement above keeps being checked rather than remembered.
3. Prototype the frontend in Rust on `iets::api`, keeping this repository's
   walk, depth bound and edge discovery, and feeding the edges from the
   derivation table rather than from a store.
4. Decide about IFD with the flakes that actually need it in front of you.

The harness is committed, so re-measuring is one command.
