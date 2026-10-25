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

Debug builds and the canonical debug launcher use their own remembered development library, with
the independent Debug Library as the initial choice. Existing
daily data stays in its original directory and is opened explicitly with `--profile daily`:

```sh
cargo xtask desktop-run-debug --profile daily
cargo xtask desktop-run-debug --profile-root /absolute/path/to/test-library
```

The [desktop profile owner](../../apps/desktop/README.md#storage-profiles) defines settings migration,
locking, and the Catalog/attachment/cache/job isolation boundary. No profile selection silently
falls back to daily data. Keep validation on an explicit temporary root.

The title-bar library button and Settings → Libraries provide creation, opening and recent-library
selection without launch arguments. `SHADOW_PROFILE_REGISTRY_ROOT` isolates bootstrap records for
tests. The opt-in `desktop_smoke/storage_profiles.*` harness accepts one named
`SHADOW_DESKTOP_EDIT_SOURCE_PATH` and `SHADOW_DESKTOP_PROFILE_SWITCH_TARGET` to verify durable working
edits across a real restart; `SHADOW_DESKTOP_PROFILE_VERIFY_EXPOSURE` verifies them after reopening.
These smoke routes reject Daily and the restart removes inherited edit/import/smoke commands.

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

The Mac-first source tree exposes `windows-native-dev`, `windows-server-dev`, and
`windows-desktop-dev` CMake presets.
They select the vcpkg toolchain, keep installed packages outside the repository, disable Metal,
and supply the compatibility prefix used by vcpkg Iconv layouts that install beneath
`usr/local`. The checked-in `vcpkg.json` owns the Windows native and Qt dependency set.
Windows development presets use `RelWithDebInfo`: Rust static archives use the release dynamic
MSVC runtime even in Cargo's development profile, so this keeps the C++, Qt, and Rust boundary on
one runtime while retaining debug symbols.

```powershell
cmake --preset windows-server-dev
cmake --build --preset windows-server-dev --target shadow-server-manager
ctest --preset windows-server-dev -R shadow-server-manager-qml-startup
cmake --install ..\.shadow-local-build\windows-server-dev --prefix ..\.shadow-local-dist\windows-server-dev
```

`windows-server-dev` builds the independent Shadow Server manager without requiring the photo
application's Qt WebView dependency. It is the Windows bring-up target for local-network Library
sharing; `windows-desktop-dev` remains the full photo-application graph. The install command creates
a self-contained Server Manager directory under `.shadow-local-dist/windows-server-dev`, including
the Qt/QML runtime, non-Qt DLL dependencies, and the isolated decode helper.

The Windows workflow runs the native-path fixtures, configures the selected Server or desktop
graph, compiles the Rust/CXX bridge, and builds the portable image kernel. Runtime launch and
immutable debug promotion remain platform backends behind the shared xtask commands. Windows
`desktop-build-promote` builds both applications, checks localization and startup, deploys their
dependency closure, and promotes an immutable copy. Set `SHADOW_CANONICAL_DEBUG_BUILD_DIR` to
the task-private external build directory, `SHADOW_GEONAMES_CITY_INDEX_PATH` to the prepared
offline index, and optionally `VCPKG_INSTALLED_DIR` to a preinstalled dependency tree.
`SHADOW_CUDA_TOOLKIT_ROOT` enables the CMake CUDA backend using that toolkit's `nvcc.exe`.
Set `SHADOW_LENSFUN_DATABASE_PATH` to an offline Lensfun XML database compatible with the
linked library (version 1 for Lensfun 0.3.4); Windows deploys it beside both applications.
`cmake/ShadowWindowsDependencies.cmake` decodes localized MSVC `/utf-8` include diagnostics
explicitly so console and headless Ninja builds retain header-dependency tracking.
The CUDA executor owns resident pixel-local adjustments and display output; RAW development,
neighborhood/mask/geometry fallback and RGB16 export preserve the existing CPU semantics.

Windows promotion uses the same canonical-debug steward lock. It verifies every copied file,
retains immutable revisions under `releases/windows-debug`, and atomically updates
`current-debug-windows.json`, including the previous revision for rollback. The shared
`cargo xtask desktop-run-debug` command resolves this pointer; `--server` launches Shadow Server.
PowerShell users can use `scripts/run_debug.ps1` with the same arguments. A prepared deployment
can also be promoted with `cargo xtask desktop-promote-debug <absolute-directory> <validation-label>`.
The Windows debug launcher selects the development Library lane in `RelWithDebInfo` as well;
the standalone Server defaults to its Debug Library. Explicit `--profile`, `--profile-root`,
and process-specific data-root overrides retain priority. `--check` only verifies launch inputs.
Private Nikon binaries and their local decoder link remain separate from this public deployment.

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
