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

## Build and test

```sh
cargo xtask native-check
```

This configures CMake, builds the library and probe, and runs CTest contract tests. Real RAW samples stay in the ignored local reference area; public deterministic fixtures will be added separately.
