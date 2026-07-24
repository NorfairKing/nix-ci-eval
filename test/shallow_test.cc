// Unit test for shallowIfRemoteGit: which flake references get a shallow fetch.
// Links the Nix libraries (unlike pure_test) because it parses real flake
// references; it never fetches, so it needs no network.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <nix/cmd/common-eval-args.hh>
#include <nix/fetchers/attrs.hh>
#include <nix/fetchers/fetch-settings.hh>
#include <nix/flake/flakeref.hh>

#include "args.hh"
#include "nix_eval.hh"

namespace {

int failures = 0;

// Whether the reference, once run through shallowIfRemoteGit, carries the
// shallow attribute Nix reads as "fetch only this commit".
bool becomesShallow(const std::string & ref, bool shallowArg) {
    auto parsed = nix::parseFlakeRef(nix::fetchSettings, ref,
                                     std::filesystem::current_path());
    auto result = nixcieval::shallowIfRemoteGit(parsed, shallowArg);
    auto attr = nix::fetchers::maybeGetBoolAttr(result.input.attrs, "shallow");
    return attr.has_value() && *attr;
}

void expectShallow(const std::string & what, const std::string & ref,
                   bool shallowArg, bool want) {
    bool got = becomesShallow(ref, shallowArg);
    if (got != want) {
        failures++;
        fprintf(stderr, "FAIL %s: got %s, want %s\n", what.c_str(),
                got ? "shallow" : "full", want ? "shallow" : "full");
    }
}

} // namespace

int main() {
    // Keep Nix's fetcher cache out of the caller's real one. The test never
    // fetches, but initialisation opens the cache directory.
    std::string cacheDir =
        (std::filesystem::temp_directory_path() / "nix-ci-eval-shallow-test-cache")
            .string();
    std::filesystem::create_directories(cacheDir);
    setenv("XDG_CACHE_HOME", cacheDir.c_str(), 1);
    setenv("HOME", cacheDir.c_str(), 1);

    nixcieval::Args args;
    nixcieval::initNixRuntime(args);

    // A remote git repository: shallow, over both transports it is reached by.
    expectShallow("https git ref is shallow",
                  "git+https://github.com/owner/repo", true, true);
    expectShallow("ssh git ref is shallow",
                  "git+ssh://git@github.com/owner/repo", true, true);

    // A local git repository hands nothing over a network, so it is left whole.
    expectShallow("file git ref is left whole", "git+file:///tmp/repo", true,
                  false);

    // github: and gitlab: are their own fetchers (a one-commit tarball), not
    // git, so shallowIfRemoteGit does not touch them.
    expectShallow("github ref is untouched", "github:owner/repo", true, false);
    expectShallow("gitlab ref is untouched", "gitlab:owner/repo", true, false);

    // A plain path is not fetched at all.
    expectShallow("path ref is untouched", "path:/tmp/repo", true, false);

    // --no-shallow (shallow = false) declines even for a remote git ref.
    expectShallow("no-shallow declines a remote git ref",
                  "git+https://github.com/owner/repo", false, false);

    if (failures == 0) {
        printf("all shallow-fetch assertions passed\n");
    }
    return failures == 0 ? 0 : 1;
}
