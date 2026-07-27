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
| Desktop-session Review and cache-maintenance CXX delegation | [`src/session_review.rs`](src/session_review.rs), [`src/session_cache_maintenance.rs`](src/session_cache_maintenance.rs) |
| Canonical wall-clock conversion and digest encoding | [`src/wall_clock.rs`](src/wall_clock.rs), [`src/digest_hex.rs`](src/digest_hex.rs) |
| Folder import lifecycle | [`src/scan_service.rs`](src/scan_service.rs) |
| Source relinking | [`src/relink_service.rs`](src/relink_service.rs) |
| Review presentation, decisions, and comparison evidence | [`src/review_service.rs`](src/review_service.rs) |
| Photo source admission, quarantine, optics discovery, and raster delivery | [`src/session_photo_source.rs`](src/session_photo_source.rs), [`src/photo_provider.rs`](src/photo_provider.rs), [`src/isolated_proxy.rs`](src/isolated_proxy.rs) |
| Preview identity, cancellation, and session-local reuse | [`src/preview_cache_identity.rs`](src/preview_cache_identity.rs), [`src/preview_render_registry.rs`](src/preview_render_registry.rs), [`src/session_preview_store.rs`](src/session_preview_store.rs) |
| 1:1 detail tile geometry and reuse | [`src/detail_viewport.rs`](src/detail_viewport.rs), [`src/detail_tile_cache.rs`](src/detail_tile_cache.rs) |
| Desktop-session interactive preview/detail render lifecycle | [`src/session_edit_render.rs`](src/session_edit_render.rs) |
| Working drafts, named versions, checkout, and edit-reference publication | [`src/session_edit_history.rs`](src/session_edit_history.rs) |
| Recipe v1 draft model and FFI translation, validation, stable identity, snapshot codec, render-plan compilation, and version summaries | [`src/recipe_v1.rs`](src/recipe_v1.rs), [`src/recipe_v1/draft.rs`](src/recipe_v1/draft.rs), [`src/recipe_v1/ffi_adapter.rs`](src/recipe_v1/ffi_adapter.rs), [`src/recipe_v1/validation.rs`](src/recipe_v1/validation.rs), [`src/recipe_v1/identity.rs`](src/recipe_v1/identity.rs), [`src/recipe_v1/snapshot_encode.rs`](src/recipe_v1/snapshot_encode.rs), [`src/recipe_v1/snapshot_decode.rs`](src/recipe_v1/snapshot_decode.rs), [`src/recipe_v1/compiler.rs`](src/recipe_v1/compiler.rs), [`src/edit_version_diff.rs`](src/edit_version_diff.rs) |
| Shared Grade Node library, application, and desktop-session orchestration | [`src/shared_grade_library.rs`](src/shared_grade_library.rs), [`src/shared_grade_application.rs`](src/shared_grade_application.rs), [`src/session_shared_grade.rs`](src/session_shared_grade.rs) |
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
