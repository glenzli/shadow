# shadow-bridge

`shadow-bridge` is the coarse-grained CXX boundary between Rust application state and the C++20 image kernel.

The decoder side of the bridge follows this coarse-grained path:

```text
Rust path
→ CXX open_libraw_utf8
→ C++ DecoderProvider / DecodeSession
→ owned metadata + capability + preview snapshots / compressed visual payload
→ pure shadow-domain types
```

No LibRaw or CXX type escapes the crate's public API. `inspect_libraw` returns a serializable descriptor snapshot; `extract_best_libraw_preview` returns the kernel-selected embedded preview or `None`; `render_libraw_reference_proxy` returns a bounded display JPEG for the no-preview fallback. The edit boundary accepts a bounded, dependency-ordered `AdjustmentRenderPlan` containing exposure, contrast, Tone Curve, resolved RGB channel gain, and saturation nodes. Plans contain at most 256 uniquely identified nodes and validate versions, finite parameters, and Tone Curve structure before execution. Graph topology, processing stages, masks, layer blending, and shared revisions are compiled before this boundary rather than interpreted here. `render_libraw_edited_proxy` remains the four-node Basic compatibility API, while `render_libraw_adjustment_plan` is the typed one-shot path. RGB gains are post-demosaic adjustments, not RAW white balance. C++ exceptions become `BridgeError`; Rust panics and C++ exceptions never cross the language boundary directly.

For slider interaction, `LibRawEditPreviewSession::open(path, max_edge)` performs that RAW render
once and retains only a bounded scene-linear float working proxy. Repeated
`render(edits, jpeg_quality)` calls provide the four-node Basic compatibility path;
`render_plan(plan, jpeg_quality)` executes a validated typed plan. Neither reopens nor decodes the
RAW. The safe wrapper is `Send + Sync`: its C++ working buffer is immutable,
each render owns all temporary state, and no LibRaw object survives preparation. The warm-session
edge is independently capped at 4096; 1600/2048 are the intended UI choices. The existing
`render_libraw_edited_proxy` one-shot convenience API remains available for stateless callers.

Compressed embedded previews and generated proxies are small enough to cross as owned bytes. Large mosaic and RGB buffers intentionally remain in C++; their future bridge will use opaque handles and tile requests, not `Vec<u16>` copies across FFI.

`decode_jpeg_display_luma(bytes, max_edge)` is the analysis-side compressed-payload bridge. It
accepts `max_edge` only in 1 through 512 and returns owned `width`, `height`, sample `stride`,
normalized `Vec<f32>` luma, and the exact preprocessing version. The output is tightly packed and
can be borrowed directly by `shadow_ai::DisplayLumaPlane`. Its semantics are explicitly an
assumed-sRGB JPEG display proxy without ICC or orientation interpretation, never a RAW-domain
measurement. Its version-2 identity includes the discovered libjpeg-turbo package revision,
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
```

The ignored `real_dng_snapshot_crosses_the_bridge` test can be enabled with an absolute local fixture path in `SHADOW_TEST_DNG`.
