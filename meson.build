project(
  'nix-ci-eval',
  'cpp',
  version : '0.0.0',
  # Optimised by default because -Werror is on and glibc's _FORTIFY_SOURCE
  # warns when it is compiled without optimisation, which would make a plain
  # `meson setup builddir` fail. c++23 is what the nix pkg-config files force
  # on us anyway, so declaring anything else here would be a fiction.
  default_options : [
    'cpp_std=c++23',
    'buildtype=release',
    'warning_level=2',
    'werror=true',
  ],
)

# The Nix C++ API is not stable across releases, and the split headers this
# uses (nix/expr, nix/store, nix/util) only exist from 2.28. Saying so turns
# "too old" into one clear pkg-config error rather than a wall of missing
# includes.
nix_version = '>=2.28'

nix_deps = [
  dependency('nix-expr', version : nix_version),
  dependency('nix-store', version : nix_version),
  dependency('nix-flake', version : nix_version),
  dependency('nix-main', version : nix_version),
  dependency('nix-cmd', version : nix_version),
  dependency('nix-fetchers', version : nix_version),
  dependency('nix-util', version : nix_version),
  dependency('nlohmann_json'),
  dependency('threads'),
]

# Pure helpers and IO, free of the Nix libraries so they can be unit-tested on
# their own.
core_lib = static_library(
  'nix-ci-eval-core',
  ['src/pure.cc', 'src/args.cc', 'src/line_io.cc'],
)

# Everything that links the Nix evaluator.
nix_lib = static_library(
  'nix-ci-eval-nix',
  ['src/nix_eval.cc', 'src/worker.cc', 'src/coordinator.cc'],
  dependencies : nix_deps,
)

executable(
  'nix-ci-eval',
  'src/main.cc',
  link_with : [core_lib, nix_lib],
  dependencies : nix_deps,
  install : true,
)

pure_test = executable(
  'pure-test',
  'test/pure_test.cc',
  link_with : core_lib,
  include_directories : include_directories('src'),
)
test('pure', pure_test)
