# shadow-image

`shadow-image` is Shadow's platform-neutral C++20 image kernel. Its first implemented provider
uses LibRaw behind provider-neutral public contracts.

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

`include/shadow/image/decoder.hpp` is the compatibility and navigation entry point. New production
code should include the narrow semantic owner directly:

- `decoder_types.hpp` / `src/decoder/decoder_types.cpp` own small shared value types and their
  invariants; `decoder_error.hpp` / `src/decoder/decoder_error.cpp` own stable failure categories,
  provider codes, and diagnostics.
- `libraw_development_settings.hpp` owns the concrete LibRaw renderer configuration.
- `src/decoder/libraw_runtime.*` owns the narrow LibRaw error/open/declared-opcode boundary shared
  by an opened decoder and a fresh renderer. `src/decoder/libraw_reference_development.*` owns the
  independent processed-linear reference lifecycle: settings validation and identity, capability
  negotiation, fresh LibRaw allocation/open/unpack/process, preview bounding, and the complete
  development receipt. `libraw_decoder.cpp` retains source metadata, embedded previews, and the
  provider-neutral RawFrame session, then delegates processed reference work to that owner.
- `raw_development_plan.hpp` owns requested RAW intent and capability negotiation, while
  `raw_development_receipt.hpp` owns the auditable execution result.
- `decoder_metadata.hpp` / `src/decoder/decoder_metadata.cpp` own source facts,
  embedded-preview descriptors, provider-ID selection, and format names.
- `raw_frame.hpp` owns untouched sensor samples; `reference_pixels.hpp` owns processed reference
  pixels and their output contracts.
- `decoder_session.hpp` owns opened-source/provider lifetimes; `proxy_rendering.hpp` owns bounded
  encoded-proxy requests and results.

These headers form a one-way dependency graph rather than a hidden prelude. The public contracts
do not expose LibRaw objects, enums, pointers, or ownership rules. A provider owns its decoder
implementation; returned buffers own their memory and remain valid after subsequent session
calls. The focused `decoder_types`, `decoder_error`, and `decoder_metadata` contract tests sit
beside the provider-level decoder-source contract, so shared invariants do not depend on linking
the LibRaw translation unit that happens to consume them.

Rust consumes owned metadata/capability/preview snapshots, the selected embedded preview, and a
final compressed proxy through the CXX adapter. `src/bridge/cxx_bridge.cpp` owns DTO projection and
stateless provider entry points; `src/bridge/cxx_handle.cpp` owns the decode, warm-preview, and
full-detail session lifecycles. `src/bridge/adjustment_render_wire.cpp` owns Recipe node projection.
The bridge remains intentionally coarse-grained: full-size mosaic/RGB buffers stay in C++, where
the fallback path performs bilinear downscaling and libjpeg-compatible encoding before transferring
bytes.

Current contract rules:

- A file without an embedded preview is valid and can still expose RawFrame/RGB capabilities.
- A recognized file whose LibRaw decoder is flagged `UNSUPPORTED_FORMAT` keeps factual metadata
  and embedded-preview capabilities but does not advertise RawFrame/reference-RGB support. This is
  the expected preview-only path for Nikon Z9 HE/HE* NEF until an external provider is available.
- Preview IDs are provider IDs, not vector positions. `select_best_preview` chooses the largest decodable candidate.
- DNG opcode lists are surfaced as `PendingCorrections` until Shadow can prove they were applied.
- `develop_source_reference` is the application source boundary. Supported public LibRaw and
  private-provider files both expose `RawFrame` and enter Shadow's owned black subtraction,
  normalization, Bayer reconstruction, white balance and camera-to-linear-sRGB path. An exact
  local DCP may replace the provider's generic matrix; profiles carrying unsupported creative
  tables are rejected as a whole. `render_reference_rgb` remains only the explicit
  provider-processed compatibility route. Every choice and fallback is recorded in the pipeline
  receipt and cache identity.
