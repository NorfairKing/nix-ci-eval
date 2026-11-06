#!/usr/bin/env bash
# With import-from-derivation off, nix-ci-eval evaluates with iets and hands
# whatever iets cannot evaluate to Nix. This asserts that the result is
# exactly what a build without iets reports: the same jobs with the same
# drvPaths and cache status, the same errors word for word, and the same
# dependency edges. It also asserts that iets did the evaluating, so a run
# that quietly fell back to Nix does not pass for one that did not.
#
# iets has no store of its own and writes derivations through a daemon, so
# this starts a nix-daemon on a throwaway store. The flakes are served over a
# loopback git daemon, the way NixCI hands over a remote repository, and once
# more from a local path. Nothing here needs the network.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
with_iets="${NIX_CI_EVAL_BIN:-$here/../../builddir/nix-ci-eval}"
without_iets="${NIX_CI_EVAL_WITHOUT_IETS_BIN:-$here/../../builddir-nix/nix-ci-eval}"
probe="$here/ifd-probe.nix"
integration="${INTEGRATION_FLAKE:-$here/../integration/flake.nix}"

root="$(mktemp -d)"
cleanup() {
  [ -n "${nix_daemon_pid:-}" ] && kill "$nix_daemon_pid" 2>/dev/null || true
  [ -f "$root/git-daemon.pid" ] && kill "$(cat "$root/git-daemon.pid")" 2>/dev/null || true
  chmod -R u+w "$root" 2>/dev/null || true
  rm -rf "$root"
}
trap cleanup EXIT

export NIX_STORE_DIR="$root/store" NIX_STATE_DIR="$root/state" \
  NIX_LOG_DIR="$root/log" NIX_CONF_DIR="$root/etc" HOME="$root/home" \
  XDG_CACHE_HOME="$root/cache" TMPDIR="$root/tmp"
mkdir -p "$NIX_CONF_DIR" "$HOME" "$XDG_CACHE_HOME" "$TMPDIR"
cat > "$NIX_CONF_DIR/nix.conf" <<'CONF'
experimental-features = nix-command flakes
sandbox = false
substituters =
build-users-group =
CONF

sock="$root/daemon.sock"
NIX_DAEMON_SOCKET_PATH="$sock" nix-daemon --store local > "$root/nix-daemon.log" 2>&1 &
nix_daemon_pid=$!
for _ in $(seq 1 100); do [ -S "$sock" ] && break; sleep 0.1; done
export NIX_REMOTE="unix://$sock"

export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t

srv="$root/srv"
mkdir -p "$srv"
# Commit a flake.nix into a repository served by the git daemon, and print
# its rev.
serve() { # name flake.nix
  local work="$root/work/$1"
  mkdir -p "$work"
  cp "$2" "$work/flake.nix"
  chmod u+w "$work/flake.nix"
  git -C "$work" init -q -b master
  git -C "$work" add flake.nix
  git -C "$work" commit -q -m "$1"
  git clone -q --bare "$work" "$srv/$1.git"
  git -C "$work" rev-parse HEAD
}

integration_rev="$(serve integration "$integration")"

# A fresh salt keeps the probe's unbuilt outputs unbuilt.
sed "s/@salt@/$(date +%s%N)/g" "$probe" > "$root/probe.nix"
probe_rev="$(serve probe "$root/probe.nix")"

cat > "$root/deep.nix" <<'FLAKE'
{
  outputs = _: {
    packages.x86_64-linux = rec {
      deeper = { inherit deeper; };
      ok = derivation {
        name = "ok";
        system = "x86_64-linux";
        builder = "/bin/sh";
        args = [ "-c" "echo > $out" ];
      };
    };
  };
}
FLAKE
deep_rev="$(serve deep "$root/deep.nix")"

