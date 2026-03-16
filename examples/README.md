# MetaTup Examples

This directory is intentionally listed in the repository root `.tupignore`.
Each example is a standalone MetaTup project you can enter and run without
changing the main repository DAG.

Suggested order:

1. `01-get-this-done`
   Function-oriented Tupfiles, `call`, `spawn`, `$(globs ...)`,
   `$(groups ...)`, `$(abs ...)`, and `$(prefix_paths ...)`.
2. `02-components-profiles-compiledb`
   `MetaTup.yaml`, `metatup gen`, profiles, `binds`, `case`, `require_if`,
   `//pkg:component` dependencies, and `auto_compiledb`.
3. `03-repo-workspaces`
   `MetaTupRepo.yaml`, path repositories, and `@repo//pkg` calls/spawns.
4. `04-dists`
   Returned dist prefixes and `metatup gen -D`.
5. `05-external-project`
   The built-in `@std//external` helper for configure/build/install flows.
6. `06-vcpkg-manifest`
   The built-in `@std//vcpkg` helper in a hermetic, test-friendly setup with
   default manifest input discovery.
7. `07-vcpkg-real`
   A real `vcpkg` manifest install that builds and links against `sqlite3`
   using stdlib defaults for host toolchain and manifest handling.

Common commands:

- Direct Tupfile examples:
  `../../metatup`
- MetaTup generation examples:
  `mkdir -p build && cd build && ../../../metatup gen <component> && ../../../metatup`

The examples favor local, hermetic inputs. The `vcpkg` example uses a tiny fake
`vcpkg` executable so it stays runnable without network access.
`07-vcpkg-real` is the maintained real-world counterpart and is not part of the
automated tests.
