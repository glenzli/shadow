# shadow-image

`shadow-image` is Shadow's platform-neutral C++20 image kernel. Its first implemented provider uses LibRaw, while callers depend only on the types in `include/shadow/image/decoder.hpp`.

## Decoder boundary

```text
DecoderProvider
└─ DecodeSession
   ├─ AssetMetadata
   ├─ DecodeCapabilities + PendingCorrections
   ├─ PreviewDescriptor[] → PreviewPayload
   ├─ RawFrame (owned sensor samples)
   ├─ PixelBuffer (reference RGB only)
   └─ EncodedProxy (bounded display JPEG fallback)
```

The public header does not expose LibRaw objects, enums, pointers, or ownership rules. A provider owns its decoder implementation; returned buffers own their memory and remain valid after subsequent session calls.

Rust consumes owned metadata/capability/preview snapshots, the selected embedded preview, and a final compressed proxy through the CXX adapter in `src/bridge/cxx_bridge.cpp`. The bridge is intentionally coarse-grained: full-size mosaic/RGB buffers remain in C++, where the fallback path performs bilinear downscaling and libjpeg-compatible encoding before transferring bytes.

Current contract rules:

- A file without an embedded preview is valid and can still expose RawFrame/RGB capabilities.
- A recognized file whose LibRaw decoder is flagged `UNSUPPORTED_FORMAT` keeps factual metadata
  and embedded-preview capabilities but does not advertise RawFrame/reference-RGB support. This is
  the expected preview-only path for Nikon Z9 HE/HE* NEF until an external provider is available.
- Preview IDs are provider IDs, not vector positions. `select_best_preview` chooses the largest decodable candidate.
- DNG opcode lists are surfaced as `PendingCorrections` until Shadow can prove they were applied.
- `render_reference_rgb` explicitly returns processed, linear-light 16-bit RGB in
  sRGB/Rec.709-D65 primaries. LibRaw has already applied black subtraction, white balance,
  demosaic, camera-to-output conversion, and fixed integer-range scaling; histogram brightness and
  frame-adaptive maximum adjustment are disabled. This is neither encoded sRGB nor untouched
  sensor-linear data, and it is not Shadow's final RAW color pipeline.
- `render_reference_proxy_jpeg` bounds the longest edge (2048, quality 95, and 4:4:4 chroma in the current recipe) and rejects unbounded requests. Its version belongs in the cache key.
- `decode_jpeg_display_luma` is a separate analysis path over compressed display proxies. It requires 8-bit libjpeg-turbo with in-memory sources, rejects encoded inputs above 128 MiB and source headers above 65,535 per axis or 100 million pixels, applies a stricter 50-million-pixel limit to multi-scan inputs, caps libjpeg memory at 256 MiB, and bounds scaled intermediates before emitting a tightly packed normalized `float` luma plane with a caller-selected edge in 1 through 512. Corrupt-data warnings, including synthesized end-of-image recovery for truncation, fail closed.
- A `DecodeSession` is thread-confined. Providers may be shared; parallel work should open independent sessions.
- `RawFrame` intentionally copies LibRaw memory and preserves raw-coordinate samples, active
  margins, CFA layout, per-CFA black/white calibration, as-shot neutral, an optional explicit
  Camera RGB -> XYZ D50 matrix, and pending DNG opcode declarations. It is explicitly
  pre-demosaic; unsupported CFA layouts remain inspectable but cannot enter Bayer-only
  processing. A later opaque/tiled buffer can remove this copy without changing the frame
  semantics.

### Local private-provider development path

`make_photo_decoder_provider()` is the normal application route. It uses raster decoding for
JPEG/HEIF, otherwise public LibRaw by default. It also discovers local native decoder modules
from a per-user plugin root:

```text
macOS:   ~/Library/Application Support/Shadow/plugins/decoders/
Windows: %LOCALAPPDATA%/Shadow/plugins/decoders/
Linux:   $XDG_DATA_HOME/shadow/plugins/decoders/
```

Each `*.shadow-decoder-link` file is a small UTF-8 local pointer, not a bundled module:

```text
shadow-private-decoder-link-v1
module=/absolute/path/to/private-decoder-module.dylib
```

