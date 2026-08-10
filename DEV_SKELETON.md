# Shadow Development Skeleton

## Purpose

- Orient a new development task before it enters Shadow's source tree.
- Preserve the few product and engineering constraints that should shape work across subsystems.

## Non-Goals

- This is not an architecture inventory, implementation summary, API catalog, or project status log.
- It does not replace the root README, subsystem READMEs, module declarations, tests, or source.

## Source Of Truth

- [`README.md`](README.md) is the product-facing entry; [`docs/README.md`](docs/README.md) routes developer work.
- [`docs/architecture/README.md`](docs/architecture/README.md) routes a change to the maintained subsystem index.
- The nearest crate/application README and its `lib.rs`, `main.rs`, native facade, or QML composition root own detailed navigation.
- Source and adjacent contract tests define behavior; schemas and wire declarations define compatibility.
- `AGENTS.md` defines workspace, test-topology, localization, coordination, and build constraints.

## Documentation Boundary

- Keep the root README focused on product identity, the shortest working entry points, documentation links, and license summary.
- Use `docs/README.md` as the developer documentation index and separate cross-cutting architecture, development, contract, or operations documents only when they have an independent audience and lifecycle.
- Keep component READMEs focused on ownership, boundary intent, and the next source navigation step.
- Keep current mechanics, feature status, call graphs, and exhaustive inventories in source, schemas, build registration, and tests rather than mirroring them into Markdown.

## Stable Constraints

- Shadow is local-first and non-destructive: original photos remain read-only and user-authored state is never silently discarded.
- Persistent identities and Recipes must remain portable across local, remote, and future platform-specific execution routes.
- Private camera providers stay outside the public distribution and outside the desktop process; shared algorithms remain provider-neutral.
- The source tree is the architecture index. Add a responsibility-named owner and update its nearest index instead of growing a separate knowledge base.
- Build output, caches, models, and photo payloads remain outside the source worktree.

## Domain Assumptions

- One logical photo may have multiple representations and source locations; path or server address is not photo identity.
- Remote originals are materialized and verified locally before editing. Network origin must not change Recipe semantics or rendering intent.
- Preview, detail, and export may use different execution quality, but they must compile from the same immutable Recipe meaning.
- GPU/CPU/provider differences are execution facts and provenance, not excuses for silent visual-contract changes.

## Entry Hints

- Start at [`docs/README.md`](docs/README.md), use the [repository map](docs/architecture/README.md), then follow only the selected subsystem's local index.
- For Qt presentation and interaction, start at [`apps/desktop/README.md`](apps/desktop/README.md).
- For desktop service orchestration, start at [`crates/shadow-desktop-bridge/README.md`](crates/shadow-desktop-bridge/README.md).
- For decoder and render behavior, start at [`cpp/shadow-image/README.md`](cpp/shadow-image/README.md) and [`crates/shadow-bridge/README.md`](crates/shadow-bridge/README.md).
- For persistent photo/catalog contracts, start at the `shadow-domain` and `shadow-catalog` entries in the repository map.

## Refresh Triggers

Update this file only when Shadow's product purpose, source-of-truth policy, cross-platform/provider boundary, identity model, or repository navigation contract changes. Routine feature work belongs in source and the nearest code-owned index.

## Boundary

Orientation only. Verify every implementation fact against current source, tests, schemas, and build configuration.
