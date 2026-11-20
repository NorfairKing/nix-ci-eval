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
    platforms = lib.platforms.linux ++ [ "aarch64-darwin" ];
  };
}
