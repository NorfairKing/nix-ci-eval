#!/usr/bin/env bash
# Ask fix (https://github.com/psyclyx/fix) and CppNix to perform nix-ci-eval's
# discovery on real flakes, and compare the derivations they arrive at.
#
#   bench/fix-differential.sh <fix-binary> <flake-ref>...
#
# A drvPath hashes the complete derivation that produced it — inputs,
# environment and builder, transitively — so two evaluators agreeing on every
# drvPath have agreed on everything that fed into every output. That is the
# only question worth asking of a replacement evaluator: nix-ci-eval exists to
# tell a consumer what to build, and an evaluator that names a derivation
# `nix build` would never produce is worse than one that is slow.
#
# Timings are printed but not asserted on. They are a single interleaved run
# on whatever machine this is, which is enough to see an order of magnitude
# and not enough to publish.
#
# Each flake is evaluated three times:
#
#   nix                     the oracle
#   fix --read-write-mode   fix with flake sources registered in the store
#   fix                     fix as it behaves by default
#
# The third run is here because the default is the one that silently
# disagrees. fix keeps fetched flake sources in its own cache rather than the
# store, so `self.outPath` is not a store path; the `src = ./.` idiom then
# resolves to a different source path than Nix computes, and every derivation
# built from it gets a drvPath that nothing else will ever produce. It is
# reported as success. --read-write-mode registers the source and the
# disagreement goes away, so the run is kept to hold that difference down
# rather than to fail the harness.
set -uo pipefail

if [ "$#" -lt 2 ]; then
  sed -n '2,4p' "$0" >&2
  exit 2
fi

fix=$1
shift

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
discover="$here/discover.nix"
system="${NIX_CI_EVAL_SYSTEM:-x86_64-linux}"

[ -x "$fix" ] || { echo "not an executable: $fix" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Flakes are an experimental feature in both evaluators, and the harness is
# run by people whose nix.conf may or may not enable them. Asking for them
# explicitly keeps the comparison from depending on the caller's config. The
# two evaluators want different sets: fix knows nothing of nix-command, and
# rejects a feature name it does not recognise.
nixFeatures="flakes nix-command"
fixFeatures="flakes"

# getFlake resolves a reference that is not in a lock file, which neither
# evaluator will do under pure evaluation.
run_nix() { # output-file flake -> milliseconds, via stdout
  local start end
  start=$(date +%s%N)
  nix-instantiate --eval --strict --json \
    --extra-experimental-features "$nixFeatures" \
    "$discover" --argstr flake "$2" --argstr system "$system" \
    > "$1" 2> "$1.err"
  end=$(date +%s%N)
  echo $(( (end - start) / 1000000 ))
}

run_fix() { # output-file flake extra-arg... -> milliseconds, via stdout
  local out=$1 flake=$2 start end
  shift 2
  start=$(date +%s%N)
  "$fix" eval --strict --json --impure \
    --extra-experimental-features "$fixFeatures" \
    "$@" \
    "$discover" --argstr flake "$flake" --argstr system "$system" \
    > "$out" 2> "$out.err"
  end=$(date +%s%N)
  echo $(( (end - start) / 1000000 ))
}

# A run that produced no output failed, and saying which of the three it was
# and why beats comparing an empty file against a full one.
report_failure() { # label output-file
  echo "  $1: FAILED, no output"
  sed -n '1,5p' "$2.err" | sed 's/^/      /'
}

status=0
for flake in "$@"; do
  echo "== $flake"

  nixMs=$(run_nix "$work/nix.json" "$flake")
  rwMs=$(run_fix "$work/fix-rw.json" "$flake" --read-write-mode)
  defMs=$(run_fix "$work/fix-default.json" "$flake")

  if [ ! -s "$work/nix.json" ]; then
    report_failure "nix" "$work/nix.json"
    status=1
    continue
  fi

  attrs=$(jq 'length' < "$work/nix.json")
  threw=$(jq '[.[] | select(. == null)] | length' < "$work/nix.json")
  echo "  $attrs attributes discovered, $threw of them threw"
  printf '  nix %6.2fs   fix --read-write-mode %6.2fs   fix %6.2fs\n' \
    "$(echo "$nixMs" | awk '{print $1/1000}')" \
    "$(echo "$rwMs" | awk '{print $1/1000}')" \
    "$(echo "$defMs" | awk '{print $1/1000}')"

  # Only --read-write-mode is held to agreeing, because it is the only mode a
  # replacement could be built on. The default is reported for contrast.
  if [ ! -s "$work/fix-rw.json" ]; then
    report_failure "fix --read-write-mode" "$work/fix-rw.json"
    status=1
  elif diff -q "$work/nix.json" "$work/fix-rw.json" > /dev/null; then
    echo "  fix --read-write-mode: agrees on every drvPath"
  else
    differing=$(jq -n --slurpfile a "$work/nix.json" --slurpfile b "$work/fix-rw.json" \
      '[$a[0] | keys[] | select($a[0][.] != $b[0][.])] | length')
    echo "  fix --read-write-mode: DISAGREES on $differing of $attrs"
    jq -n --slurpfile a "$work/nix.json" --slurpfile b "$work/fix-rw.json" \
      -r '$a[0] | keys[] | select($a[0][.] != $b[0][.])
          | "      " + . + "\n        nix " + ($a[0][.] | tostring)
                     + "\n        fix " + ($b[0][.] | tostring)' | head -15
    status=1
  fi

  if [ ! -s "$work/fix-default.json" ]; then
    report_failure "fix (default)" "$work/fix-default.json"
  elif diff -q "$work/nix.json" "$work/fix-default.json" > /dev/null; then
    echo "  fix (default): agrees on every drvPath"
  else
    differing=$(jq -n --slurpfile a "$work/nix.json" --slurpfile b "$work/fix-default.json" \
      '[$a[0] | keys[] | select($a[0][.] != $b[0][.])] | length')
    echo "  fix (default): disagrees on $differing of $attrs (expected; see the header)"
  fi
done

exit "$status"
