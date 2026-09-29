# Shadow Agent Instructions

## Orientation and source facts

Before planning or editing Shadow, read `SKELETON.md`. Read `REVIEW_SKELETON.md` for review or a
substantive change. These files are durable orientation, never proof of current implementation.
Verify facts through the selected owner's source, schema, tests, build registration, and runnable
artifact.

## Shared workspace boundaries

When multiple agents or tasks may touch this checkout, use the current
`$coordinate-shared-workspace` skill before writing. Its Dev Mesh record under the ignored
`.dev-mesh/` directory owns run, claim, inherited-baseline, work-result, handoff, and commit
procedure; do not maintain a competing local protocol in this repository.

- Claim the narrow semantic scope and exact likely paths. Existing dirty paths belong to another
  owner until an explicit inherited-baseline acceptance or handoff authorizes them.
- Never clean, revert, stage, reformat, commit, or publish another owner's work. Use the skill's
  managed publication path for a commit; do not use a workspace-wide stage as a shortcut.
- Keep collaboration payload-free: pass paths, stable identities, dimensions, digests, and bounded
  diagnostics. Do not transfer a worktree snapshot, catalog, RAW/DNG, rendered image, model,
  cache, or fixture corpus through coordination.
- Treat source images as bounded local evidence. Inspect one named, display-sized preview only when
  needed; never attach, encode, or bulk-read photo payloads for discussion.
- Use task-private external Cargo and CMake build directories. Before write-heavy validation, run
  `sh scripts/local_shared_workspace_guard.sh`; a failure is a transport blocker, not contention.
- Use the least ceremony that preserves ownership and recovery. The hard guards are ownership,
  non-destructive state handling, and serialization of shared Git or canonical-debug publication.

## Repository navigation and cohesion

The source tree is Shadow's detailed architecture index. Start with `SKELETON.md`, then
[`docs/README.md`](docs/README.md), the [repository map](docs/architecture/README.md), and the
nearest README or source entry for the selected owner. The root README remains product-facing.

- Keep `lib.rs`, `main.rs`, CXX/Qt facades, and top-level QML controllers readable as composition
  and navigation boundaries. Put state machines, persistence policy, protocols, and feature
  behavior in responsibility-named owners.
- Update the nearest code-owned index when a stable responsibility is added, extracted, renamed, or
  removed. Do not duplicate current mechanics across Markdown files.
- Apply `$maintain-source-cohesion` when a substantial change pressures an ownership boundary,
  especially in bridge facades, desktop controllers, large QML surfaces, services, persistence, or
  algorithms. Use its judgment; explain a keep/extract/defer decision only when it is non-obvious or
  needed for a handoff.
- Keep private-invariant tests adjacent to their semantic owner and public cross-owner behavior at
  the real integration boundary. A legacy hotspot is a review signal, not permission for unrelated
  cleanup.

## Interactive editing pipeline contract

Before changing an interactive adjustment, RAW-development/rebinding route, node executor, warm
preview session, GPU/CPU fallback, presentation surface, or preview scheduler, read
[`docs/development/interactive-editing-pipeline.md`](docs/development/interactive-editing-pipeline.md)
and use the project-local `$shadow-interactive-rendering` skill. Treat a responsive edit as a
minimal invalidation-path contract, not merely as a GPU implementation detail: the change must name
its reusable upstream state, its exact recomputation frontier, every host/device transfer, cache
identity impact, cancellation rule, and preview/detail/export equivalence boundary.

## Canonical test topology

Shadow does not keep executable Rust test bodies inline in production source. API doctests and
compile-time assertions may remain with the contract they document; ordinary test functions may not.

- A production Rust owner may end with `#[cfg(test)] mod tests;`, but implementations begin in
  `<owner>/tests.rs`, never in production source or a sibling `<owner>_tests.rs`. Do not redirect
  tests with `#[path]` into a distant directory.
- When one owner needs several test responsibilities, use `<owner>/tests/mod.rs` and
  responsibility-named children. Avoid generic or numbered test names.
- `src/tests/` is reserved for private facade-owned contracts spanning sibling modules. Its children
  use `<responsibility>_contract.rs` and import their named production contracts and fixtures
  directly. Package-level `tests/` contains black-box contracts against the public API.
- Fixtures live with the narrowest owner and move with that owner. Promote them only after genuine
  reuse. Do not duplicate production logic or expose internals merely for test convenience.
- Native/QML tests follow the same ownership levels. Every runnable source maps to one build target
  and one test registration; moving it also moves definitions, dependencies, environment, labels,
  timeouts, fixtures, and generated registry expectations.
- A passing old executable is not source-fresh evidence. Build changed native sources before running
  their registrations. A QML source-path load is focused component evidence, not packaged-module or
  real-window acceptance.

`cargo xtask test-layout` is a zero-debt structural gate and runs first in `cargo xtask check`.
An inline executable test, distant private-source inclusion, disabled test, or generic test owner is
a structural regression rather than baseline debt.

## Desktop localization contract

English `tr()`/`qsTr()` text is the canonical desktop message identity. Simplified Chinese must be
a finished product presentation, not a best-effort fallback:

- Route ordinary user-visible labels, states, errors, actions, and accessibility text through Qt
  translation. Technical tokens such as `RGB`, LUT names, camera formats, numeric patterns, and
  schema identities may remain language-neutral.
- Every production message has exactly one finished, non-empty Simplified Chinese translation. Do
  not commit unfinished, stale, duplicate, or placeholder-mismatched catalog entries.
- Run `cargo xtask desktop-i18n-check` whenever production QML/C++ text or the Chinese catalog
  changes. Canonical desktop build and release use the same extraction, message-set, placeholder,
  and QM-compilation gate.

## Build outputs and canonical debug build

Source-root `target/` and `build/` are quarantined legacy payloads. Use a task-private external
`CARGO_TARGET_DIR` and `SHADOW_BUILD_DIR`; concurrent tasks never reconfigure or link another
task's directory for convenience.

The stable debug entry is `../.shadow-local-build/current-debug/Shadow.app`, launched with
`scripts/run_debug.sh`. Only a temporary canonical-debug steward, coordinated through the shared
workspace skill and the semantic resource `release:canonical-debug`, may advance it.

- Validate complete current-source application input before promotion: workspace guard, canonical
  desktop build, localization, startup/edit smoke coverage, and any relevant installed private
  provider smoke.
- Promote with `scripts/promote_debug_build.sh /absolute/path/to/Shadow.app <validation-label>`.
  It copies into an immutable revision and atomically advances `current-debug`; never mutate an app
  the user may be running.
- Preserve the actual category of promotion failure. Retain the current promoted build and at least
  one previous revision for rollback; prune only through the same canonical-debug authority. If
  authorization or environment blocks promotion, record the candidate identity, validation, and
  resume condition rather than calling it contention or claiming a release.
