#!/usr/bin/env bash
# Asserts that an input evaluation fetched is reported as an `input` line, and
# that one it never used is not.
# Also asserts that it is reported as soon as it is fetched, rather than once
# evaluation is over.
#
# Locking a flake copies every input into the store, which would make the
# unused one look fetched. So the flake is locked against one throwaway store
# and evaluated against another, which holds only what evaluation fetched.
set -euo pipefail

bin="${NIX_CI_EVAL_BIN:?}"

tmp="$(mktemp -d)"
# Store paths are read-only, so make them writable before removing them.
trap 'chmod -R u+w "$tmp" 2>/dev/null || true; rm -rf "$tmp"' EXIT

with_store() {
  local store="$1"
  shift
  env \
    NIX_STORE_DIR="$store/store" \
    NIX_STATE_DIR="$store/state" \
    NIX_LOG_DIR="$store/log" \
    NIX_DATA_DIR="$store/data" \
    HOME="$tmp/home" \
    NIX_CONFIG="substituters =" \
    "$@"
}

mkdir -p "$tmp/used" "$tmp/unused" "$tmp/unused-flake" "$tmp/flake"
echo used > "$tmp/used/name"
echo unused > "$tmp/unused/name"
# A flake input, because locking could read the flake of one to lock its own
# inputs, which would fetch it whether evaluation used it or not.
cat > "$tmp/unused-flake/flake.nix" <<NIX
{
  outputs = { ... }: { };
}
NIX
cat > "$tmp/flake/flake.nix" <<NIX
{
  inputs.used = { url = "path:$tmp/used"; flake = false; };
  inputs.unused = { url = "path:$tmp/unused"; flake = false; };
  inputs.unused-flake.url = "path:$tmp/unused-flake";
  outputs = { used, ... }: {
    packages.x86_64-linux.default = derivation {
      name = "pkg";
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "echo \${builtins.readFile "\${used}/name"} > \$out" ];
    };
    # Takes a moment to evaluate and uses no input, so the input the other
    # attribute fetched is known well before this is.
    packages.x86_64-linux.slow =
      let
        range = builtins.genList (i: i) 8000;
        sum = builtins.foldl' (acc: _: acc + builtins.foldl' builtins.add 0 range) 0 range;
      in
      derivation {
        name = "slow";
        system = "x86_64-linux";
        builder = "/bin/sh";
        args = [ "-c" "echo \${toString sum} > \$out" ];
      };
  };
}
NIX

with_store "$tmp/lock-store" \
  nix --extra-experimental-features 'nix-command flakes' \
  flake lock "path:$tmp/flake"

raw="$tmp/raw.ndjson"
with_store "$tmp/eval-store" \
  "$bin" --flake "path:$tmp/flake" --system x86_64-linux > "$raw"

used_nar_hash="$(jq -r '.nodes.used.locked.narHash' < "$tmp/flake/flake.lock")"
expected="$(with_store "$tmp/eval-store" \
  nix-store --print-fixed-path --recursive sha256 "$used_nar_hash" source)"
actual="$(jq -r 'select(.type == "input") | .storePath' < "$raw")"

if [ "$actual" != "$expected" ]; then
  echo "expected exactly the used input to be reported" >&2
  echo "expected: $expected" >&2
  echo "actual:   $actual" >&2
  cat "$raw" >&2
  exit 1
fi

first_input="$(jq -r .type < "$raw" | grep -n '^input$' | head -1 | cut -d: -f1)"
slow_job="$(jq -r 'if .type == "job" then .attrPath | join(".") else .type end' < "$raw" | grep -n '^packages.x86_64-linux.slow$' | cut -d: -f1)"
if [ "$first_input" -ge "$slow_job" ]; then
  echo "expected the input to be reported before the slow attribute's job" >&2
  cat "$raw" >&2
  exit 1
fi

echo "nix-ci-eval inputs test passed"
