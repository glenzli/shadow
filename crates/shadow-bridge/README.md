# shadow-bridge

`shadow-bridge` is the coarse-grained CXX boundary between Rust application state and the C++20 image kernel.

## Source index

The generated CXX wire declaration stays centralized and auditable in [`src/lib.rs`](src/lib.rs).
The safe Rust side is organized by independent contract family even when several families consume
that same private wire representation:

- [`src/decoder.rs`](src/decoder.rs) owns source-neutral inspection, embedded-preview extraction,
  and shared decoder wire mappings.
- [`src/display_luma.rs`](src/display_luma.rs) owns bounded, versioned display-proxy analysis.
- [`src/lut_baking.rs`](src/lut_baking.rs) owns bounded color-only LUT sampling, shared cancellation,
  production-parser admission, and measured interpolation-error results.
- [`src/optics.rs`](src/optics.rs) owns optical settings, profile discovery, and execution
  receipts.
- [`src/raw_development.rs`](src/raw_development.rs) owns RAW plan values, wire conversion,
  negotiation results, and source-development provenance receipts.
- [`src/adjustment/mod.rs`](src/adjustment/mod.rs) is the stable public adjustment index and
  cross-operation plan composition boundary. Its children are the semantic owners:
  [`geometry.rs`](src/adjustment/geometry.rs) for final crop, orientation, straighten, and bounded
  perspective Canvas state;
  [`local_mask.rs`](src/adjustment/local_mask.rs) for spatial intent and bounded immutable
  managed-raster admission, and [`retouch.rs`](src/adjustment/retouch.rs) for local repair intent;
  [`oklab_lightness_curve.rs`](src/adjustment/oklab_lightness_curve.rs),
  [`selective_tone.rs`](src/adjustment/selective_tone.rs),
  [`perceptual_color.rs`](src/adjustment/perceptual_color.rs), and
  [`oklab_color_warper.rs`](src/adjustment/oklab_color_warper.rs) for tone/color families;
  [`detail_effects.rs`](src/adjustment/detail_effects.rs) for the shared 37-value payload and its
  three pass contracts; and [`lut.rs`](src/adjustment/lut.rs) for bounded immutable LUT documents.
  [`parameter_validation.rs`](src/adjustment/parameter_validation.rs) is the narrow internal
  finite/range primitive shared by those validators. The index still owns the Basic Edit
  compatibility-plan builder and its edited-proxy request until those caller-facing adapters are
  extracted as the next boundary.
- [`src/preview_analysis.rs`](src/preview_analysis.rs) owns warm-preview histograms, source
  clipping masks, execution provenance, and fail-closed analysis validation.
- [`src/preview_frame.rs`](src/preview_frame.rs) owns the move-only native interactive-frame
  handle, descriptor validation, and RGB8/R8 slices tied to that owner's lifetime.
- [`src/preview_session.rs`](src/preview_session.rs) owns reusable warm-preview state,
  cancellation, and generation-matched render outcomes.
- [`src/detail_session.rs`](src/detail_session.rs) owns the retained full-resolution source,
  bounded tile requests, and tightly packed RGB8 output validation.
- [`src/render_wire.rs`](src/render_wire.rs) is the single auditable adapter from typed
  adjustment, geometry, and tile contracts to the flat private CXX wire.
- [`src/provider.rs`](src/provider.rs) owns provider identities, supported source declarations,
  and RAW preflight negotiation entry points.
- [`src/one_shot.rs`](src/one_shot.rs) owns stateless reference-proxy and adjustment-plan renders.
- [`src/error.rs`](src/error.rs) owns the public failure vocabulary shared by safe bridge modules.
- [`src/lib.rs`](src/lib.rs) is the public module index and the centralized generated CXX wire
  declaration.

On the native side, the public ABI remains in
[`cxx_bridge.hpp`](../../cpp/shadow-image/include/shadow/image/cxx_bridge.hpp) and its composition
shim. The leaf
[`cxx_preview_frame.hpp`](../../cpp/shadow-image/include/shadow/image/cxx_preview_frame.hpp) and
[`cxx_preview_frame.cpp`](../../cpp/shadow-image/src/bridge/cxx_preview_frame.cpp) own the first
`UniquePtr` frame boundary and its borrowed immutable slices. The internal
[`adjustment_render_wire.cpp`](../../cpp/shadow-image/src/bridge/adjustment_render_wire.cpp)
owns the complete Rust-to-C++ Adjustment decoder: every operation variant, local-mask layer
boundary (including the kind-six raster metadata/payload record), retouch target/stroke, node
limit, and stable invalid-request diagnostic. Cargo compiles it only as part of the
`shadow-bridge` CXX shim; it is not a `Shadow::Image` source.

Sharing the CXX representation is not by itself a reason to share one Rust source file. Extract a
safe contract when it has its own invariants, failure policy, and consumers; keep the wire
declaration intact unless the generated ABI itself gains a separately versioned boundary.