- `render_reference_proxy_jpeg` bounds the longest edge (2048, quality 95, and 4:4:4 chroma in the current recipe) and rejects unbounded requests. Its version belongs in the cache key.
- `decode_jpeg_display_luma` is a separate analysis path over compressed display proxies. It requires 8-bit libjpeg-turbo with in-memory sources, rejects encoded inputs above 128 MiB and source headers above 65,535 per axis or 100 million pixels, applies a stricter 50-million-pixel limit to multi-scan inputs, caps libjpeg memory at 256 MiB, and bounds scaled intermediates before emitting a tightly packed normalized `float` luma plane with a caller-selected edge in 1 through 512. Corrupt-data warnings, including synthesized end-of-image recovery for truncation, fail closed.
- A `DecodeSession` is thread-confined. Providers may be shared; parallel work should open independent sessions.
- `RawFrame` intentionally copies LibRaw memory and preserves raw-coordinate samples, active
  margins, CFA layout, per-CFA black/white calibration, as-shot neutral, an optional explicit
  Camera RGB -> XYZ D50 matrix, and pending DNG opcode declarations. It is explicitly
  pre-demosaic; unsupported CFA layouts remain inspectable but cannot enter Bayer-only
  processing. A later opaque/tiled buffer can remove this copy without changing the frame
  semantics.
- Native-size Bayer reconstruction, the precompiled camera transform and orientation are fused
  into one output pass. The CPU path remains the exact reference. On macOS, Metal v1 performs the
  same full-detail contract in fp32 and bounded output tiles; area-integrated catalog previews
  remain on CPU. The actual `shadow-fused-raw-cpu-v1` or
  `shadow-fused-raw-metal-full-v1` identity is cache-visible. Metal failure in automatic mode
  falls back to CPU inside the RawFrame route and can never silently select provider-processed
  RGB.

Runtime controls:

```text
SHADOW_RAW_PIPELINE=auto|raw-frame|processed
SHADOW_IMAGE_ACCELERATION=auto|cpu|metal
```

`metal` requires Metal for eligible native-size work; area previews deliberately continue to use
CPU. Build-time `SHADOW_ENABLE_METAL=OFF` compiles the same public API against a cross-platform
stub.

The Metal implementation also follows the language boundary.
`src/raw/metal_raw_development_msl.hpp` owns the complete MSL reconstruction, CFA denoise, area
preview, and DCP post-processing program. `src/raw/metal_raw_development.mm` owns the mirrored host
ABI, process-wide device and pipeline lifecycle, request validation, buffer/command dispatch, and
result projection. Editing one side requires checking the shared struct layout assertions and all
four shader entry-point names.

DCP color development has a one-way internal owner graph.
`src/raw/dcp_color_matrix_math.hpp` owns the shared 3×3 algebra, standard white points, and
Bradford adaptation. `src/raw/dcp_color_rendering.*` owns HueSatMap/LookTable/tone-curve
preparation plus CPU/Metal post-matrix pixel execution. `src/raw/dcp_color_development.cpp`
retains camera-neutral interpretation, single/dual-illuminant calibration, matrix-route
selection, immutable transform composition, and receipt identity.

Metal-capable CI or a local release gate should configure
`SHADOW_REQUIRE_METAL_TESTS=ON`. That mode makes CTest require a real Metal device, forces the
full RawFrame pipeline onto Metal, and lowers the scheduling-only tile budget so the compact
fixture exercises multi-tile copies. Ordinary portable tests keep this option off and validate the
same API against the CPU/stub implementation.

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

`shadow-image-decode-helper neutral-detail-tile` is a development isolation proof, not the current
Recipe-aware edit or export route. It proves that a child process can open a source through the
private-provider boundary, prepare neutral full-detail pixels, bind its receipt to a caller nonce,
and atomically publish a tightly packed RGB8 tile. The
`shadow-image-neutral-detail-helper-contract` CTest launches the real helper with the existing
private-provider fixture and verifies its complete 2-by-1 rectangle/full-dimensions receipt,
six-byte row layout, pipeline identity, and six-byte artifact. It does not replace that process
boundary with parsed fixture text.

