# Shadow Skeleton

## Purpose and Boundaries

- Shadow is a local-first, non-destructive photographic workflow for importing, reviewing, editing,
  developing, and exporting photographs across macOS and Windows.
- It preserves the semantic continuity of a logical photo and its immutable Recipe across local,
  remote-Library, and future platform-specific execution routes.
- It does not make the root repository a model store, camera-provider distribution channel, or
  persistent implementation knowledge base.

## Source Authority

- [`README.md`](README.md) is the product-facing entry and shortest working path.
- [`docs/README.md`](docs/README.md) routes developer, contract, and operations work.
- [`docs/architecture/README.md`](docs/architecture/README.md) routes a change to a maintained
  subsystem; its local README, entry module, facade, schema, build registration, and adjacent tests
  define current mechanics.
- [`AGENTS.md`](AGENTS.md) owns Shadow-specific workspace, test-topology, localization, payload,
  and canonical-debug constraints.
- [`REVIEW_SKELETON.md`](REVIEW_SKELETON.md) provides durable review priorities and red lines.

## Repository Map

| Concern | Stable entry | Ownership boundary |
| --- | --- | --- |
| Find the owning subsystem | [`docs/architecture/README.md`](docs/architecture/README.md) | Repository-level routing to a narrow source owner |
| Identity, Recipes, Catalog, cache, and workflows | [`crates/`](crates/) through the repository map | Portable domain and persistence contracts |
| RAW decode, development, and native image execution | [`cpp/shadow-image/README.md`](cpp/shadow-image/README.md) and [`crates/shadow-bridge/README.md`](crates/shadow-bridge/README.md) | Provider-neutral image contract across C++ and Rust |
| Desktop presentation and long-lived application services | [`apps/desktop/README.md`](apps/desktop/README.md) and [`crates/shadow-desktop-bridge/README.md`](crates/shadow-desktop-bridge/README.md) | Qt interaction versus durable service orchestration |
| Interactive adjustment, preview, and render performance | [`docs/development/interactive-editing-pipeline.md`](docs/development/interactive-editing-pipeline.md) | Minimal invalidation path, GPU continuity, cancellation, and preview/detail/export equivalence |
| Local AI evidence and model-admission contracts | [`crates/shadow-ai/README.md`](crates/shadow-ai/README.md) | Model-independent evidence and explicit human authority |
| Cross-repository validation and runnable debug build | [`docs/development/README.md`](docs/development/README.md) | Reproducible developer and release evidence |

## Architectural Priors

- Source is the detailed architecture index. A skeleton routes to stable owners; it does not mirror
  current implementation, API shape, feature status, or call paths.
- A logical photo can have several representations and locations. Filesystem path, cache path, and
  server address are not logical identity.
- Preview, detail, and export can have different execution quality, but must compile from the same
  immutable Recipe meaning.
- Private camera providers remain outside the public distribution and desktop process. Shared
  algorithms and product contracts stay provider-neutral.
- Local AI and accelerated execution are admitted through explicit provenance and product policy;
  they do not gain authority to modify originals or silently turn model evidence into user facts.
- Large cohesive owners are acceptable. Revisit an owner when lifecycle, policy, mutation authority,
  or failure handling becomes independently changeable.

## Project Invariants

- Original photo bytes remain read-only. User-authored state is migrated or explicitly preserved,
  never silently discarded.
- Network origin, hardware provider, fallback, cache residency, and model backend are provenance,
  not permission to change authored image semantics.
- Build output, model artifacts, caches, generated previews, and photo payloads remain outside the
  source worktree.
- User-visible desktop text remains fully localized in Simplified Chinese as well as canonical
  English source identity.
- Product behavior must be proven through the real relevant boundary: source, schema, build graph,
  linked runtime, or packaged UI as appropriate.

## Navigation Scope

Start here for durable project orientation, then use the repository map and the selected owner's
local entry in one or two hops. Add a nested `SKELETON.md` only if a large subsystem cannot provide
that routing through its ordinary README and source entry. Update this file only when a stable
boundary, owner route, source authority, or project invariant changes.

## Contract

Orientation only. Verify current facts in source, tests, schemas, build configuration, and real
artifacts before acting.