The adjacent [`src/tests/mod.rs`](src/tests/mod.rs) routes private bridge-contract tests to RAW
development, display luma, aggregate adjustment plans, Perceptual Color, Oklab Color Warper,
preview execution, detail sessions, and opt-in real-source modules. The two color contract files
mirror their production owners while `adjustment_plan.rs` owns compatibility and aggregate plan
behavior spanning registry validation, Tone Curve, Selective Tone/detail, and retouch. Test
fixtures stay with the contract that consumes them. Do not add another multi-domain test block to
`lib.rs`, and do not split the generated ABI merely to satisfy a line count.

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
`render_plan(plan, jpeg_quality)` executes a validated typed plan. Interactive callers use
`render_plan_interactive_frame_cancellable`, which moves tightly packed display-sRGB RGB8 and
optional paired R8 coverage into one native owner and borrows both slices without a
native-to-Rust full-frame copy. On Apple Metal, that same opaque owner can instead expose a
buffer-backed RGBA8-sRGB texture descriptor `{resource, texture, device, stride, format}`; Rust
projects only the descriptor while the owner keeps the Metal allocation alive. The desktop may
explicitly materialize packed RGB8 for a named software/failure fallback, but native presentation
does not traverse either Rust bytes or JPEG. The legacy `render_plan_rgb8_cancellable` materializer remains for
compatibility. Both routes skip compression and are transient rather than cache artifacts. The parallel
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
allocation independently against a source-kind limit: 512 MiB for packed u16 raster sources and
1 GiB for owned scene-linear fp32 RawFrame development. Its opaque C++ handle retains the
immutable full-resolution linear RGB source in sRGB primaries but no decoder.
`render_plan_tile` accepts an unscaled, in-bounds rectangle whose width and height are each at
most 1024. Pixel-local plans normalize only that crop. Neighborhood plans such as Sharpen first
expand it by the conservative sum of enabled operation footprints, capped at a 512-pixel apron
and a 2048-pixel working side, execute on that expanded linear-float region, and return only the
requested core as tightly packed display-encoded sRGB RGB8 bytes. It deliberately does not JPEG-encode
individual tiles, avoiding independent chroma/block
boundaries at tile seams. The detail wrapper is also `Send + Sync`; concurrent calls read the
retained source and own all crop/edit/output memory independently.

`render_plan_tile16` is the export-only counterpart. It forces CPU replay through the same
full-detail plan and returns tightly packed display-encoded RGB16 samples for true 16-bit TIFF
assembly. The caller validates the direct-execution receipt and stitches only bounded tiles; this
path is not used by interactive preview or presentation.

Compressed embedded previews and durable generated proxies are small enough to cross as owned
bytes. Interactive RGB8 and R8 coverage instead stay in one immutable native frame owner and cross
as borrowed slices. Large mosaic and full-resolution u16 RGB buffers remain in C++; detail
requests copy only bounded RGB8 tiles across FFI. RGB16 crosses the boundary only for explicit
high-bit-depth export tiles.

AI RAW foundations use a separate, explicit large-payload contract in
[`src/raw_foundation.rs`](src/raw_foundation.rs). The verified artifact/cache owner supplies
path-free source, artifact, and cache-key SHA-256 identities plus finite interleaved linear-camera
RGB. Rust seals the one supported public RawNIND model/revision, validates zero-or-one Bayer crop,
exact sample count, arithmetic, and a 1 GiB ceiling, then owns that `Vec<f32>` for one synchronous
preview/detail preparation call. `src/bridge/raw_foundation_wire.*` copies only the small
provenance strings and borrows the pixel vector as `std::span<const float>`; the returned native
session owns an independent scene-linear result and retains neither the Rust vector nor a local
artifact path. Enabled-foundation methods are fail-closed and cannot invoke the ordinary Bayer or
provider-RGB overload.

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

Cargo uses CXX 1.0.198 and compiles the same `shadow-image` translation units used by CMake.
[`build.rs`](build.rs) reads the canonical portable, Metal, and fallback manifests
from [`cpp/shadow-image/cmake/source-manifests/`](../../cpp/shadow-image/cmake/source-manifests/);
CMake consumes the same files. Adding or extracting a native implementation therefore updates
one source index, and both build graphs fail closed on malformed, duplicate, missing, or empty
manifest entries. Header-only dependencies remain in the bridge's additional-input manifest.

LibRaw 0.22+ and 8-bit libjpeg-turbo with in-memory source support are discovered through
`pkg-config`; unsupported JPEG builds fail at compile time. The discovered libjpeg-turbo version
is embedded in the Rust and C++ preprocessing contracts. On macOS the build script filters
Homebrew's obsolete `-lstdc++` entry because CXX already links libc++.

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
