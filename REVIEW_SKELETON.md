# Shadow Review Skeleton

## Review Priorities

- Correct non-destructive photo and Recipe semantics before implementation convenience.
- Preserve identity, persistence, wire, cache, rendering, and provider boundaries across all execution paths.
- Keep interaction behavior observable through real QML input and packaged resources, not controller-only tests.
- Keep each change navigable through one semantic owner and its nearest source-owned index.
- Treat `SKELETON.md` as durable orientation and source, schemas, build graphs, and artifacts as facts.

## Block

- Any path that modifies an original photo or silently drops user-authored state.
- Path-, machine-, server-address-, or cache-location-dependent logical photo identity.
- Loading private decoder/provider code into the desktop process or public distribution.
- Silent fallback that changes authored RAW, Recipe, color, geometry, mask, or export semantics.
- New binary payloads, models, caches, build output, or photo fixtures inside the source worktree.
- Mixed-language production UI or executable tests hidden inside production source.

## Risk Patterns

- Preview succeeds through an RGB compatibility route while detail/export require sensor-domain data.
- A direct method test passes while real pointer, focus, resource, or runtime-language behavior is broken.
- Cache keys omit source environment, requested plan, Recipe identity, provider contract, or representation fingerprint.
- One facade accumulates persistence, orchestration, rendering, and presentation policy that should have separate lifecycles.
- Rust/C++/Qt declarations compile in one build graph while another manifest or packaged module omits the same contract.
- A project skeleton or component README accumulates feature status, current call chains, operator
  procedures, or exhaustive implementation inventories instead of routing to a stable owner.

## Verification Expectations

- Build the exact changed native/QML targets before running their registered tests.
- Exercise representative real input for gestures and packaged-module reachability for new QML/resource families.
- Run the desktop localization gate whenever user-visible text changes.
- Validate preview, detail, and export together when source development or Recipe compilation changes.
- Use task-private external build directories, then promote only a complete current-source build through the canonical debug process.

## Review Method

1. Read this file and `SKELETON.md` for durable intent.
2. Use `docs/README.md`, the repository map, and the nearest code-owned index to find the semantic owner.
3. Inspect the actual diff, source, tests, schemas, and build registrations.
4. Lead with concrete findings and exact file references.
5. Treat these skeletons as preferences and constraints, never as factual proof of current implementation.
