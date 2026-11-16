#!/usr/bin/env bash
# Compare two builds of nix-ci-eval on real flakes: do they discover the same
# thing, and which is faster.
#
#   bench/compare.sh <old-binary> <new-binary> <repetitions> <flake-ref>...
#
# Sameness is the part worth automating. Two runs of the same build do not
# produce the same stream: dependency edges are reported as soon as both ends
# are known, so which edges share a line depends on the order results arrive.
# What must not change is the set of outputs, their derivations, and the union
# of each output's dependencies, which is what a consumer is told to read
# anyway. That is what this compares.
#
# Timings come from runs interleaved old, new, old, new, so that both see the
# same machine. A loaded machine still makes them noisy, which is what the
# repetitions are for.
set -uo pipefail

if [ "$#" -lt 4 ]; then
  sed -n '2,5p' "$0" >&2
  exit 2
fi

old=$1
new=$2
reps=$3
shift 3

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# The stream reduced to what a consumer is promised: every job with its
# derivation, every error, and each attribute's dependencies unioned over
# every line that named it.
normalise='
  (map(select(.type == "job") | "job " + (.attrPath | join(".")) + " " + .drvPath) | sort)
  + (map(select(.type == "error") | "error " + (.attrPath | join("."))) | sort)
  + ([.[] | select(.type == "dependency")]
     | group_by(.attrPath)
     | map("dep " + (.[0].attrPath | join("."))
           + " -> " + ([.[].dependencies[] | join(".")] | unique | join(",")))
     | sort)
  | .[]'

run() { # binary output-file flake -> "milliseconds exit-code"
  local start end rc
  start=$(date +%s%N)
  "$1" --flake "$3" --system x86_64-linux > "$2" 2> "$2.err"
  rc=$?
  end=$(date +%s%N)
  echo "$(( (end - start) / 1000000 )) $rc"
}

for flake in "$@"; do
  echo "== $flake"
  for i in $(seq 1 "$reps"); do
    read -r oldMs oldRc <<< "$(run "$old" "$work/old.ndjson" "$flake")"
    read -r newMs newRc <<< "$(run "$new" "$work/new.ndjson" "$flake")"
    awk -v i="$i" -v o="$oldMs" -v orc="$oldRc" -v n="$newMs" -v nrc="$newRc" \
      'BEGIN { printf "  run %d: old %6.1fs (exit %d)   new %6.1fs (exit %d)   %.2fx\n", i, o/1000, orc, n/1000, nrc, (n > 0 ? o/n : 0) }'

    jq -s "$normalise" "$work/old.ndjson" > "$work/old.norm"
    jq -s "$normalise" "$work/new.ndjson" > "$work/new.norm"
    if diff -q "$work/old.norm" "$work/new.norm" > /dev/null; then
      echo "          same $(wc -l < "$work/old.norm") facts discovered"
    else
      echo "          DIFFERENT: $(diff "$work/old.norm" "$work/new.norm" | grep -c '^[<>]') of $(wc -l < "$work/old.norm") differ"
      diff "$work/old.norm" "$work/new.norm" | head -10
    fi
  done
done
