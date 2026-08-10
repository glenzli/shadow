# Development guide

This guide owns repository-wide developer workflows. Component-specific build options and focused
tests remain documented beside their source owner.

## Canonical debug application

Build and atomically promote a complete debug application with:

```sh
./scripts/build_and_promote_debug.sh
```

Launch the promoted application with:

```sh
./scripts/run_debug.sh
```

Use `./scripts/build_and_promote_debug.sh --check` to inspect resolved build and asset inputs
without building. A first machine setup supplies `SHADOW_GEONAMES_CITY_INDEX_PATH` and
`SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR`; later builds can reuse the dedicated local asset cache or
the previous canonical bundle as a read-only bootstrap source.

Promoted builds live outside the repository under the sibling `.shadow-local-build` directory.
Promotion creates an immutable revision-stamped release and atomically advances `current-debug`, so
a running application is never modified in place. `./scripts/run_debug.sh --check` reports the
resolved executable and managed model paths without launching the application.

The launcher detaches by default and appends output to
`.shadow-local-build/logs/shadow-debug.log`. Pass `--foreground` for process-attached debugging.

## Repository checks

Run the broad repository gates with:

```sh
cargo xtask check
cargo xtask test
```

Useful focused entry points include:

```sh
cargo xtask format
cargo xtask test-layout
cargo xtask doctor
cargo xtask native-check
cargo xtask desktop-i18n-check
cargo xtask desktop-build
```

`cargo xtask format` delegates Rust to `rustfmt.toml` and tracked native sources to
`.clang-format`. Pass explicit native files for a narrow edit, `--check` for non-mutating
verification, or `--all` only when intentionally normalizing all tracked native sources.

Repository instructions require build output outside the source tree. Agents and concurrent tasks
must use task-private external Cargo and CMake directories; see [AGENTS.md](../../AGENTS.md) for the
shared-workspace and canonical-debug publication contracts.

## Local RAW validation

Run the local RAW matrix with:

```sh
cargo xtask raw-smoke ./local-reference/sample-assets/raw
```

With no directory argument, `raw-smoke` uses the ignored
`local-reference/sample-assets/raw/` directory. Proprietary samples, generated previews, models,
and benchmark output must not enter Git.

The matrix exercises metadata, embedded-preview extraction, bounded reference rendering, and
optical-profile discovery. Provider-specific expectations belong in the relevant image or provider
README rather than this repository-wide guide.

## Operator commands

The command-line application provides focused catalog, scan, backup, decode, AI, and remote-Library
diagnostics. See the [`shadow-cli` command guide](../../apps/shadow-cli/README.md) for the maintained
command list.

The standalone Library Server has its own [operations guide](../operations/library-server.md).
