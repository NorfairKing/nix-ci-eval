#!/usr/bin/env bash
# Ask whether iets refuses exactly what Nix refuses with
# import-from-derivation off, over bench/ifd-probe/flake.nix.
#
#   bench/ifd-probe.sh <iets-binary>
#
# iets never builds during evaluation, so for a repository that has turned
# IFD off it could stand in for Nix without losing anything, provided the two
# fail on the same attributes. This builds half of the probe's IFD inputs
# first, so that an evaluator which reads an output merely because it is
# already in the store disagrees with Nix, which refuses regardless.
set -euo pipefail

if [ "$#" -ne 1 ]; then
  sed -n '5p' "$0" >&2
  exit 2
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(mktemp -d)"
trap 'rm -rf "$repo"' EXIT

# A fresh salt per run keeps the probe's unbuilt outputs unbuilt.
sed "s/@salt@/$(date +%s%N)/g" "$here/ifd-probe/flake.nix" > "$repo/flake.nix"
git -C "$repo" init -q
git -C "$repo" add flake.nix
git -C "$repo" -c user.name=probe -c user.email=probe@localhost commit -qm probe
rev="$(git -C "$repo" rev-parse HEAD)"
flake="git+file://$repo?rev=$rev"

nix build --no-link "$flake#prebuild.built" "$flake#prebuild.builtNix" "$flake#prebuild.builtSet"

"$here/iets-differential.sh" --no-import-from-derivation "$1" "$flake"
