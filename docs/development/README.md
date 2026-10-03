# Development guide

This guide owns repository-wide developer workflows. Component-specific build options and focused
tests remain documented beside their source owner.

## Interactive editing pipeline

Before changing a live adjustment, RAW rebinding, GPU/CPU render path, warm session, or preview
scheduler, read the [interactive editing pipeline contract](interactive-editing-pipeline.md). It
defines the required minimal invalidation path, source/receipt continuity, host/device-transfer
budget, and interactive-versus-settled behavior. The project-local
`$shadow-interactive-rendering` skill turns that contract into the required change workflow.

## External single-photo edit tools

The [stdio edit-tool contract](agent-edit-stdio.md) documents the explicit one-photo launch, typed
exposure proposal, shared owner/CAS arbitration, immutable export, and temporary-session limits.

## Multi-photo composition

The [desktop composition owner](../../apps/desktop/README.md#hdr-and-panorama-composition)
documents source admission, quality limits, cancellable worker execution and non-destructive
publication. The native decoder index owns the linear TIFF round-trip contract.

## Canonical debug application

Build and atomically promote a complete debug application with:

```sh
cargo xtask desktop-build-promote
```

Launch the promoted application with:

```sh
cargo xtask desktop-run-debug
```

The default promotion path is intentionally fast: it keeps the workspace guard, localization gate,
incremental package build, bundle admission, copy verification, and launch-input check, but skips
the startup CTest. Use `cargo xtask desktop-build-promote --verify` when a change needs the
desktop and server-manager startup smoke as well. CMake reuses the configured canonical build
directory while still refreshing its build graph for the current source tree.

The matching `.sh` and `.ps1` files in `scripts/` are thin wrappers for terminals that prefer
platform-native launchers. Use `cargo xtask desktop-build-promote --check` to inspect resolved
build and asset inputs
without building. A first machine setup supplies `SHADOW_GEONAMES_CITY_INDEX_PATH`; later builds can
reuse the dedicated local asset cache or the previous canonical bundle as a read-only bootstrap source.

Promoted builds live outside the repository under the sibling `.shadow-local-build` directory.
Promotion creates an immutable revision-stamped release and atomically advances `current-debug`, so
a running application is never modified in place. `cargo xtask desktop-run-debug --check` reports the
resolved executable and managed model paths without launching the application.

The launcher detaches by default and appends output to
`.shadow-local-build/logs/shadow-debug.log`. Pass `--foreground` for process-attached debugging.

## Local build storage

Shadow keeps build output outside the repository so concurrent work cannot pollute source, but those
external artifacts still need deliberate retention. Inspect the canonical CMake directory, default
Cargo target root, and immutable debug releases with:

```sh
cargo xtask storage-report
```

Debug releases are immutable rollback points. The prune command is dry-run by default, always keeps
`current-debug`, and keeps three releases unless requested otherwise. Close any old debug app before
deleting its bundle:

```sh
cargo xtask storage-prune-debug --keep 3
cargo xtask storage-prune-debug --keep 3 --apply
```

This command intentionally does not delete canonical or task-private Cargo/CMake directories:
those need an owner-aware task lease rather than an age-based guess.

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
