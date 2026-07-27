# shadow-bridge

`shadow-bridge` is the coarse-grained CXX boundary between Rust application state and the C++20 image kernel.

## Source index

The generated CXX wire declaration stays centralized and auditable in [`src/lib.rs`](src/lib.rs).
The safe Rust side is organized by independent contract family even when several families consume
that same private wire representation:

- [`src/decoder.rs`](src/decoder.rs) owns source-neutral inspection, embedded-preview extraction,
  and shared decoder wire mappings.
- [`src/display_luma.rs`](src/display_luma.rs) owns bounded, versioned display-proxy analysis.
- [`src/optics.rs`](src/optics.rs) owns optical settings, profile discovery, and execution
  receipts.
- [`src/raw_development.rs`](src/raw_development.rs) owns RAW plans, negotiation, and
  source-development provenance receipts.
- [`src/adjustment.rs`](src/adjustment.rs) owns the typed adjustment graph, geometry, local masks,
  parameters, and their shared fail-closed validation chain.
- [`src/preview_analysis.rs`](src/preview_analysis.rs) owns warm-preview histograms, source
  clipping masks, execution provenance, and fail-closed analysis validation.
- [`src/preview_session.rs`](src/preview_session.rs) owns reusable warm-preview state,
  cancellation, and generation-matched render outcomes.
- [`src/detail_session.rs`](src/detail_session.rs) owns the retained full-resolution source,
  bounded tile requests, and tightly packed RGB8 output validation.
- [`src/lib.rs`](src/lib.rs) re-exports the public contract and retains only shared wiring that has
  not yet gained a responsibility-named owner.

Sharing the CXX representation is not by itself a reason to share one Rust source file. Extract a
safe contract when it has its own invariants, failure policy, and consumers; keep the wire
declaration intact unless the generated ABI itself gains a separately versioned boundary.

The adjacent [`src/tests/mod.rs`](src/tests/mod.rs) routes private bridge-contract tests to RAW
development, display luma, adjustment plans, preview execution, detail sessions, and opt-in real
source modules. Test fixtures stay with the contract that consumes them. Do not add another
multi-domain test block to `lib.rs`, and do not split the generated ABI merely to satisfy a line
count.

The decoder side of the bridge follows this coarse-grained path:

```text
Rust path
→ CXX open_libraw_utf8
→ C++ DecoderProvider / DecodeSession
→ owned metadata + capability + preview snapshots / compressed visual payload
→ pure shadow-domain types
```

No LibRaw or CXX type escapes the crate's public API. `inspect_libraw` returns a serializable descriptor snapshot; `extract_best_libraw_preview` returns the kernel-selected embedded preview or `None`; `render_libraw_reference_proxy` returns a bounded display JPEG for the no-preview fallback. The edit boundary accepts a bounded, dependency-ordered `AdjustmentRenderPlan` containing exposure, contrast, selective tone, Tone Curve, processed-RGB white balance, saturation, perceptual color, and detail/effects nodes. Plans contain at most 256 uniquely identified nodes and validate versions, finite parameters, Tone Curve structure, and operation-specific bounds before execution. Graph topology, processing stages, masks, layer blending, and shared revisions are compiled before this boundary rather than interpreted here. `render_libraw_adjustment_plan` is the typed one-shot path. RGB temperature/tint is a post-demosaic CAT16 adaptation, not RAW sensor-domain white balance. C++ exceptions become `BridgeError`; Rust panics and C++ exceptions never cross the language boundary directly.

For slider interaction, `LibRawEditPreviewSession::open(path, max_edge)` asks LibRaw for
processed linear-light 16-bit RGB in sRGB/Rec.709-D65 primaries once, normalizes/downsamples it,
and retains only a bounded linear float working proxy. Repeated
`render(edits, jpeg_quality)` calls provide the four-node Basic compatibility path;
`render_plan(plan, jpeg_quality)` executes a validated typed plan. The parallel
`render_plan_with_analysis` path returns that JPEG together with four exact 256-bin histograms
from the uncompressed display-encoded sRGB proxy before JPEG encoding and strict processed-linear working-RGB `< 0` /
`> 1` per-channel and any-channel clipping counts from before output clamping. Rust validates the
analysis version, dimensions, bin lengths/sums, and clipping union bounds before exposing fixed
arrays. Neither render path reopens nor decodes the
RAW. The safe wrapper is `Send + Sync`: its C++ working buffer is immutable,
each render owns all temporary state, and no LibRaw object survives preparation. The warm-session
edge is independently capped at 4096; 1600/2048 are the intended UI choices. The existing
`render_libraw_edited_proxy` one-shot convenience API remains available for stateless callers.

