#!/usr/bin/env bash
# Behavioural test for shallow fetching: serve a two-commit repository over a
# loopback git daemon, evaluate it with nix-ci-eval, and assert that the
# default fetch pulled only the one commit while --no-shallow pulled both.
#
# The daemon runs on 127.0.0.1, so this needs no network. git+file:// would be
# simpler but is deliberately never fetched shallow (a local repo crosses no
# network), so a real transport is required to exercise the behaviour at all.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="${NIX_CI_EVAL_BIN:-$here/../../builddir/nix-ci-eval}"

root="$(mktemp -d)"
trap 'chmod -R u+w "$root" 2>/dev/null || true; [ -n "${daemon_pid:-}" ] && kill "$daemon_pid" 2>/dev/null || true; rm -rf "$root"' EXIT

# A no-input flake so evaluation stays offline, with two commits so "one" and
# "both" are distinguishable.
work="$root/work"
mkdir -p "$work"
cat > "$work/flake.nix" <<'FLAKE'
{
  outputs = _: {
    packages.x86_64-linux.default = derivation {
      name = "shallow-fixture";
      system = "x86_64-linux";
      builder = "/bin/sh";
      args = [ "-c" "echo hi > $out" ];
    };
  };
}
FLAKE
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
git -C "$work" init -q -b master
git -C "$work" add flake.nix
git -C "$work" commit -q -m one
echo "# second commit" >> "$work/flake.nix"
git -C "$work" add flake.nix
git -C "$work" commit -q -m two
rev="$(git -C "$work" rev-parse HEAD)"

# Serve it, bare, over a loopback git daemon.
srv="$root/srv"
mkdir -p "$srv"
git clone -q --bare "$work" "$srv/repo.git"
touch "$srv/repo.git/git-daemon-export-ok"
port=9418
git daemon --reuseaddr --listen=127.0.0.1 --port="$port" \
  --base-path="$srv" --export-all --detach --pid-file="$root/daemon.pid" "$srv"
daemon_pid="$(cat "$root/daemon.pid")"

# Wait for the daemon to accept connections.
for _ in $(seq 1 50); do
  if git ls-remote "git://127.0.0.1:$port/repo.git" >/dev/null 2>&1; then break; fi
  sleep 0.1
done

flake_url="git+git://127.0.0.1:$port/repo.git?ref=master&rev=$rev"

# Run nix-ci-eval with a fresh fetcher cache and throwaway store, and report
# how many commits its fetch left in that cache.
commits_fetched() {
  local extra_arg="$1"
  local cache store
  cache="$(mktemp -d)"
  store="$(mktemp -d)"
  env \
    XDG_CACHE_HOME="$cache" \
    HOME="$root/home" \
    NIX_STORE_DIR="$store/store" \
    NIX_STATE_DIR="$store/state" \
    NIX_LOG_DIR="$store/log" \
    NIX_DATA_DIR="$store/data" \
    "$bin" --flake "$flake_url" --system x86_64-linux $extra_arg > "$cache/out.ndjson"

  # The stream must be complete either way.
  if [ "$(tail -n1 "$cache/out.ndjson")" != '{"type":"done"}' ]; then
    echo "nix-ci-eval did not finish cleanly for '$extra_arg'" >&2
    cat "$cache/out.ndjson" >&2
    exit 1
  fi

  # The fetched repository lives in Nix's git cache; count its commits. This
  # cache is fresh per run, so there is exactly one.
  local repo
  repo="$(find "$cache/nix/gitv3" -mindepth 1 -maxdepth 1 -type d | head -n1)"
  if [ -z "$repo" ]; then
    echo "no fetched repository under the git cache for '$extra_arg'" >&2
    find "$cache/nix/gitv3" >&2 || true
    exit 1
  fi
  echo "$(basename "$repo")|$(git -C "$repo" rev-list --count "$rev")"
}

default_result="$(commits_fetched '')"
noshallow_result="$(commits_fetched '--no-shallow')"

default_dir="${default_result%|*}"
default_commits="${default_result#*|}"
noshallow_dir="${noshallow_result%|*}"
noshallow_commits="${noshallow_result#*|}"

fail=0

# Default: shallow. Nix names a shallow cache directory with a -shallow suffix,
# and the fetch brings down only the one commit.
case "$default_dir" in
  *-shallow) ;;
  *) echo "default fetch used a non-shallow cache dir: $default_dir" >&2; fail=1 ;;
esac
if [ "$default_commits" != "1" ]; then
  echo "default fetch pulled $default_commits commits, expected 1" >&2
  fail=1
fi

# --no-shallow: the whole history, in a non-shallow cache directory.
case "$noshallow_dir" in
  *-shallow) echo "--no-shallow still used a shallow cache dir: $noshallow_dir" >&2; fail=1 ;;
esac
if [ "$noshallow_commits" != "2" ]; then
  echo "--no-shallow fetch pulled $noshallow_commits commits, expected 2" >&2
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi

echo "nix-ci-eval shallow-fetch test passed"
