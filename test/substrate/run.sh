#!/usr/bin/env bash
# Regression test: a run must force the flake's shared evaluation substrate
# once, not once per evaluation worker.
#
# Every worker used to construct its own evaluator, which meant loading the
# flake and forcing everything its outputs share (in a real flake: nixpkgs,
# the overlays, the package set) from scratch, once per worker. That cost is
# the dominant one on a real flake and it does not amortise, so a pool of N
# paid it N times.
#
# The fixture flake traces when its substrate is forced. A trace is printed by
# the process that forces the thunk, and only the first time, so counting the
# trace lines counts the evaluations of the substrate the run really performed.
#
# The fixture's attributes are deliberately slow, so the run has a reason to
# grow its pool past one worker; a run that stays at one worker would force the
# substrate once whether or not anything is shared, and prove nothing.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="${NIX_CI_EVAL_BIN:-$here/../../builddir/nix-ci-eval}"

store="$(mktemp -d)"
trap 'chmod -R u+w "$store" 2>/dev/null || true; rm -rf "$store"' EXIT

out="$store/out.ndjson"
err="$store/err.txt"
env \
  NIX_STORE_DIR="$store/store" \
  NIX_STATE_DIR="$store/state" \
  NIX_LOG_DIR="$store/log" \
  NIX_DATA_DIR="$store/data" \
  HOME="$store/home" \
  "$bin" --flake "path:$here" --system x86_64-linux --workers 4 \
  > "$out" 2> "$err"

forced="$(grep -c 'SUBSTRATE-FORCED' "$err" || true)"
if [ "$forced" != 1 ]; then
  echo "the evaluation substrate was forced $forced times; it must be forced once" >&2
  exit 1
fi

# The count above is only worth anything if the run actually discovered
# everything: a run that died early would force the substrate once and report
# nothing.
jobs="$(grep -c '"type":"job"' "$out" || true)"
if [ "$jobs" != 40 ]; then
  echo "expected 40 jobs, got $jobs" >&2
  exit 1
fi
if [ "$(tail -1 "$out")" != '{"type":"done"}' ]; then
  echo "expected the run to complete with a done sentinel" >&2
  exit 1
fi

echo "nix-ci-eval substrate-sharing test passed"
