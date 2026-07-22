# nix-ci-eval

A small, fast, parallel discoverer of a flake's buildable outputs for one
system.

It discovers the following outputs, scoped to a given system:

- `packages`
- `checks`
- `devShells`
- `formatter`
- `devShell`

and streams newline-delimited JSON, one object per discovered output:

```console
$ nix run github:NorfairKing/nix-ci-eval -- --flake github:NorfairKing/autodocodec --system x86_64-linux
```

Every object carries a `type`. A `job` (prettified here; each is really a
single line):

```json
{
  "type": "job",
  "attrPath": ["packages", "x86_64-linux", "autodocodec"],
  "drvPath": "/nix/store/8mwsxkyklmr52jclm6q3ax0l9v6929d0-autodocodec-0.6.0.0.drv",
  "isCached": false
}
```

An attribute that fails to evaluate becomes an `error` line rather than
aborting the run:

```json
{
  "type": "error",
  "attrPath": ["checks", "x86_64-linux", "broken"],
  "error": "error: undefined variable 'foo'"
}
```

A `dependency` edge to another discovered output, reported as soon as both of
its ends are known, so these arrive mixed in with the jobs:

```json
{
  "type": "dependency",
  "attrPath": ["packages", "x86_64-linux", "autodocodec-swagger2"],
  "dependencies": [["packages", "x86_64-linux", "autodocodec"]]
}
```

One attribute can appear in several of these, each naming a few of the things
it depends on, so take their union rather than the last one you saw.
Dependencies are transitive: if `a` depends on `b` and `b` on `c`, then `a`
depends on `c` too and that edge is reported.

The last line is always this, and only ever appears once:

```json
{
  "type": "done"
}
```

Read it as the only proof that you have the whole stream. Output that stops
without it was cut short, which otherwise looks exactly like a flake with
fewer outputs.

## Fetching

A `git+` flake reference to a remote repository is fetched shallow, so only the
commit being evaluated comes over the network instead of everything it descends
from. In exchange, Nix cannot count revisions it did not fetch, so `self.revCount`
is missing and a flake that reads it fails to evaluate. Local repositories keep
their `revCount`, since nothing they hand over crosses a network and there is
little to win by cutting their history short. `github:` and `gitlab:` references
are untouched too: they fetch a tarball of the one commit and never involved git
to begin with.

More invocations:

```console
$ nix run github:NorfairKing/nix-ci-eval -- --flake . --system x86_64-linux --no-dependencies
$ nix run github:NorfairKing/nix-ci-eval -- --flake . --system x86_64-linux --impure
```

Run `nix run github:NorfairKing/nix-ci-eval -- --help` for all flags.
