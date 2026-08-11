# Development guide

This guide owns repository-wide developer workflows. Component-specific build options and focused
tests remain documented beside their source owner.

## Canonical debug application

Build and atomically promote a complete debug application with:

```sh
cargo xtask desktop-build-promote
```

Launch the promoted application with:

```sh
cargo xtask desktop-run-debug
```

The matching `.sh` and `.ps1` files in `scripts/` are thin wrappers for terminals that prefer
platform-native launchers. Use `cargo xtask desktop-build-promote --check` to inspect resolved
build and asset inputs
without building. A first machine setup supplies `SHADOW_GEONAMES_CITY_INDEX_PATH` and
`SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR`; later builds can reuse the dedicated local asset cache or
the previous canonical bundle as a read-only bootstrap source.

Promoted builds live outside the repository under the sibling `.shadow-local-build` directory.
Promotion creates an immutable revision-stamped release and atomically advances `current-debug`, so
a running application is never modified in place. `cargo xtask desktop-run-debug --check` reports the
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
verification, or `--all` only when intentionally normalizing all tracked native sources. Native
formatting uses `clang-format` from `PATH`; set `SHADOW_CLANG_FORMAT` to an explicit executable
when the toolchain is installed elsewhere.

## Windows compile contract

The Mac-first source tree exposes `windows-native-dev` and `windows-desktop-dev` CMake presets.
They select the vcpkg toolchain, keep installed packages outside the repository, disable Metal,
and supply the compatibility prefix used by vcpkg Iconv layouts that install beneath
`usr/local`. The checked-in `vcpkg.json` owns the Windows native and Qt dependency set.

```powershell
cmake --preset windows-desktop-dev
cmake --build --preset windows-desktop-dev --target shadow-desktop-rust-build
```

The Windows workflow runs the native-path fixtures, configures that desktop graph, compiles the
Rust/CXX bridge, and builds the portable image kernel. Runtime launch and immutable debug promotion
remain platform backends behind the shared xtask commands; the macOS backends are implemented here,
and Windows can add its backend without changing the command contract.

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
