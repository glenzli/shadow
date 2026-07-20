# shadow-image

`shadow-image` is Shadow's platform-neutral C++20 image kernel. Its first implemented provider uses LibRaw, while callers depend only on the types in `include/shadow/image/decoder.hpp`.

## Decoder boundary

```text
DecoderProvider
└─ DecodeSession
   ├─ AssetMetadata
   ├─ DecodeCapabilities + PendingCorrections
   ├─ PreviewDescriptor[] → PreviewPayload
   ├─ MosaicBuffer
   ├─ PixelBuffer (reference RGB only)
   └─ EncodedProxy (bounded display JPEG fallback)
```

The public header does not expose LibRaw objects, enums, pointers, or ownership rules. A provider owns its decoder implementation; returned buffers own their memory and remain valid after subsequent session calls.

Rust consumes owned metadata/capability/preview snapshots, the selected embedded preview, and a final compressed proxy through the CXX adapter in `src/bridge/cxx_bridge.cpp`. The bridge is intentionally coarse-grained: full-size mosaic/RGB buffers remain in C++, where the fallback path performs bilinear downscaling and libjpeg-compatible encoding before transferring bytes.

Current contract rules:

- A file without an embedded preview is valid and can still expose mosaic/RGB capabilities.
- Preview IDs are provider IDs, not vector positions. `select_best_preview` chooses the largest decodable candidate.
- DNG opcode lists are surfaced as `PendingCorrections` until Shadow can prove they were applied.
- `render_reference_rgb` is a correctness/fallback path. It is not Shadow's final scene-linear color pipeline.
- `render_reference_proxy_jpeg` bounds the longest edge (2048 and quality 88 in the first recipe) and rejects unbounded requests. Its version belongs in the cache key.
- `decode_jpeg_display_luma` is a separate analysis path over compressed display proxies. It requires 8-bit libjpeg-turbo with in-memory sources, rejects encoded inputs above 128 MiB and source headers above 65,535 per axis or 100 million pixels, applies a stricter 50-million-pixel limit to multi-scan inputs, caps libjpeg memory at 256 MiB, and bounds scaled intermediates before emitting a tightly packed normalized `float` luma plane with a caller-selected edge in 1 through 512. Corrupt-data warnings, including synthesized end-of-image recovery for truncation, fail closed.
- A `DecodeSession` is thread-confined. Providers may be shared; parallel work should open independent sessions.
- The current owned `MosaicBuffer` intentionally copies LibRaw memory. A later opaque/tiled buffer can remove that copy without changing metadata semantics.

## CPU edit reference

`include/shadow/image/edit.hpp` defines the first correctness-oriented edit path. Its input is
explicitly native interleaved RGB float32, scene-referred, linear-light data with named RGB
primaries, white point, and luminance coefficients. It is not legal to feed the decoder's
display-referred `PixelBuffer` directly into this path: a future camera/display color transform
must establish the declared working space first.

The version-1 ordered node executor currently supports exposure, pivoted contrast, the versioned
piecewise-linear Tone Curve, resolved RGB channel gains, and luma-preserving saturation. It
deliberately preserves negative
and greater-than-one scene values, performs no implicit gamut mapping or clipping, rejects
NaN/Inf and float overflow, and refuses unknown schema/implementation versions. Node order is
observable and stable. This ordered executor is the CPU reference subset of the future typed DAG;
masks, branching, blending, tile scheduling, and GPU implementations remain separate work.
`validate_adjustment_nodes` exposes the same parameter validation without requiring pixels, so
the one-shot edited-proxy path rejects malformed plans before asking a decoder to render RGB.

Tone Curve is available both through the standalone `apply_tone_curve` reference operator and as
a normal ordered executor node. Version 1 uses 2 through 256 finite control points whose
strictly increasing x coordinates span exactly 0 through 1. It applies a piecewise-linear curve
to each scene-linear working-RGB channel, interpolates normalized samples, and extrapolates
negative and super-white samples with the endpoint segment slopes. It never clips. This is a
transparent baseline for parity tests; it does not claim to be a final
perceptual, luminance-only, or display-referred tone-curve model.

`WarmEditPreviewSession` is the interactive path for this exact version-1 subset. Preparation
asks the decoder for reference RGB once, converts samples to scene-linear sRGB, and bilinearly
downsamples them into an immutable float working proxy before any adjustment. Exposure, pivot
contrast, RGB gains, and saturation follow the current linear proxy assumptions. Tone Curve is
nonlinear, so executing it on the prepared proxy is an interactive approximation rather than a
bit-equivalent full-resolution result; masked and neighborhood nodes require an explicitly
different preview strategy. The warm edge is capped at 4096 (at most 192 MiB for a
square interleaved RGB float32 proxy; typical 3:2 images and the UI's 1600/2048 choices use less).
Each render owns its output/edit/JPEG buffers, so const renders may safely run concurrently; the
original decoder session is neither retained nor revisited during slider interaction.

`render_jpeg_with_analysis` freezes a transient sidecar from that same complete warm render.
R/G/B and encoded Rec.709 luma each use 256 `u64` bins over the uncompressed display-sRGB
RGB8 result immediately before JPEG encoding. Separate per-channel and any-channel counts inspect
the edited scene-linear values immediately before output clamping and use strict `< 0` and `> 1`;
exact endpoints are not called clipped. The sidecar is complete-proxy output analysis, not RAW
sensor exposure, full-resolution statistics, or persisted evidence. It owns no extra per-pixel
luma plane and remains call-local for concurrent renders.

## Display-luma analysis boundary

`include/shadow/image/display_luma.hpp` names the complete preprocessing contract returned with
every plane. Version 2 pins the exact libjpeg-turbo package revision, RGB8 output, `JDCT_ISLOW`,
disabled fancy upsampling and block smoothing, 1/2/4/8 IDCT scale selection, assumed encoded sRGB,
no ICC transform, stored pixel orientation, fixed-point encoded-domain Rec.709 luma,
center-aligned bilinear resize, and the requested maximum edge. A dependency or setting change
therefore creates a different persisted preprocessing identity instead of silently reusing old
measurements. This is deliberately a reproducible observation of a display proxy, not RAW
exposure or sensor luminance. The libjpeg fatal-error callback is contained inside a C-style
allocation frame; C++ owned output is constructed only after that `setjmp` boundary has finished,
so corrupt data cannot jump across live C++ containers.

## Build and test

```sh
cargo xtask native-check
```

This configures CMake, builds the library and probe, and runs CTest contract tests. Real RAW samples stay in the ignored local reference area; public deterministic fixtures will be added separately.