# iets refuses to parse a file nested past 50,000 levels, where Nix parses
# 400,000. Here that file is flake.nix itself, so iets fails the whole run and
# Nix has to walk the flake from the root.
{
  printf '{ outputs = _: { packages.x86_64-linux = { nested = '
  head -c 60000 /dev/zero | tr '\0' '['
  printf '1'
  head -c 60000 /dev/zero | tr '\0' ']'
  printf '; ok = derivation { name = "ok"; system = "x86_64-linux"; '
  # shellcheck disable=SC2016 # $out is for the builder, not this shell.
  printf 'builder = "/bin/sh"; args = [ "-c" "echo > $out" ]; }; }; }; }\n'
} > "$root/nested.nix"
nested_rev="$(serve nested "$root/nested.nix")"

port=9419
git daemon --reuseaddr --listen=127.0.0.1 --port="$port" --base-path="$srv" \
  --export-all --detach --pid-file="$root/git-daemon.pid" "$srv"
for _ in $(seq 1 50); do
  git ls-remote "git://127.0.0.1:$port/integration.git" >/dev/null 2>&1 && break
  sleep 0.1
done

# Half of the probe's IFD inputs are built first: Nix with IFD off refuses
# them anyway, and iets must too.
nix build --no-link \
  "git+file://$root/work/probe?rev=$probe_rev#prebuild.built" \
  "git+file://$root/work/probe?rev=$probe_rev#prebuild.builtNix" \
  "git+file://$root/work/probe?rev=$probe_rev#prebuild.builtSet"

# Everything a consumer can see, in an order that does not depend on timing.
# Edges for one attribute may arrive over several lines, so they are merged.
normalise='
  ([.[] | select(.type == "job" or .type == "error")
     | {type, attrPath, drvPath, isCached, error}] | sort_by(.attrPath)),
  ([.[] | select(.type == "dependency")]
     | group_by(.attrPath)
     | map({attrPath: .[0].attrPath, dependencies: (map(.dependencies) | add | unique)})),
  ([.[] | select(.type == "done")] | length)'

fail=0
compare() { # label flake-ref expected-iets-summary
  local label="$1" ref="$2" summary="$3"
  "$without_iets" --flake "$ref" --system x86_64-linux --no-import-from-derivation \
    > "$root/$label.nix.ndjson" 2> "$root/$label.nix.err"
  "$with_iets" --flake "$ref" --system x86_64-linux --no-import-from-derivation \
    > "$root/$label.iets.ndjson" 2> "$root/$label.iets.err"
  if ! diff -u <(jq -s "$normalise" < "$root/$label.nix.ndjson") \
               <(jq -s "$normalise" < "$root/$label.iets.ndjson"); then
    echo "$label: iets and Nix disagree" >&2
    sed 's/^/    /' "$root/$label.iets.err" >&2
    fail=1
  fi
  if ! grep -qxF "nix-ci-eval: $summary" "$root/$label.iets.err"; then
    echo "$label: expected '$summary' on stderr, got:" >&2
    sed 's/^/    /' "$root/$label.iets.err" >&2
    fail=1
  fi
}

compare integration-remote "git+git://127.0.0.1:$port/integration.git?ref=master&rev=$integration_rev" \
  "iets reported 7 jobs and handed 1 attributes to Nix"
compare integration-local "git+file://$root/work/integration?rev=$integration_rev" \
  "iets reported 7 jobs and handed 1 attributes to Nix"
compare probe "git+git://127.0.0.1:$port/probe.git?ref=master&rev=$probe_rev" \
  "iets reported 4 jobs and handed 13 attributes to Nix"
compare deep "git+git://127.0.0.1:$port/deep.git?ref=master&rev=$deep_rev" \
  "iets reported 1 jobs and handed 1 attributes to Nix"
compare nested "git+git://127.0.0.1:$port/nested.git?ref=master&rev=$nested_rev" \
  "iets reported 0 jobs and handed 0 attributes to Nix"
if ! grep -q "^nix-ci-eval: iets did not finish" "$root/nested.iets.err"; then
  echo "nested: expected iets to fail and Nix to take over" >&2
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "nix-ci-eval iets test passed"