The desktop does not currently expose this neutral operation as a Precision editing capability.
A future crate-private `isolated_detail_worker` should own child scheduling and cancellation,
source/helper/Recipe/geometry identities, receipt validation, and cache publication. That worker
must introduce a new versioned Recipe-aware operation; it must not overload this neutral proof,
promote an unowned parser into the public crate facade, or substitute a JPEG proxy for a RAW detail
tile.

Lensfun optics has two production owners behind the stable `OpticsProvider` API.
`src/optics/lensfun_profile_catalog.*` owns database selection and loading, normalized camera
identity lookup, compatible-lens projection, explicit/manual profile resolution, synchronization,
and the match cache. `src/optics/lensfun_optics.cpp` consumes an immutable match and owns settings
validation, manual fallback correction, Lensfun modifier projection, and packed/scene-linear pixel
execution.

Decoder contract tests follow the production responsibilities instead of one aggregate executable:

- `tests/decoder_source_contract_test.cpp` owns RawFrame validation, sensor clipping, noise
  calibration, Bayer demosaic, RAW-plan negotiation, and embedded-preview selection.
- `tests/libraw_reference_development_contract_test.cpp` owns LibRaw settings validation,
  processed-reference capability/quality negotiation, and full-versus-preview admission without
  requiring a camera fixture.
- `tests/color_management_contract_test.cpp` owns decoded-source ICC behavior.
- `tests/raster_provider_contract_test.cpp` owns provider-neutral raster-source decoding.
- `tests/private_decoder_contract_test.cpp` owns the private plugin ABI, loading, stale-module
  rejection, and router precedence.
- `tests/neutral_detail_helper_contract.cmake` owns the real helper-process/private-provider
  development proof, including nonce-bound receipt fields and atomic RGB8 artifact publication.
- `tests/lensfun_profile_catalog_contract_test.cpp` owns database availability, automatic/manual
  identity admission, compatible-profile ordering and uniqueness, cached resolution, and explicit
  missing camera/lens statuses.
- `tests/optics_preparation_contract_test.cpp` owns manual/Lensfun pixel correction and its
  position before warm-preview and full-detail preparation.
- `tests/proxy_output_contract_test.cpp` owns encoded proxy limits and the explicit display-sRGB
  output boundary.
- `tests/edit_preview_session_contract_test.cpp` owns immutable warm-preview preparation, receipt
  retention, repeated rendering, geometry-derived radius, bounds, and preflight validation.
- `tests/edit_preview_execution_contract_test.cpp` owns output analysis, cancellation, backend
  receipts, and execution identity.
- `tests/detail_tile_contract_test.cpp` owns full-resolution tile bounds, apron scheduling,
  retained-source limits, geometry, and seam-free output.
- `tests/libraw_provider_contract_test.cpp` owns LibRaw settings, provider identity, real-fixture
  metadata, and source-development provenance.
- `tests/source_rendering_contract_test.cpp` owns the consistency of DNG baseline exposure across
  source-rendering outputs.
- `tests/raw_pipeline_routing_contract_test.cpp` owns host-versus-provider route selection,
  backend identity, explicit fallback, exact-DCP admission, and host capability negotiation.
- `tests/raw_sensor_preparation_contract_test.cpp` owns CFA-preserving denoise, calibration/cache
  identity, highlight treatment, reconstruction quality, and preview/detail source calibration.
- `tests/raw_pipeline_contract_test_support.hpp` owns only their common assertions, base Bayer
  frame, processed fallback, and synthetic decode session. Routing-only DCP fixtures and
  preparation-only noise/gradient frames live in the adjacent responsibility-named support
  headers rather than in a false-common fixture module.
- Test-only support is responsibility-named: shared assertions, processed-RGB sessions, optics
  observations, and scoped environment overrides live in separate narrow headers.

