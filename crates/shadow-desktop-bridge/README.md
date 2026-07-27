# shadow-desktop-bridge

`shadow-desktop-bridge` is the long-lived Rust application boundary consumed by the Qt desktop
shell. It owns coarse desktop services and the CXX-facing `DesktopSession`; it does not own QML
presentation, SQLite schema details, or native image algorithms.

## Responsibility index

| Change | Primary owner |
| --- | --- |
| Desktop CXX structs, methods, and session composition | [`src/lib.rs`](src/lib.rs) |
| Library paging and filters | [`src/library_service.rs`](src/library_service.rs) |
| Desktop-session Library, relink, scan, and durable-export CXX delegation | [`src/session_library.rs`](src/session_library.rs), [`src/session_scan.rs`](src/session_scan.rs), [`src/session_export.rs`](src/session_export.rs) |
| Folder import lifecycle | [`src/scan_service.rs`](src/scan_service.rs) |
| Source relinking | [`src/relink_service.rs`](src/relink_service.rs) |
| Review presentation, decisions, and comparison evidence | [`src/review_service.rs`](src/review_service.rs) |
| Photo source admission and raster delivery | [`src/photo_provider.rs`](src/photo_provider.rs), [`src/isolated_proxy.rs`](src/isolated_proxy.rs) |
| Preview identity, cancellation, and session-local reuse | [`src/preview_cache_identity.rs`](src/preview_cache_identity.rs), [`src/preview_render_registry.rs`](src/preview_render_registry.rs), [`src/session_preview_store.rs`](src/session_preview_store.rs) |
| 1:1 detail tile geometry and reuse | [`src/detail_viewport.rs`](src/detail_viewport.rs), [`src/detail_tile_cache.rs`](src/detail_tile_cache.rs) |
| Recipe v1 translation and version summaries | [`src/recipe_v1.rs`](src/recipe_v1.rs), [`src/edit_version_diff.rs`](src/edit_version_diff.rs) |
| Shared Grade Node library and application | [`src/shared_grade_library.rs`](src/shared_grade_library.rs), [`src/shared_grade_application.rs`](src/shared_grade_application.rs) |
| Export execution and queueing | [`src/export_service.rs`](src/export_service.rs), [`src/export_queue_service.rs`](src/export_queue_service.rs) |
| Cache ownership and explicit maintenance | [`src/cache_maintenance_service.rs`](src/cache_maintenance_service.rs) |
| Cross-responsibility facade contracts and test support | [`src/tests/mod.rs`](src/tests/mod.rs) |

Add behavior to the module that owns its lifecycle and failure policy. Change `lib.rs` only when
the CXX contract, session composition, or a narrow delegation must change. A new independent
desktop workflow should begin in a responsibility-named service and be wired through the facade
after its contract is stable.

## Test layout

Small private-invariant tests remain in their production service. The crate-local `src/tests/`
tree exercises contracts that need private access across the facade and is indexed by product
responsibility rather than by numbered test parts. Public behavior that does not need crate-private
access belongs in the package-level `tests/` directory.

Run focused validation with build output outside the shared source worktree:

```sh
sh scripts/local_shared_workspace_guard.sh
CARGO_TARGET_DIR=/absolute/task-private/path cargo test -p shadow-desktop-bridge --lib
```
