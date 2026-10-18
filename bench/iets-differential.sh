#!/usr/bin/env bash
# Ask iets (https://github.com/numtide/iets) and nix-ci-eval to discover the
# same flake's outputs, and compare the derivations they arrive at.
#
#   bench/iets-differential.sh <iets-binary> <flake-ref>...
#
# Where bench/fix-differential.sh swaps one evaluator for another underneath
# the same expression, this compares two whole tools: `iets eval-jobs` walks
# the attribute tree itself, so a replacement built on it would hand iets the
# walk as well as the evaluation. bench/jobs-root.nix is the root of that
# walk, written to match src/nix_eval.cc's selectExpr.
#
# A drvPath hashes the complete derivation that produced it, inputs,
# environment and builder, transitively, so two tools agreeing on every
# drvPath have agreed on everything that fed into every output. The
# attribute sets are compared too: a tool that discovered a different set of
# outputs disagrees just as much as one that reached a different derivation.
#
# iets takes only a local, locked flake: a path, or `git+file:///p?rev=SHA`.
# A `github:` or `git+https:` reference, which is what the deployed tool is
# given, is not something iets can fetch, so there is nothing to measure
# there. docs/iets-evaluator.md has the details.
#
# Timings are printed but not asserted on. They are a single interleaved run
# on whatever machine this is, which is enough to see an order of magnitude
# and not enough to publish.
set -uo pipefail

if [ "$#" -lt 2 ]; then
  sed -n '2,4p' "$0" >&2
  exit 2
fi

iets=$1
shift

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
nce="${NIX_CI_EVAL_BIN:-$here/../builddir/nix-ci-eval}"
system="${NIX_CI_EVAL_SYSTEM:-x86_64-linux}"

[ -x "$iets" ] || { echo "not an executable: $iets" >&2; exit 2; }
[ -x "$nce" ] || { echo "not an executable: $nce (set NIX_CI_EVAL_BIN)" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Both tools report a per-attribute failure rather than aborting, and the
# shapes differ: nix-ci-eval writes an `error` line, iets puts an `error` key
# on the job line. Either way the attribute was discovered and no derivation
# was reached, which is `null` here, as in bench/discover.nix.
nce_map='select(.type == "job" or .type == "error")
  | { (.attrPath | join("\u0000")): (.drvPath // null) }'
iets_map='select(has("attrPath"))
  | { (.attrPath | join("\u0000")): (if has("error") then null else .drvPath end) }'

run_nce() { # output-file flake -> milliseconds, via stdout
  local start end
  start=$(date +%s%N)
  "$nce" --flake "$2" --system "$system" --no-dependencies \
    > "$work/nce.ndjson" 2> "$1.err"
  end=$(date +%s%N)
  jq -S -s "map($nce_map) | add // {}" < "$work/nce.ndjson" > "$1"
  echo $(( (end - start) / 1000000 ))
}

# --force-recurse because nix-ci-eval recurses into every attribute set that
# is not a derivation, where eval-jobs otherwise wants recurseForDerivations.
# --no-instantiate because the comparison is about the derivations the two
# evaluators compute, not about who wrote a .drv to the store; iets needs a
# daemon to write one and nix-ci-eval does not.
run_iets() { # output-file flake -> milliseconds, via stdout
  local start end
  start=$(date +%s%N)
  "$iets" eval-jobs --flake "$2" \
    --select "($(cat "$here/jobs-root.nix")) \"$system\"" \
    --force-recurse --no-instantiate --quiet \
    > "$work/iets.ndjson" 2> "$1.err"
  end=$(date +%s%N)
  jq -S -s "map($iets_map) | add // {}" < "$work/iets.ndjson" > "$1"
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

  nceMs=$(run_nce "$work/nce.json" "$flake")
  ietsMs=$(run_iets "$work/iets.json" "$flake")

  if [ "$(jq 'length' < "$work/nce.json")" = 0 ]; then
    report_failure "nix-ci-eval" "$work/nce.json"
    status=1
    continue
  fi
  if [ "$(jq 'length' < "$work/iets.json")" = 0 ]; then
    report_failure "iets" "$work/iets.json"
    status=1
    continue
  fi

  attrs=$(jq 'length' < "$work/nce.json")
  threw=$(jq '[.[] | select(. == null)] | length' < "$work/nce.json")
  echo "  $attrs attributes discovered, $threw of them threw"
  awk -v n="$nceMs" -v f="$ietsMs" \
    'BEGIN { printf "  nix-ci-eval %6.2fs   iets %6.2fs   %.2fx\n", n/1000, f/1000, (f > 0 ? n/f : 0) }'

  if diff -q "$work/nce.json" "$work/iets.json" > /dev/null; then
    echo "  agrees on every drvPath"
  else
    jq -n --slurpfile a "$work/nce.json" --slurpfile b "$work/iets.json" -r '
      [($a[0] + $b[0] | keys[]) | select($a[0][.] != $b[0][.])] as $bad
      | "  DISAGREES on \($bad | length) of \($a[0] | length)",
        ($bad[:10][] | (. | gsub("\u0000"; ".")) as $shown
          | "      " + $shown
            + "\n        nix-ci-eval " + ($a[0][.] | tostring)
            + "\n        iets        " + ($b[0][.] | tostring))'
    status=1
  fi
done

exit "$status"
