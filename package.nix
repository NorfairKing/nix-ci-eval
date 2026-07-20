# Build of the nix-ci-eval discovery tool.
{ lib
, stdenv
, meson
, ninja
, pkg-config
, nix
, nlohmann_json
}:
stdenv.mkDerivation {
  pname = "nix-ci-eval";
  version = "0.0.0";

  src = ./.;

  # nixpkgs' meson hook defaults to --buildtype=plain, which adds no
  # optimisation flags at all. Build the shipped binary optimised.
  mesonBuildType = "release";

  nativeBuildInputs = [ meson ninja pkg-config ];
  buildInputs = [ nix nlohmann_json ];

  doCheck = true;

  meta = {
    description = "Discover a flake's buildable outputs for one system, fast and in parallel";
    homepage = "https://github.com/NorfairKing/nix-ci-eval";
    mainProgram = "nix-ci-eval";
    # The per-worker memory ceiling reads ru_maxrss as KiB, which is a Linux
    # convention rather than a portable one.
    platforms = lib.platforms.linux;
  };
}