`LibRawEditDetailSession::open(path)` is the separate 1:1 path. It checks decoder metadata against
a worst-case RGB u16 allocation before the reference render starts, verifies the actual retained
allocation independently, and rejects either above 512 MiB. Its opaque C++ handle retains the
immutable full-resolution processed-linear u16 RGB source in sRGB primaries but no decoder.
`render_plan_tile` accepts an unscaled, in-bounds rectangle whose width and height are each at
most 1024. Pixel-local plans normalize only that crop. Neighborhood plans such as Sharpen first
expand it by the conservative sum of enabled operation footprints, capped at a 512-pixel apron
and a 2048-pixel working side, execute on that expanded linear-float region, and return only the
requested core as tightly packed display-encoded sRGB RGB8 bytes. It deliberately does not JPEG-encode
individual tiles, avoiding independent chroma/block
boundaries at tile seams. The detail wrapper is also `Send + Sync`; concurrent calls read the
retained source and own all crop/edit/output memory independently.

Compressed embedded previews and generated proxies are small enough to cross as owned bytes.
Large mosaic and full-resolution u16 RGB buffers remain in C++; detail requests copy only bounded
RGB8 tiles across FFI rather than exposing `Vec<u16>`.

`decode_jpeg_display_luma(bytes, max_edge)` is the analysis-side compressed-payload bridge. It
accepts `max_edge` only in 1 through 512 and returns owned `width`, `height`, sample `stride`,
normalized `Vec<f32>` luma, and the exact preprocessing version. The output is tightly packed and
can be borrowed directly by `shadow_ai::DisplayLumaPlane`. Its semantics are explicitly an
assumed-sRGB JPEG display proxy without ICC or orientation interpretation, never a RAW-domain
measurement. Its v1 identity includes the discovered libjpeg-turbo package revision,
RGB8 output, slow integer DCT, disabled fancy upsampling and block smoothing, fixed IDCT
scale/resize/luma rules, and the requested edge. Rust rejects a C++ result whose reported identity
does not match the build-time contract. This makes incompatible preprocessing detectable; it does
not by itself make differently authored embedded/generated proxies comparable.

## Path contract

The first implementation is Mac-first and accepts a UTF-8 path. This limitation is explicit: `inspect_libraw` rejects a non-UTF-8 `Path` instead of using a lossy display string. The Windows adapter will add a native UTF-16 entry point while preserving the existing provider and snapshot schemas.

## Build

Cargo uses CXX 1.0.198 and compiles the same `shadow-image` sources used by CMake. LibRaw 0.22+ and 8-bit libjpeg-turbo with in-memory source support are discovered through `pkg-config`; unsupported JPEG builds fail at compile time. The discovered libjpeg-turbo version is embedded in the Rust and C++ preprocessing contracts. On macOS the build script filters Homebrew's obsolete `-lstdc++` entry because CXX already links libc++.

```sh
cargo test --package shadow-bridge
cargo run --package shadow-cli -- inspect-raw /absolute/path/to/input.dng
cargo xtask raw-smoke /absolute/path/to/raw-fixtures
```

The ignored `real_dng_snapshot_crosses_the_bridge` and
`real_dng_full_edit_detail_session_renders_deterministic_tiles` tests can be enabled with an
absolute local fixture path in `SHADOW_TEST_DNG`. The ignored
`real_raw_folder_smoke_matrix` recursively checks a mixed-vendor fixture directory supplied by
`SHADOW_TEST_RAW_FOLDER`; `cargo xtask raw-smoke` is its repository-level entry point.
