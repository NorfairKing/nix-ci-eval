#!/usr/bin/env bash
# Regression test: what a flake's outputs share is evaluated a fixed number of
# times, however many workers the run uses.
#
# Every worker used to build its own evaluator, which meant loading the flake
# and forcing everything its outputs share (in a real flake: nixpkgs, the
# overlays, the package set) from scratch, once per worker. That cost is the
# dominant one on a real flake and it does not amortise, so a pool of N paid it
# N times.
#
# The fixture flake traces when its substrate is forced. A trace is printed by
# the process that forces the thunk, and only the first time, so counting the
# trace lines counts the evaluations of the substrate the run really performed.
# Running twice with different pool sizes is what separates "shared" from
# "cheap": the count has to be the same both times.
#
# The fixture's attributes are deliberately slow, so that the run has a reason
# to grow its pool to the size it is allowed; a run that stays at one worker
# would force the substrate once whichever way this works.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="${NIX_CI_EVAL_BIN:-$here/../../builddir/nix-ci-eval}"

store="$(mktemp -d)"
trap 'chmod -R u+w "$store" 2>/dev/null || true; rm -rf "$store"' EXIT

# How many times the substrate was forced by a run with this many workers, and
# how many jobs that run reported.
forcings() {
  local workers=$1
  local out="$store/out-$workers.ndjson"
  local err="$store/err-$workers.txt"
  env \
    NIX_STORE_DIR="$store/store" \
    NIX_STATE_DIR="$store/state" \
    NIX_LOG_DIR="$store/log" \
    NIX_DATA_DIR="$store/data" \
    HOME="$store/home" \
    "$bin" --flake "path:$here" --system x86_64-linux --workers "$workers" \
    > "$out" 2> "$err"

  # A count is only worth anything if the run actually discovered everything:
  # a run that died early would force the substrate once and report nothing.
  local jobs
  jobs="$(grep -c '"type":"job"' "$out" || true)"
  if [ "$jobs" != 40 ]; then
    echo "expected 40 jobs from a run with $workers workers, got $jobs" >&2
    exit 1
  fi
  if [ "$(tail -1 "$out")" != '{"type":"done"}' ]; then
    echo "a run with $workers workers did not complete" >&2
    exit 1
  fi
  grep -c 'SUBSTRATE-FORCED' "$err" || true
}

small="$(forcings 2)"
large="$(forcings 8)"

if [ "$small" != "$large" ]; then
  echo "the substrate was forced $small times with 2 workers and $large times with 8:" >&2
  echo "it is being evaluated per worker rather than shared between them" >&2
  exit 1
fi

# Bounded as well as constant: one worker has to force it before there is an
# attribute worth sharing, and the run forces it once more where every later
# worker can inherit it.
if [ "$small" -gt 2 ]; then
  echo "the substrate was forced $small times, which is more than the two" >&2
  echo "evaluations a run needs to make it shareable" >&2
  exit 1
fi

echo "nix-ci-eval substrate-sharing test passed ($small evaluations at both pool sizes)"