## CPU edit reference

`include/shadow/image/edit.hpp` is the compatibility and navigation entry for the edit kernel.
New production code should include the narrow semantic owner directly:

- `working_rgb.hpp` owns the in-process float raster, scene/display reference, and named working
  color-space contract.
- `photo_geometry.hpp` owns crop/orientation state, the shared integer layout, coordinate mapping,
  and geometry application.
- `edit_error.hpp` owns edit failure categories and their optional source-node location.
- `adjustment_parameters.hpp` owns the complete authored parameter registry and its stable variant
  order; `adjustment_graph.hpp` owns node identity and operation mapping.
- `edit_execution_plan.hpp` owns locality, footprints, validation, compiled segments, and their
  source-node index lifetime.
- `cpu_edit_reference.hpp` owns the deterministic flat-node oracle; `tone_curve.hpp` owns
  standalone Oklab Lightness application and exact smooth-curve sampling; `retouch.hpp` owns
  deterministic spot/continuous-brush repair; `adjustment_layers.hpp` owns masks, layer
  composition, and masked execution.
- `warm_edit_preview.hpp` owns the reusable interactive preview session, analysis, cancellation,
  and execution provenance; `full_edit_detail.hpp` owns bounded full-resolution tile sessions.
- `edited_proxy_rendering.hpp` owns one-shot adjusted proxy orchestration, while
  `proxy_rendering.hpp` remains the unedited encoded-proxy owner.

