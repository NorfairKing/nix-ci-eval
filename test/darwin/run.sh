#!/usr/bin/env bash
set -euo pipefail

root="$(mktemp -d)"
root="$(cd "$root" && pwd -P)"
cleanup() {
  if [ -n "${parent:-}" ]; then
    child="${child:-$(/usr/bin/pgrep -P "$parent" || true)}"
    kill -KILL "$parent" 2>/dev/null || true
  fi
  if [ -n "${child:-}" ]; then kill -KILL "$child" 2>/dev/null || true; fi
  chmod -R u+w "$root" 2>/dev/null || true
  rm -rf "$root"
}
trap cleanup EXIT

export HOME="$root/home" NIX_CONFIG='substituters ='
export NIX_STORE_DIR="$root/store" NIX_STATE_DIR="$root/state"
export NIX_LOG_DIR="$root/log" NIX_DATA_DIR="$root/data"
mkdir "$root/flake"
cat > "$root/flake/flake.nix" <<'FLAKE'
{
  outputs = _: builtins.trace "outputs forced" {
    packages.aarch64-darwin.default = derivation {
      name = "darwin-fixture";
      system = "aarch64-darwin";
      builder = "/bin/sh";
    };
    checks.aarch64-darwin =
      let fifo = builtins.getEnv "TEST_FIFO";
      in if fifo == "" then {} else {
        block = builtins.trace "blocked evaluation starting" (builtins.readFile fifo);
      };
  };
}
FLAKE

args=(--flake "path:$root/flake" --system aarch64-darwin --workers 1 --impure)
unset TEST_FIFO
"$NIX_CI_EVAL_BIN" "${args[@]}" > "$root/normal" 2> "$root/normal.err"
"$NIX_CI_EVAL_BIN" "${args[@]}" --max-memory-size 1 > "$root/low" 2> "$root/low.err"
grep -q '"type":"job"' "$root/normal"
[ "$(tail -1 "$root/normal")" = '{"type":"done"}' ]
cmp "$root/normal" "$root/low"
# With one slot, re-forcing the flake outputs proves the worker was recycled.
[ "$(grep -c '^trace: outputs forced$' "$root/normal.err")" -eq 1 ]
[ "$(grep -c '^trace: outputs forced$' "$root/low.err")" -gt 1 ]

# Leave the worker blocked on a FIFO with no writer, not idle on its IPC pipe.
mkfifo "$root/block"
TEST_FIFO="$root/block" "$NIX_CI_EVAL_BIN" "${args[@]}" > "$root/death" 2> "$root/death.err" &
parent=$!
for _ in {1..100}; do
  if grep -q '^trace: blocked evaluation starting$' "$root/death.err"; then break; fi
  sleep 0.05
done
cat "$root/death.err"
grep -q '^trace: blocked evaluation starting$' "$root/death.err"
child="$(/usr/bin/pgrep -P "$parent")"
[[ "$child" =~ ^[0-9]+$ ]]
kill -KILL "$parent"
wait "$parent" || [ "$?" -eq 137 ]
unset parent
for _ in {1..100}; do
  if ! kill -0 "$child" 2>/dev/null; then
    unset child
    [ ! -s "$root/death" ]
    echo 'nix-ci-eval Darwin memory and parent-death test passed'
    exit 0
  fi
  sleep 0.05
done
echo 'worker survived parent death' >&2
exit 1
