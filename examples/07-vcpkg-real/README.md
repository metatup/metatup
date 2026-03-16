# 07 - Real vcpkg Project

This example uses a real `vcpkg` executable and a real package manifest. It is
kept in `examples/`, but it is intentionally not part of the automated test
suite because it depends on external tooling and may need network access on the
first install.

Prerequisites:

- `vcpkg` installed and reachable either through `VCPKG_ROOT` or `PATH`
- a working C toolchain
- on Windows, an MSVC developer shell

Run:

```sh
../../metatup
```

What it shows:

- The standard `@std//native` package defaulting to the host toolchain automatically
- The standard `@std//vcpkg` helper returning spreadable install metadata and package flags
- A pinned `builtin-baseline` in `manifest/vcpkg.json`
- Default export of installed `include/` and `lib/` prefix directories
- Building and linking a native binary against the installed `sqlite3` package

Outputs:

- `build/vcpkg_installed/<triplet>/include/sqlite3.h`
- `build/vcpkg_installed/<triplet>/lib/...sqlite3...`
- `build/sqlite-demo` or `build/sqlite-demo.exe`

Maintenance notes:

- Keep `manifest/vcpkg.json` pinned. Refresh the baseline when you intentionally
  update the dependency set.
- If you need more installed content tracked, extend `vcpkg/export_dirs` or use
  explicit `vcpkg/export_files`.
- The example relies on stdlib defaults for host toolchain selection, manifest
  input discovery, and `include/` + `lib/` exports.
