#!/usr/bin/env bash
# Integration test: run the built nix-ci-eval against the hermetic fixture
# flake and compare its output to a golden.
#
# The tool needs a writable Nix store to instantiate the fixture's derivations,
# but must not touch the caller's real store, so it is pointed at a throwaway
# one under a temp dir. The fixture has no inputs, so this is fully offline.
#
# drvPaths carry a store-dir prefix and a hash that shift with the store
# location and the Nix version, and evaluation-error text carries file paths
# and Nix's exact formatting; none of that is the tool's contract. So the
# output is normalised to the parts that are (type, attribute path, dependency
# edges, derivation name, cache status) before it is compared.
#
# Set UPDATE_GOLDEN=1 to rewrite the golden from the current output.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="${NIX_CI_EVAL_BIN:-$here/../../builddir/nix-ci-eval}"
golden="$here/expected.txt"

store="$(mktemp -d)"
# The fixture is copied in as a read-only store path, so make it writable
# before removing it.
trap 'chmod -R u+w "$store" 2>/dev/null || true; rm -rf "$store"' EXIT

normalise='
  if .type == "job" then
    "job " + (.attrPath | join(".")) + " "
      + (.drvPath | sub(".*/[0-9a-z]+-"; "") | sub("\\.drv$"; ""))
      + " cached=" + (.isCached | tostring)
  elif .type == "dependency" then
    "dep " + (.attrPath | join("."))
      + " -> " + (.dependencies | map(join(".")) | join(","))
  elif .type == "error" then
    "error " + (.attrPath | join("."))
  elif .type == "done" then
    "done"
  else
    "UNKNOWN " + tostring
  end'

raw="$store/raw.ndjson"
env \
  NIX_STORE_DIR="$store/store" \
  NIX_STATE_DIR="$store/state" \
  NIX_LOG_DIR="$store/log" \
  NIX_DATA_DIR="$store/data" \
  HOME="$store/home" \
  "$bin" --flake "path:$here" --system x86_64-linux > "$raw"

actual="$(jq -r "$normalise" < "$raw" | LC_ALL=C sort)"

if [ "${UPDATE_GOLDEN:-}" = 1 ]; then
  printf '%s\n' "$actual" > "$golden"
  echo "updated $golden"
  exit 0
fi

if ! diff -u "$golden" <(printf '%s\n' "$actual"); then
  echo "nix-ci-eval integration output does not match the golden." >&2
  echo "Re-run with UPDATE_GOLDEN=1 to accept the new output." >&2
  exit 1
fi

# An edge is reported as soon as both of its ends are known, so at least one
# arrives before the last job does. Batching them until every job was in would
# still pass the golden, which is sorted, so this is what holds the difference.
last_job="$(jq -r '.type' < "$raw" | grep -n '^job$' | tail -1 | cut -d: -f1)"
first_edge="$(jq -r '.type' < "$raw" | grep -n '^dependency$' | head -1 | cut -d: -f1)"
if [ -z "$first_edge" ] || [ "$first_edge" -ge "$last_job" ]; then
  echo "expected an edge before the last job, so edges are not batched to the end" >&2
  jq -r '.type' < "$raw" | cat -n >&2
  exit 1
fi

# An edge names attributes that a consumer has to have been told about already,
# because it cannot order a build it has never heard of. So every end of every
# edge must have been announced as a job earlier in the stream.
if ! jq -r '
  if .type == "job" then
    "job " + (.attrPath | join("."))
  elif .type == "dependency" then
    (["dep " + (.attrPath | join("."))]
      + (.dependencies | map("dep " + join(".")))) | .[]
  else
    empty
  end' < "$raw" |
  awk '
    $1 == "job" { seen[$2] = 1; next }
    $1 == "dep" && !($2 in seen) {
      print "  edge names " $2 " before its job line" > "/dev/stderr"
      bad = 1
    }
    END { exit bad ? 1 : 0 }'; then
  echo "every end of an edge must be announced as a job before the edge names it" >&2
  exit 1
fi

# The golden is sorted, so it cannot see ordering. The completion sentinel is
# only worth anything if it is genuinely the last thing written: a consumer
# that has seen it must be able to conclude it has seen everything.
if [ "$(jq -r '.type' < "$raw" | tail -1)" != "done" ]; then
  echo "the last line must be the completion sentinel" >&2
  jq -r '.type' < "$raw" | tail -3 >&2
  exit 1
fi
if [ "$(jq -r 'select(.type == "done")' < "$raw" | grep -c '^{')" != 1 ]; then
  echo "there must be exactly one completion sentinel" >&2
  exit 1
fi

# The command-line surface, which the golden above cannot see: asking for the
# usage text is an answer on stdout, while a mistake is an error on stderr.
# Getting these the wrong way round breaks `--help | less` and any caller
# running under `set -e`.
help_out="$("$bin" --help 2>/dev/null)" || {
  echo "--help must exit 0" >&2
  exit 1
}
if [ -z "$help_out" ]; then
  echo "--help must write the usage text to stdout" >&2
  exit 1
fi
if [ -n "$("$bin" --help 2>&1 >/dev/null)" ]; then
  echo "--help must write nothing to stderr" >&2
  exit 1
fi

for bad in --bogus ""; do
  if "$bin" $bad >/dev/null 2>&1; then
    echo "a bad invocation ($bad) must exit non-zero" >&2
    exit 1
  fi
  if [ -z "$("$bin" $bad 2>&1 >/dev/null)" ]; then
    echo "a bad invocation ($bad) must explain itself on stderr" >&2
    exit 1
  fi
done

echo "nix-ci-eval integration test passed"
