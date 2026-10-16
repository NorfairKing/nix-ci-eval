#!/usr/bin/env bash
# Ask fix (https://github.com/psyclyx/fix) and CppNix to perform nix-ci-eval's
# discovery on real flakes, and compare the derivations they arrive at.
#
#   bench/fix-differential.sh <fix-binary> <flake-ref>...
#
# A drvPath hashes the complete derivation that produced it, inputs,
# environment and builder, transitively, so two evaluators agreeing on every
# drvPath have agreed on everything that fed into every output. That is the
# only question worth asking of a replacement evaluator: nix-ci-eval exists to
# tell a consumer what to build, and an evaluator that names a derivation
# `nix build` would never produce is worse than one that is slow.
#
# fix is run with --read-write-mode, which is the only mode a replacement
# could be built on. Without it fix leaves the fetched flake source in its own
# cache instead of the store, `self.outPath` is not a store path, and every
# derivation reached through `src = ./.` gets a drvPath that nothing else will
# ever produce, reported as success. docs/fix-evaluator.md has the details.
#
# Which reference you pass matters, and a disagreement may be telling you
# about the reference rather than about the attribute. A `github:` or `git+`
# reference agrees today. A bare path or a `path:` reference does not: Nix
# names the source store path `source` whatever the directory is called, and
# fix names it after the directory, so everything built from it differs.
#
# Timings are printed but not asserted on. They are a single interleaved run
# on whatever machine this is, which is enough to see an order of magnitude
# and not enough to publish.
set -uo pipefail

if [ "$#" -lt 2 ]; then
  sed -n '2,5p' "$0" >&2
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
# two want different sets: fix knows nothing of nix-command and rejects a
# feature name it does not recognise.
nixFeatures="flakes nix-command"
fixFeatures="flakes"

# getFlake resolves a reference that no lock file pins, which neither
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

run_fix() { # output-file flake -> milliseconds, via stdout
  local start end
  start=$(date +%s%N)
  "$fix" eval --strict --json --impure --read-write-mode \
    --extra-experimental-features "$fixFeatures" \
    "$discover" --argstr flake "$2" --argstr system "$system" \
    > "$1" 2> "$1.err"
  end=$(date +%s%N)
  echo $(( (end - start) / 1000000 ))
}

# A run that produced no output failed, and saying which one and why beats
# comparing an empty file against a full one.
report_failure() { # label output-file
  echo "  $1: FAILED, no output"
  sed -n '1,5p' "$2.err" | sed 's/^/      /'
}

status=0
for flake in "$@"; do
  echo "== $flake"

  nixMs=$(run_nix "$work/nix.json" "$flake")
  fixMs=$(run_fix "$work/fix.json" "$flake")

  if [ ! -s "$work/nix.json" ]; then
    report_failure "nix" "$work/nix.json"
    status=1
    continue
  fi
  if [ ! -s "$work/fix.json" ]; then
    report_failure "fix" "$work/fix.json"
    status=1
    continue
  fi

  attrs=$(jq 'length' < "$work/nix.json")
  threw=$(jq '[.[] | select(. == null)] | length' < "$work/nix.json")
  echo "  $attrs attributes discovered, $threw of them threw"
  awk -v n="$nixMs" -v f="$fixMs" \
    'BEGIN { printf "  nix %6.2fs   fix %6.2fs   %.2fx\n", n/1000, f/1000, (f > 0 ? n/f : 0) }'

  if diff -q "$work/nix.json" "$work/fix.json" > /dev/null; then
    echo "  agrees on every drvPath"
  else
    # Over the keys of both, not just the oracle's: an evaluator that
    # discovered a different set of attributes disagrees just as much as one
    # that reached a different derivation, and reporting nothing in that case
    # would be the most misleading answer available.
    jq -n --slurpfile a "$work/nix.json" --slurpfile b "$work/fix.json" -r '
      [($a[0] + $b[0] | keys[]) | select($a[0][.] != $b[0][.])] as $bad
      | "  DISAGREES on \($bad | length) of \($a[0] | length)",
        ($bad[:10][] | "      " + .
          + "\n        nix " + ($a[0][.] | tostring)
          + "\n        fix " + ($b[0][.] | tostring))'
    status=1
  fi
done

exit "$status"
