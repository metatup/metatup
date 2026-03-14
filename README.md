# MetaTup

MetaTup is an expansion of Tup, mostly done with OpenAI Codex, that, while
straying away from original philosophy, adapts the mental model for bigger,
more complex projects, while also improving structural clarity.

Think of MetaTup's relation to Tup as to how C++ is related to C.

## Introduced capabilities

MetaTup extends Tup in a few major areas:

* Function-oriented Tupfiles.
  Functions can contain rules, accept bound arguments, return values, and be invoked either in the current Tupfile context or via `spawn` in another Tupfile.
* Expression helpers for function composition.
  `$(globs ...)` resolves globs in-place, `$(groups ...)` expands built groups for returns and downstream use, and `$(abs ...)` returns the absolute path to a single file.
* YAML-driven build selection with `MetaTup.yaml` and `TupBuild.yaml`.
  Components can describe concrete builds or aggregate dependencies, and `tup gen` materializes those definitions into `TupBuild.yaml`.
* Higher-level dependency wiring.
  Components support argument `binds`, profile-based argument sets, conditional dependencies via `require_if`, and cross-package references such as `//pkg:component`.
* Dist materialization.
  Functions can assemble redistributable prefixes and expose them as named returns, which `TupBuild.yaml` can materialize through `dists` entries.
* Optional automatic compilation database refresh.
  `auto_compiledb` allows TupBuild-driven builds to regenerate `compile_commands.json` after successful updates.

A typical MetaTup flow looks like this:

1. Define reusable components in `MetaTup.yaml`.
2. Create a build directory and run `tup gen <component>`.
3. Run `tup` to execute the generated `TupBuild.yaml`.

For the command-level details, see `metatup.1`.

---

# Tup

http://gittup.org/tup

## About Tup

Tup is a file-based build system for Linux, OSX, and Windows. It takes
as input a list of file changes and a directed acyclic graph (DAG). It
then processes the DAG to execute the appropriate commands required to
update dependent files. Updates are performed with very little overhead
since tup implements powerful build algorithms to avoid doing
unnecessary work. This means you can stay focused on your project rather
than on your build system.

Further information can be found at http://gittup.org/tup
