# Feature progress

## Feature request

> I'd like to investigate using https://github.com/psyclyx/fix to do eval
> instead of our own custom evaluator.

## Answers from the user (step 2)

- **Git base**: branch from `master` (there is no `development` branch here).
- **Deliverable**:
  1. Figure out if using `fix` would be better.
  2. If there is a chance `fix` could be better, prepare a branch with a full
     replacement of the C++ nix-linked evaluator by `fix`, **without shelling
     out**. Link to `fix` instead.
- **Must keep**: every part of the output. Streaming NDJSON, daemon-free
  operation, `isCached` per job, dependency edges. Resource usage may change.
- **Upstream appetite**: everything is on the table.

## Workflow steps

### Step 0: Prepare git

Before implementing, prepare your git environment:

1. Make sure you're on the correct branch: `development`.
   Abort if you're not.
2. Create a feature branch for this work.
   Name it something descriptive related to the feature.
   Use lower-kebab-case for the branch name.
3. Push the branch before starting work.

I will clean up the commit history before merging, so you can commit as
often as you'd like without worrying about it.
I'd rather you make small, frequent commits as you work through the
implementation.

### Step 1: Research

Thoroughly explore the codebase to understand the areas relevant to this
feature. Read existing code, patterns, and related handlers/templates/models.
If the feature involves UI/UX decisions, do web searches for inspiration and
best practices.

The goal of this step is to build enough understanding to produce a solid plan and
ask informed questions. Do NOT start writing code yet.

### Step 2: Ask questions

Ask all clarifying questions at once — about requirements, edge cases, UX decisions,
and implementation approach. Front-load everything so there are no surprises later.

Wait for answers before proceeding.

### Step 3: Propose a plan

Based on the research and answers, propose a detailed implementation plan. Include:

- How to tell when you're done: Feedback loops
- Test coverage approach
- Which files will be created/modified
- Database changes (if any)
- Route additions (if any)
- Key implementation decisions and trade-offs

Save the approved plan to `feature-progress.md`.

Wait for feedback and approval before proceeding.

### Step 4: Implement

Implement the approved plan. Include tests where appropriate — check existing test
coverage patterns in the codebase to calibrate what "appropriate" means for this
feature.

### Step 5: Self-review (round 1)

Code review your own work using the `code-review` skill as a guide. Be thorough
and take your time.

**Important**: This is not a list of suggestions to present. Implement your own
review findings directly — fix every issue you find, then run feedback loops again.

### Step 6: Get CI to pass

Use the /ci skill to get CI to pass.

### Step 7: Self-review (round 2) + final CI

Take one more pass at reviewing your own code.
Implement any further improvements you find.

### Step 8: Final CI check

Use the /ci skill to get CI to pass again.

### Step 9: Commit, Push, and PR

0. Remove the feature-progress.md file.

1. Make a final commit on this branch.

2. Push the branch.

3. Create a PR on top of development.
   In the PR description, include:
   - Summary of what was implemented
   - Any remaining questions, suggestions, or advice
   - Things I should manually check (visual and code review)
   - A list of things I should try out locally (UX)

## Notes

### Status

- [x] Step 0: branch `fix-evaluator-investigation` off `origin/master`, pushed.
- [x] Step 1: research
- [x] Step 2: questions answered
- [ ] Step 3: plan
- [ ] Step 4: implement
- [ ] Steps 5-9

### Research findings

`fix` (psyclyx/fix, v0.3.0, MIT, alpha) is a from-scratch Nix evaluator in Zig.

What it has that matters here:

- Computes drv paths deterministically **without a store** (`docs/derivation/model.md`):
  normalize the recipe, hash it, compute paths. Writing `.drv` files is a
  separate store-layer operation.
- The in-memory `Drv` model carries `input_drvs` (dependency `.drv` + needed
  output names) and `input_srcs`, derived from string context.
- Shallow git fetch with the same `revCount` semantics we chose
  (`src/fetchers/git_transport.zig`: `--depth 1`, `rev_count = 0` sentinel).
- Flakes, pure eval by default for `--flake` installables, IFD, `--workers N`,
  `--gc-budget`.
- A whole-nixpkgs drvPath differential against CppNix over nixpkgs'
  `ci/eval/outpaths.nix` (~80k derivations), plus Lix/snix language suites.
- Packaged with `zig_0_16`, which is in our pinned nixpkgs 26.05.

Gaps against our contract:

- **No library artifact and no C ABI.** `build.zig` only produces executables;
  the only `export fn`s are a SHA256 helper. Linking needs an upstream lib
  target plus a C API.