Files are considered in filename order, duplicate module targets are ignored, and each private
provider must return explicit `unsupported` before the router tries the next local provider or
public LibRaw. Decode, SDK licence, data and resource errors are surfaced rather than silently
hidden by fallback. A module rebuild at the same path invalidates its decode identity through its
canonical path, size and modification time.

`SHADOW_PRIVATE_DECODER_PLUGIN_PATH` remains a strict one-module override for CI and direct local
debugging; when it is set, automatic discovery is deliberately skipped. `SHADOW_PLUGIN_DIRECTORY`
can override the plugin-root location itself for isolated development or tests.

When `BUILD_TESTING` is enabled, CMake builds
`shadow-image-libraw-dummy-private-provider`: a deliberately non-proprietary module that merely
wraps Shadow's bundled LibRaw provider through exactly the same descriptor/create/destroy ABI as a
future private camera adapter. It is intended to exercise the route end to end before any vendor
SDK is involved:

```sh
export SHADOW_PRIVATE_DECODER_PLUGIN_PATH="$PWD/build/desktop-dev/cpp/shadow-image/libshadow-image-libraw-dummy-private-provider.so"
./build/desktop-dev/cpp/shadow-image/shadow-raw-probe /path/to/photo.raw ./bench-results/provider-smoke
```

The probe and desktop both use `make_photo_decoder_provider()`, so the reported provider identity
confirms whether this route was selected. The route's cache identity includes the provider's own
version as well as a compact canonical-path/size/mtime module fingerprint, so rebuilding a local
module invalidates old decode artifacts even if its author accidentally forgets to advance a
version string. The module is a development fixture, not a public Nikon decoder and does not
contain vendor code or calibration data.

## CPU edit reference

`include/shadow/image/edit.hpp` defines the first correctness-oriented edit path. Its input is
explicitly native interleaved RGB float32, scene-referred, linear-light data with named RGB
primaries, white point, and luminance coefficients. It is not legal to feed the decoder's
integer `PixelBuffer` directly into this path: the proxy boundary validates its explicit
processed-linear contract, normalizes it, and establishes the declared float working space.

The ordered node executor currently supports exposure, pivoted contrast, versioned RGB Tone
Curves, processed-RGB CAT16 temperature/tint adaptation, luma-preserving saturation, selective
tone, perceptual color controls, and detail/effects. It
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
asks the decoder for processed linear RGB once, normalizes the samples, and bilinearly downsamples
them into an immutable linear-sRGB float working proxy before any adjustment. Exposure, pivot
contrast, RGB white balance, and saturation follow the current linear proxy assumptions. Tone Curve is
nonlinear, so executing it on the prepared proxy is an interactive approximation rather than a
bit-equivalent full-resolution result; masked and neighborhood nodes require an explicitly
different preview strategy. The warm edge is capped at 4096 (at most 192 MiB for a
square interleaved RGB float32 proxy; typical 3:2 images and the UI's 1600/2048 choices use less).
Each render owns its output/edit/JPEG buffers, so const renders may safely run concurrently; the
original decoder session is neither retained nor revisited during slider interaction.

`render_jpeg_with_analysis` freezes a transient sidecar from that same complete warm render.
R/G/B and encoded Rec.709 luma each use 256 `u64` bins over the uncompressed display-sRGB
RGB8 result immediately before JPEG encoding. Output-transform v1 accepts only the declared
linear-sRGB working space, reduces out-of-gamut Oklab chroma along a constant-hue ray, clamps
display lightness only at black/white, then applies the sRGB transfer and quantization. It is not
an HDR tone mapper. Separate per-channel and any-channel counts inspect the edited scene-linear
values before that display transform and use strict `< 0` and `> 1`; exact endpoints are not
called clipped. The sidecar is complete-proxy output analysis, not RAW sensor exposure,
full-resolution statistics, or persisted evidence. It owns no extra per-pixel luma plane and
remains call-local for concurrent renders.

`include/shadow/image/lut.hpp` provides the first LUT resource contract. It strictly parses
bounded 3D `.cube` documents (2³ through 65³ entries), preserves explicit domains and canonical
red-fastest storage order, rejects 1D/malformed files, and provides clamped-domain trilinear
sampling. LUT resource ownership and Grade Node persistence remain outside this image-kernel
format/parser boundary.

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