The implementation follows the same map. `src/proxy/developed_source_raster.*` owns validation,
dimensions, resizing, and bounded rectangular extraction for the decoder's two developed-source
representations. `src/proxy/jpeg_proxy_encoding.*` owns the bounded libjpeg 4:4:4 encoder shared
by reference and edited proxies. `src/proxy/proxy_render_request_validation.*` owns the shared
proxy-size/JPEG-quality boundary and RAW-plan schema/intent checks. Lifecycle-specific preparation
and rendering stay with the warm-preview, full-detail, and proxy owners rather than with these
leaf modules. `src/proxy/full_edit_detail.cpp` is the complete retained-source/tile lifecycle
owner; its memory policy, optical source preparation, apron expansion, and render methods move
together. `src/proxy/proxy_rendering.cpp` owns the ordinary one-shot reference-proxy pipeline and
the canonical aspect-preserving proxy dimension calculation.
`src/proxy/edited_proxy_rendering.cpp` owns only the one-shot adjusted-proxy entry points and
delegates the retained preview lifecycle to `WarmEditPreviewSession`.
`src/proxy/warm_edit_preview.cpp` owns that retained interactive lifecycle, including bounded
source preparation, execution provenance, cancellation, analysis, and JPEG delivery.
`src/edit/adjustment_graph.cpp` closes the complete parameter-variant to operation/id registry;
execution backends consume that graph identity instead of redefining it.
`src/edit/working_color_math.*` owns D65 working-space validation, RGB↔XYZ matrices, Oklab
conversion, and CAT16 white-balance preparation shared by CPU execution and Metal program
lowering. `src/edit/adjustment_node_diagnostics.*` preserves the common source-node diagnostic
identity used by those internal semantic owners.
`src/edit/edit_execution_validation.*` owns the float-image and execution-window admission
contract. `src/edit/tone_curve.*` owns PCHIP preparation and sampling plus Oklab Lightness and
Opponent curve execution; its prepared state exposes only the source curve, derivatives,
segment count, and neutral identity required by Metal lowering.
`src/edit/perceptual_color.*` owns hue-band and ordered Point Color mapping, global Oklab
opponent balance, Selective Color, validation, and the shared sub-stage classifier consumed by
CPU execution and Metal lowering. `src/edit/oklab_color_warper.*` separately owns lattice
validation, neutral classification, boundary feathering, interpolation, and CPU execution;
sharing Oklab does not make it part of the Perceptual Color operation.
`tests/oklab_color_warper_contract_test.cpp` mirrors that owner with row-major interpolation,
boundary feather, validation-order, backend parity, and host-lowering contracts; resident
resource-cache behavior remains with the Warm Metal tests.
Perceptual Color tests follow the same semantic index:
`tests/point_color_contract_test.cpp` owns hue bands, vibrance, feathering, and ordered ranges;
`tests/selective_color_contract_test.cpp` owns CMYK target routing and lightness protection; and
`tests/perceptual_color_contract_test.cpp` owns stage composition, classification, exact bypass,
and extended-range behavior.
`src/edit/scalar_neighborhood_filters.*` owns scalar Gaussian convolution, reflect-101
coordinates, and replicated-border guided filtering. Its prepared guided-filter aggregate keeps
dimensions, radius, mean, variance, and border policy together so detail stages cannot combine
incompatible transient fields.
`src/edit/finishing_effects_cpu.*` owns the final grain/vignette pass, including deterministic
coordinate noise, full-raster geometry, highlight protection, and the fused RGB write that makes
whole-image and tiled execution agree.
`src/edit/technical_detail_cpu.*` owns the ordered denoise, dehaze/defringe, and capture-sharpen
recovery pass. One internal plan derives its guided-filter radii, bilateral fallback, sharpen
support, and public footprint so execution cannot drift from tile planning.
`src/edit/creative_detail_grading.*` owns the shared Oklab field for Texture, Clarity, and Local
Contrast plus the following grading-wheel pass. It derives creative footprint and execution from
one plan, while Metal lowering sees only immutable prepared wheel deltas and tonal weights.
`src/edit/perceptual_contrast.*` owns the validated mapping from the public multiplicative
contrast factor and scene-linear pivot to one bounded Oklab-lightness curve. CPU execution and
Metal lowering consume the same immutable prepared contract.
`src/edit/guided_selective_tone.*` owns the fixed scene-EV zones and complete two-pass guided
filter. Its prepared plan binds the authored amounts, per-axis mask radii, and scheduler footprint
so tile planning and execution cannot describe different neighborhoods.
`tests/creative_detail_grading_contract_test.cpp` mirrors that owner with coupled-band,
grading-wheel, neutral-axis, and locality contracts; `tests/detail_effects_contract_test.cpp`
retains only the cross-pass schema and version boundary.
`src/edit/rgb_pixel_traversal.hpp` owns node-attributed row scheduling, interleaved RGB addressing,
and checked writes for pixel-local transforms; color and effect owners supply only their
algorithms.
`src/edit/metal_adjustment_program.*` owns the transient host-side Metal ABI and the portable
two-pass compiler that counts side-table resources, lowers the complete operation registry, assigns
offsets, and verifies the resulting program as one transaction. It consumes prepared semantics from
the adjustment owners rather than reinterpreting them. `src/edit/metal_adjustment_execution.hpp`
owns only backend availability and the execution attempt boundary.
`tests/metal_adjustment_program_contract_test.cpp` validates that compiler without a Metal device:
operation expansion and order, all side-table ranges, stale-plan rejection, and the resident
prevalidated-raster contract remain one test-owned transaction.
The embedded Warm Metal program is a separate language owner in
`src/proxy/warm_edit_gpu_msl.hpp`; the Objective-C++ runtime consumes it without owning its
kernel implementation. Its mirrored host records and checked buffer layout live in
`src/proxy/warm_edit_gpu_kernel_contract.hpp`.
Pure, cross-platform neighborhood-stage recognition and before/after plan rewriting live in
`src/proxy/warm_edit_gpu_render_plan.*`; one exhaustive variant records the selected route and
the Objective-C++ runtime only consumes that plan.
Process-wide Metal device, queue, runtime compilation, and the all-or-nothing pipeline registry
are owned by `src/proxy/warm_edit_gpu_pipeline_context.*`.
Session-resident source buffers, side-table caches, lazy neighborhood rasters, slot leases,
working-set admission, synchronization, and GPU statistics move together in
`src/proxy/warm_edit_gpu_resident_resources.*`; the dispatcher only receives leased buffer views.
Program lowering, stage-specific buffer selection, Metal command encoding, kernel order, status
interpretation, and readback form one execution pipeline in
`src/proxy/warm_edit_gpu_dispatcher.*`; `warm_edit_gpu.mm` is the thin session facade.
`tests/warm_edit_gpu_contract_test.cpp` is the thin runner for the corresponding real-device
contract. Its responsibility-named children mirror resident session lifecycle, technical and
creative detail dispatch, and resident side-table caches; their only shared fixture owns
CPU-oracle parity inputs and comparisons.