- **No local store.** `local`/`auto` store URIs fail explicitly; a daemon is
  required for store operations. Our checks run daemon-free on a throwaway
  `$TMPDIR` store.
- **No streaming.** `fix eval --json` emits one blob at the end.
- **No substitutability query exposed.** The daemon client has `isValidPath`
  and `queryMissing`, but nothing surfaces them for an `isCached` answer.
- **No dependency-edge output.** `input_drvs` exists in memory but is not
  exposed, and eval does not write `.drv` files, so our
  `queryPathInfo(drv).references` walk has nothing to read.

Benchmarks on the author's hardware (pinned, `bench/results/`): 1.7x-5x faster
than CppNix on real NixOS/home-manager configs, up to ~39x on
parallelism-friendly torture cases, ~3.2x on nixpkgs package metadata.

### Measurements taken here

Built `fix` 0.3.0 from `nix/fix.nix` against our pinned nixpkgs 26.05
(`zig_0_16`), no changes needed. nixpkgs pin
`/nix/store/m0chqc0bmfxhjfpilx67cph63kn7gd11-source`. Each differential
flattens a tree to `{ "<attr.path>" = "<drvPath>" | null; }` and compares the
two evaluators byte for byte.

| Workload | Attrs | `nix-instantiate` | `fix` | drvPaths |
|---|---|---|---|---|
| `python3Packages` | 10858 (368 threw) | 7.7s / 1.0 GiB | 1.3s | byte-identical |
| nixpkgs top level, depth 2 | ~99k | 69.2s / 12.6 GiB | 27.7s / 39.1 GiB | byte-identical |
| this flake's discovery root, pinned rev | 8 | — | — | byte-identical **only with `--read-write-mode`** |

Wall clock on this flake's own discovery: `nix-ci-eval` 1.47s / 262 MiB,
`fix` 0.65s / 958 MiB (fix doing drvPaths only, no `isCached`, no edges).

### Correctness findings

1. **`self.outPath` is not a store path.** `fix` keeps fetched flake sources in
   `~/.cache/fix/...` rather than the store, so `builtins.getFlake ... .outPath`
   is a cache directory. The near-universal `src = ./.` idiom then resolves to a
   *different* source store path than Nix computes, because Nix re-imports an
   already-in-store path (producing the doubled `<hash>-<hash>-source` name) and
   `fix` does not. Result: a wrong drvPath, reported silently as success. On a
   pinned rev of this very flake, `fix` said
   `snpkm8b2l1rfl34cj22kp02msimmb98x-nix-ci-eval-0.0.0.drv` where Nix said
   `9jijvhpv6m7k0klq8k5gp3nz129sikhk-...`. Passing `--read-write-mode` registers
   the source in the store and the two agree byte for byte.
   `fix`'s own whole-nixpkgs differential cannot catch this: it passes nixpkgs
   as a plain `--arg` path, so flake source handling never enters.
2. **Flake inputs are fetched by `fix`, not read from the store.** On
   `~/src/validity` it went to the network and died after 2m14s with
   `fetching 'https://gitlab.horizon-haskell.net/...' failed: FetchGitFailed`,
   where `nix-ci-eval` finished in 5.9s from the lock.
3. **No local store.** `--store local?root=...` fails with "local, auto, and
   chroot stores need fix's native local-store backend, which is not implemented
   yet". Every check in `flake.nix` runs daemon-free on a throwaway store under
   `$TMPDIR` via `NIX_STORE_DIR`, so none of them can run under `fix` today.
   A `nix daemon` serving a throwaway store is reachable
   (`NIX_DAEMON_SOCKET_PATH=... fix eval --store daemon` works), but there is no
   way to tell `fix` a store dir other than `/nix/store`, and a build sandbox
   has `/nix/store` read-only.
4. **Store-selector quirk.** `--store unix://<socket>` fails with
   `StoreUnavailable` on a socket that `nix store info` talks to happily;
   `NIX_DAEMON_SOCKET_PATH` + `--store daemon` works.

### Verdict

Evaluation itself is faster (2.5x-5.9x wall clock) and, where it is comparable,
exactly as correct (byte-identical drvPaths over ~110k attributes). So there is
a real chance `fix` is better and the replacement is worth preparing.

The cost is concentrated outside the evaluator: `fix` has no library artifact
and no C ABI (only an executable; the sole `export fn`s are a SHA256 helper),
no local-store backend, no streaming output, no substitutability query, and no
dependency-edge output. It does have a clean embedder API — `Engine.init`,
`Engine.evaluate`, `stringValue`, attribute navigation — exercised by
`src/integration/expr_api/*.zig`, which is what a C shim would sit on.
