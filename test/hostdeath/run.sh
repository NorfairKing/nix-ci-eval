#!/usr/bin/env bash
# Regression test: the run survives losing the process its workers are forked
# from.
#
# Workers forked by the substrate host die with it, by design, so that nothing
# outlives the run. The run itself must not: it re-forks its own workers, hands
# their unfinished attributes to them again, and still reports every output.
# Without that, losing the host would silently cost a handful of attributes,
# which looks exactly like a flake that does not have them.
#
# The host is killed rather than asked to stop, because a crash is the way it
# is actually lost.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="${NIX_CI_EVAL_BIN:-$here/../../builddir/nix-ci-eval}"
fixture="$here/../substrate"

store="$(mktemp -d)"
trap 'chmod -R u+w "$store" 2>/dev/null || true; rm -rf "$store"' EXIT

out="$store/out.ndjson"
env \
  NIX_STORE_DIR="$store/store" \
  NIX_STATE_DIR="$store/state" \
  NIX_LOG_DIR="$store/log" \
  NIX_DATA_DIR="$store/data" \
  HOME="$store/home" \
  "$bin" --flake "path:$fixture" --system x86_64-linux --workers 4 \
  > "$out" 2> "$store/err.txt" &
run=$!

# A reported job is proof that the pool is working, which is when losing the
# host costs something.
for _ in $(seq 1 600); do
  if grep -q '"type":"job"' "$out" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

# Every worker is a child of the host, so while there is a host the run has
# exactly one child of its own. More than one means the run had already fallen
# back to forking its own workers, and this test would be killing one of those
# instead of the host it means to.
children="$(pgrep -P "$run" | wc -l)"
if [ "$children" != 1 ]; then
  echo "expected the run to have one child (the substrate host), got $children" >&2
  kill -9 "$run" 2>/dev/null || true
  exit 1
fi
kill -9 "$(pgrep -P "$run")"

wait "$run"

jobs="$(grep -c '"type":"job"' "$out" || true)"
if [ "$jobs" != 40 ]; then
  echo "expected 40 jobs after losing the host, got $jobs" >&2
  exit 1
fi
if [ "$(tail -1 "$out")" != '{"type":"done"}' ]; then
  echo "expected the run to complete with a done sentinel" >&2
  tail -3 "$out" >&2
  exit 1
fi

echo "nix-ci-eval host-death test passed"