The edit path accepts explicitly native interleaved RGB float32, scene-referred, linear-light data
with named RGB primaries, white point, and luminance coefficients. It is not legal to feed the
decoder's integer `PixelBuffer` directly into this path: the proxy boundary validates its explicit
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

Edit-kernel contract tests follow the same ownership boundaries as the implementation:

- `tests/adjustment_execution_contract_test.cpp` is the thin runner for backend dispatch,
  admission, resource failure, real-Metal parity, operation-order, concurrency, and optional
  benchmark contracts. Responsibility-named children own those cases; narrow fixtures separately
  own deterministic parity images, advanced LUT data, and perceptual-color parameters.
- `tests/edit_execution_plan_contract_test.cpp` owns operation identity, locality, footprints,
  validation-before-elision, stable plan identity, and maximal execution-plan segmentation.
- `tests/core_adjustment_execution_contract_test.cpp` owns the basic CPU pixel semantics,
  observable node order, and disabled-node behavior.
- `tests/edit_input_validation_contract_test.cpp` owns fail-closed parameter/version handling,
  bounded validation without pixels, color encoding, working-space, and raster-layout admission.
- `tests/perceptual_contrast_contract_test.cpp` owns the perceptual pivot, factor mapping,
  identity/collapse endpoints, and shared CPU/Metal lowering contract.
- `tests/guided_selective_tone_contract_test.cpp` owns fixed EV zones, two-pass radius/footprint
  binding, smooth monotonic response, chroma preservation, and edge-aware behavior.
- `tests/tone_curve_contract_test.cpp` owns smooth-curve interpolation, endpoint extrapolation,
  Oklab-lightness execution, and its graph-node contract.
- `tests/scalar_neighborhood_filters_contract_test.cpp` owns reflect-101 Gaussian and
  replicated-border guided-filter invariants.
- `tests/finishing_effects_contract_test.cpp` owns grain determinism, vignette geometry,
  neutral bypass, and full-image/tile coordinate equivalence.
- `tests/technical_detail_contract_test.cpp` owns sharpen, denoise, and independent defringe
  behavior.
- `tests/detail_effects_contract_test.cpp` owns the cross-pass schema/version contract and
  creative color-grading behavior.
- `tests/lut_execution_contract_test.cpp` owns Cube LUT bypass, interpolation, blending, and
  extreme-scene execution; `tests/lut_contract_test.cpp` remains the resource parser owner.
- `tests/spatial_edit_contract_test.cpp` owns local masks, repair/clone strokes, and photo
  geometry.
- `tests/perceptual_color_contract_test.cpp` owns point color, selective color, perceptual hue
  routing, vibrance, and extended-gamut behavior.
- `tests/edit_contract_test_support.hpp` contains only the shared assertions and small image
  fixtures used by these executables.

Oklab Lightness Tone Curve is available through the standalone
`apply_oklab_lightness_tone_curve` reference operator and as a normal ordered executor node.
Version 1 uses 2 through 256 finite control points whose strictly increasing x coordinates span
exactly 0 through 1. The monotone PCHIP evaluator changes only Oklab L, extrapolates with endpoint
tangents, and never gamut-clips. Oklab Opponent curves reuse that evaluator as bounded a/b offset
curves keyed by clamped photographic lightness.

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
