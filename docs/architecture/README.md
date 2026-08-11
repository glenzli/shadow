# Architecture and repository map

Shadow uses the source tree as its detailed architecture index. Start with the narrowest subsystem
below, then follow its local README, module declarations, native facade, or QML composition root.

| Area | Stable entry | Responsibility |
| --- | --- | --- |
| Domain contracts | [`shadow-domain`](../../crates/shadow-domain/src/lib.rs) | Pure identities, edit graphs, Recipes, review state, and persisted contract types |
| Native filesystem paths | [`shadow-native-path`](../../crates/shadow-native-path/README.md) | Lossless `AssetLocation` encoding, platform checks, and host-path reconstruction |
| Catalog | [`shadow-catalog`](../../crates/shadow-catalog/src/lib.rs) | SQLite ownership, repositories, immutable ledgers, and projections |
| Cache | [`shadow-cache`](../../crates/shadow-cache/README.md) | Content-addressed storage, verification, quarantine, and cache records |
| Core workflows | [`shadow-core`](../../crates/shadow-core/src/lib.rs) | Scanning, source inspection, cache orchestration, and bounded workers |
| Library sharing | [`shadow-library-sharing`](../../crates/shadow-library-sharing/README.md) | Authenticated remote manifests and verified on-demand original materialization |
| AI evidence | [`shadow-ai`](../../crates/shadow-ai/README.md) | Technical observations, explicit feedback, admission, and model-independent scoring |
| Rust image boundary | [`shadow-bridge`](../../crates/shadow-bridge/README.md) | Safe Rust API over the C++ decoder and render kernel |
| Desktop services | [`shadow-desktop-bridge`](../../crates/shadow-desktop-bridge/README.md) | Long-lived Library, Review, Precision, export, and CXX-facing services |
| Native image kernel | [`shadow-image`](../../cpp/shadow-image/README.md) | Decoder providers, RAW development, adjustment execution, and native image buffers |
| Qt application | [`apps/desktop`](../../apps/desktop/README.md) | QML presentation, Qt controllers, image providers, and desktop lifecycle |
| CLI | [`shadow-cli`](../../apps/shadow-cli/README.md) | Focused catalog, decode, backup, AI, and remote-Library operator commands |
| Repository validation | [`xtask`](../../xtask/src/main.rs) | Repository-wide format, structure, test, localization, and build gates |

## Stable boundaries

- A logical photo may own several representations and locations. A path, cache location, or server
  address is not photo identity.
- Original photos remain read-only. User-authored state is migrated or explicitly preserved rather
  than silently discarded.
- Remote originals are materialized and verified locally before edit admission; network origin does
  not change Recipe meaning.
- Preview, detail, and export may use different execution quality while compiling the same immutable
  Recipe semantics.
- Private camera providers remain outside the public distribution and desktop process. Shared
  algorithms and contracts remain provider-neutral.
- Execution provider and fallback are provenance. They may not silently change authored image
  semantics.

These are navigation and boundary constraints, not proof of current implementation. Verify a
change against the selected owner, its schemas, build graph, and tests. When a responsibility moves,
update the nearest code-owned index in the same change.
