#!/usr/bin/env bash
# Regression test: a worker must evaluate a deep-but-finite expression that the
# `nix` CLI handles, because that CLI (as `nix flake show`) is what discovery
# replaced. nix-ci-eval evaluates in forked workers, which inherit the default
# 8 MiB stack; unless the tool raises the stack the way the `nix` CLI does, a
# deeply nested value overflows the worker mid-attribute. Nix's stack-overflow
# SIGSEGV is uncatchable, so the worker dies and the attribute is misreported
# as a crashed-worker error rather than discovered.
#
# The soft stack limit is pinned to 8 MiB (the common default) with the hard
# limit left alone, so the result does not depend on the caller's ambient
# limit: without the fix the worker overflows here; with it the tool raises the
# soft limit back up before evaluating, so it must not overflow.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="${NIX_CI_EVAL_BIN:-$here/../../builddir/nix-ci-eval}"

store="$(mktemp -d)"
trap 'chmod -R u+w "$store" 2>/dev/null || true; rm -rf "$store"' EXIT

# A hermetic no-input flake with one attribute whose value is a deeply nested
# list literal. Parsing recurses once per level, so this overflows an 8 MiB
# stack somewhere past 100k levels; 200k is comfortably past that yet far below
# what the 64 MiB stack the `nix` CLI uses can hold.
depth=200000
flake="$store/flake"
mkdir -p "$flake"
{
  head -c "$depth" /dev/zero | tr '\0' '['
  printf '1'
  head -c "$depth" /dev/zero | tr '\0' ']'
} > "$flake/deep.nix"
cat > "$flake/flake.nix" <<'FLAKE'
{
  outputs = _: {
    checks.x86_64-linux.deep = import ./deep.nix;
  };
}
FLAKE

out="$( (
  ulimit -S -s 8192
  env \
    NIX_STORE_DIR="$store/store" \
    NIX_STATE_DIR="$store/state" \
    NIX_LOG_DIR="$store/log" \
    NIX_DATA_DIR="$store/data" \
    HOME="$store/home" \
    "$bin" --flake "path:$flake" --system x86_64-linux
) )"

# The attribute is a list, so a healthy run just ignores it and emits no line
# for it. A worker that overflowed reports it as a crashed-worker error
# instead; that error line is the regression.
if printf '%s\n' "$out" | grep -q '"type":"error"'; then
  echo "regression: a deep-but-finite attribute overflowed the worker stack" >&2
  printf '%s\n' "$out" | grep '"type":"error"' >&2
  exit 1
fi

# The run must still complete. The completion sentinel is the only proof the
# stream was not cut off partway.
if [ "$(printf '%s\n' "$out" | tail -1)" != '{"type":"done"}' ]; then
  echo "expected the run to complete with a done sentinel" >&2
  printf '%s\n' "$out" | tail -3 >&2
  exit 1
fi

echo "nix-ci-eval stack-depth test passed"
