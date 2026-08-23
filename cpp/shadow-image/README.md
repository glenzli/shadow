# shadow-image

`shadow-image` is Shadow's platform-neutral C++20 image kernel. Its first implemented provider
uses LibRaw behind provider-neutral public contracts.

Changes to an interactive render path must also follow the repository-wide
[interactive editing pipeline contract](../../docs/development/interactive-editing-pipeline.md):
it defines minimal invalidation, GPU continuity, cancellation, and preview/detail/export
equivalence requirements without duplicating this source-owned implementation map.

The line-based manifests under
[`cmake/source-manifests/`](cmake/source-manifests/) are the single compiled-source index shared
by CMake and direct Cargo builds. Portable, Metal, and non-Metal fallback
translation units are selected there; adding Windows acceleration must add a distinct manifest
and backend owner rather than duplicate the portable kernel or fork this library.

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
  development receipt. `src/decoder/dng_noise_profile.*` owns bounded, random-access extraction of
  the exact DNG `NoiseProfile` from one unambiguous primary Bayer Raw IFD and its conversion from
  normalized DNG coefficients to per-site DN units. `libraw_decoder.cpp` retains source metadata,
  embedded previews, and the provider-neutral RawFrame session, then delegates processed reference
  work to that owner.
- `raw_development_plan.hpp` owns requested RAW intent and capability negotiation, while
  `raw_development_receipt.hpp` owns the auditable execution result.
- `raw_white_balance.hpp` / `src/raw/raw_white_balance.cpp` own the calibrated
  photographer-facing temperature/tint contract, CIE white-point conversion,
  source CameraNeutral interpretation, and DCP/provider-matrix projection.
  Camera-channel ratios remain internal renderer values.
- `decoder_metadata.hpp` / `src/decoder/decoder_metadata.cpp` own source facts,
  embedded-preview descriptors, provider-ID selection, and format names.
  `focus_observation.hpp` / `src/decoder/focus_observation.cpp` normalize
  bounded camera-authored AF locations into the display-oriented uncropped
  source space. Nikon AFInfo2 V0400-family areas and Sony FocusLocation points
  retain distinct provenance and never become a sharpness claim.
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
full-detail session lifecycles. `include/shadow/image/cxx_preview_frame.hpp` and
`src/bridge/cxx_preview_frame.cpp` own the borrowed-slice ABI for one move-only interactive frame.
`src/bridge/adjustment_render_wire.cpp` owns Recipe node projection.
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
- An embedded DNG `NoiseProfile` is admitted only from the unique highest-quality CFA Raw IFD.
  Shared or three-plane RGB coefficients are accepted in `CFAPlaneColor` order; BigTIFF, ambiguous
  Raw IFDs, malformed/non-finite coefficients, unsupported plane shapes, linearization tables, and
  spatial black-level deltas leave sensor calibration unavailable. `BaselineNoise` is never
  relabelled as exact camera calibration.
- `develop_source_reference` is the application source boundary. Supported public LibRaw and
  private-provider files both expose `RawFrame` and enter Shadow's owned black subtraction,
  normalization, Bayer reconstruction, white balance and camera-to-linear-sRGB path. An exact
  local DCP may replace the provider's generic matrix; profiles carrying unsupported creative
  tables are rejected as a whole. `render_reference_rgb` remains only the explicit
  provider-processed compatibility route. Every choice and fallback is recorded in the pipeline
  receipt and cache identity. `src/raw/raw_frame_development_plan.*` owns source validation, the
  immutable camera transform and optional DCP lifetime, source-wide luminance calibration,
  preview/diagnostic geometry, conventional CFA-denoise intent, and RAW receipt finalization.
  Local RAW development never loads an AI model; Infer Runtime owns RawNIND execution and publishes
  its verified camera-RGB foundation through the distinct ingestion boundary below.
  `include/shadow/image/raw_foundation.hpp` and `src/raw/raw_foundation.cpp` own the distinct
  post-model ingestion boundary for externally materialized AI foundations. They accept only a
  verified, path-free public RawNIND identity and finite interleaved linear Camera RGB, bind its
  zero-or-one canonical-RGGB crop to the source active sensor, then apply the existing
  source-bound camera transform and orientation into scene-linear working RGB. Bounded preview
  resampling happens before that linear transform. `src/raw/raw_foundation_source.*` is the sole
  source-route integration owner: it reuses the original RawFrame for calibration, DCP rendering,
  luminance, sensor clipping, and the original camera-RGB reconstruction. Strength below 100%
  linearly blends that reconstruction with the cached full-strength AI camera RGB before DCP and
  working-space conversion; changing strength therefore changes developed-render identity without
  rematerializing or re-identifying the AI artifact. Its receipt includes requested strength,
  while the reusable foundation identity includes only the exact model, implementation, source,
  artifact, and cache-key identities. Geometry, provenance, or provider-policy mismatch fails without a provider-RGB or
  original-RAW fallback. The requested RAW plan remains auditable, while its effective AI
  execution disables overlapping conventional RAW denoise and highlight reconstruction. The
  explicit overloads in `warm_edit_preview.*` and
  `full_edit_detail_source_preparation.*` then delegate that result through the existing optics,
  source-rendering, Recipe, and display owners. Warm preview stays bounded; detail/export retains
  the complete scene-linear foundation and cannot enter the resident-CFA route.
  `src/raw/raw_preview_rebinding.*` owns the interactive exception to otherwise fixed source
  development: ordinary RAW keeps one already-neural/conventionally-denoised sensor frame, while
  a bounded AI preview keeps its original and full-strength AI oriented Camera RGB blend bases.
  A strength-only request rebinds those bases, and a temperature/tint-only request
  recompiles the generic camera transform or exact DCP and publishes a new immutable warm
  session, receipt, optics result, and GPU edit source without reopening the decoder, rereading the
  foundation artifact, or repeating denoise. Full-detail and export deliberately retain one mixed
  full-resolution raster instead of doubling their hundreds-of-MiB source memory. Any quality,
  opcode, denoise, highlight, source, foundation, or optics change fails
  this narrow reuse contract and returns to normal source preparation.
  `src/raw/raw_frame_source_preparation.*` owns the one-time session decode, plan negotiation,
  exact-DCP admission, final RawFrame pipeline receipt, and unforgeable source identity shared by
  materialized and resident consumers. Only that owner may prepare region optics for publication,
  so independently prepared or cross-source camera/optics state is rejected before CFA work or
  device upload. The default source treatment applies selected CFA gains (normalized by their
  minimum) before demosaic, but retains every sub-white sensor sample as fp32 even if white balance
  carries it above one. Each reconstruction footprint also retains exact per-colour physical-white
  coverage. Before the camera matrix, a one-sided opposed-channel step may lift only a weak channel
  supported by exact clipping or the final narrow linear-response shoulder; it never lowers a
  measured channel. Shared three-colour physical
  clipping then continuously removes only the unmeasured residual chroma, closing the equal-evidence
  Bayer-phase hole while leaving a one-colour emitter saturated. This local fp32 work is fused into
  the existing CPU/Metal sampling pass and creates no full-frame side buffer or device transfer.
  The source also projects an immutable, display-sized R8 CFA-chroma-risk sidecar from the same
  calibrated linear-response limits. A single near-white channel remains measured colour evidence;
  risk rises when independently sampled channels lose headroom and their evidence diverges. A
  terminal shared-clipping component feathers its confidence one display bin into adjacent valid
  bins; it copies no neighbouring hue, detail, or luminance. Bounded warm preview retains this
  sidecar beside its reusable source. The provider-default RAW policy also replaces physically
  clipped topology with a low-frequency scene-linear surface reconstructed from measured
  neighbours. A push-pull guide is bounded to 384 pixels on its longest edge; the full raster
  receives one in-place linear pass, and a brightness gate keeps its small exterior chroma shoulder
  from crossing an adjacent dark subject. Reconstruction retains the clipped core within one
  quarter stop of its measured luminance and follows the reliable boundary chromaticity with only
  a small neutral safety pull. It therefore prevents a warm clipped surface from becoming a dark
  grey island while still synthesizing only luminance and colour trend, never texture.
  Detail/export materializes this source once instead of inferring a different surface independently
  in resident tiles. DCP input rendering follows the reconstruction, so the spatial estimate
  remains a RAW source operation. The warm preview then binds the prepared source and one R8
  chroma-risk plane once to the resident GPU edit session (or the matching CPU fallback); slider
  events add no source work, neighbourhood pass, or extra evidence plane. During actual negative highlight/white
  recovery, Selective Tone progressively pulls chroma toward neutral in
  proportion to that source risk while preserving its ordinary Oklab-lightness behavior elsewhere.
  It never reconstructs spatial detail or invents a neighbouring hue. `disabled` stays an explicit
  unbounded diagnostic plan. The historical `aggressive` value remains a compatible diagnostic
  alias that adds its earlier CFA-scale spatial chroma feather before the same default surface
  reconstruction; no desktop authoring switch selects it. The effective policy and reconstruction
  identity remain part of the developed cache identity. CPU and Metal apply the same CFA-scale
  route before their camera transforms.
  `src/raw/raw_frame_source_development.*` consumes that preparation for the complete
  CPU/Metal materialization transaction. `src/raw/raw_frame_region_development.*` owns the exact
  CPU region contract: oriented output cores, active-sensor reconstruction coordinates,
  stored-sensor demosaic/denoise preimages, halos, CFA phase, and byte-identical full-versus-region
  reconstruction. `src/raw/resident_raw_source.*` owns the aggregate CPU/device detail lifecycle,
  keeping one CFA plus immutable camera/DCP/optics/receipt state. Forced CPU materializes only
  requested RGB regions; automatic/Metal delegates one exact source preimage at a time to
  `src/raw/metal_resident_raw_source.*`, retaining sensor/DCP buffers across requests without a
  complete fp32 readback. `src/raw/raw_pipeline.cpp` retains top-level route selection and provider
  compatibility fallback.
- `render_reference_proxy_jpeg` bounds the longest edge (2048, quality 95, and 4:4:4 chroma in the current recipe) and rejects unbounded requests. Its version belongs in the cache key.
- `decode_jpeg_display_luma` is a separate analysis path over compressed display proxies. It requires 8-bit libjpeg-turbo with in-memory sources, rejects encoded inputs above 128 MiB and source headers above 65,535 per axis or 100 million pixels, applies a stricter 50-million-pixel limit to multi-scan inputs, caps libjpeg memory at 256 MiB, and bounds scaled intermediates before emitting a tightly packed normalized `float` luma plane with a caller-selected edge in 1 through 512. Corrupt-data warnings, including synthesized end-of-image recovery for truncation, fail closed.
- A `DecodeSession` is thread-confined. Providers may be shared; parallel work should open independent sessions.
- `RawFrame` intentionally copies LibRaw memory and preserves raw-coordinate samples, active
  margins (preferring LibRaw's standard RAW inset over legacy rendered-image margins), CFA layout,
  per-CFA black/white calibration, as-shot neutral, an optional explicit
  Camera RGB -> XYZ D50 matrix, the optional physical XYZ D65 -> Camera RGB calibration used to
  derive manual temperature/tint neutrals, optional exact embedded sensor-noise calibration, and pending DNG
  opcode declarations. It is explicitly
  pre-demosaic; unsupported CFA layouts remain inspectable but cannot enter Bayer-only
  processing. A later opaque/tiled buffer can remove this copy without changing the frame
  semantics.
- `src/raw/raw_frame_staging.cpp` owns the short-lived AI sidecar projection of that same
  provider-neutral frame: the active Bayer rectangle is written as little-endian uint16 samples,
  with CFA, black/white levels, both colour-calibration contracts, and decoder identity in a
  bounded manifest. The sample file is
  published before the manifest and both stay outside the source tree.
- `auto_geometry.hpp` and `src/analysis/auto_geometry.cpp` own bounded, deterministic geometry
  analysis over a borrowed display-sRGB RGB8 preview. They return non-authoritative straighten and
  keystone proposals with confidence and line evidence; the analyzer never mutates or persists a
  Recipe. Authored crop, orientation, perspective, and rendering remain owned by `photo_geometry`.
- Native-size Bayer reconstruction, the precompiled camera transform and orientation are fused
  into one output pass. The CPU path remains the exact reference. On macOS, Metal v1 performs the
  balanced bilinear and high-quality directional-green/colour-difference contracts in fp32 and
  bounded output tiles, while its area-preview kernel performs CFA-aware sensor-footprint
  integration for bounded catalog/edit sources. The actual
  `shadow-fused-raw-cpu-v1` or
  `shadow-fused-raw-metal-full-v1` identity is cache-visible. Metal failure in automatic mode
  falls back to CPU inside the RawFrame route and can never silently select provider-processed
  RGB.

Runtime controls:

```text
SHADOW_RAW_PIPELINE=auto|raw-frame|processed
SHADOW_IMAGE_ACCELERATION=auto|cpu|metal
```

`metal` requires Metal for every eligible Bayer detail or area-preview request. Build-time
`SHADOW_ENABLE_METAL=OFF` compiles the same public API against a cross-platform stub.

RawNIND runs only through Infer Runtime. `shadow-image` accepts its verified camera-RGB foundation;
it never loads, validates, or gates a local model artifact.

DNG technology notice: This product includes DNG technology under license by Adobe.

The Metal implementation also follows the language boundary.
`src/raw/metal_raw_development_msl.hpp` is the thin one-library composition index:
`metal_raw_common_msl.hpp` owns the shared ABI, Bayer sampling, source-clipping projection, and
the same editable CFA scale, per-colour physical-white topology, one-sided opposed repair, and
residual shared-chroma contract mirrored by the CPU region developer; WB-induced fp32 headroom
reaches the resident GPU edit source without reconstructing missing spatial detail;
`metal_raw_denoise_msl.hpp` owns same-CFA sensor denoise;
`metal_raw_reconstruction_msl.hpp` owns balanced/high-quality detail and CFA-area previews; and
`metal_dcp_color_msl.hpp` owns DCP post-processing. Host execution is split by transaction:
`src/raw/metal_raw_runtime.*` owns the process-wide device, command queue, compiled pipelines,
bounded arithmetic, and diagnostics; `raw_denoise_plan.*` owns cache-visible denoise intent,
calibration, and receipts, while `raw_denoise.cpp` owns standalone CPU/Metal fallback execution;
`metal_raw_denoise_encoding.*` owns the mirrored denoise ABI plus its GPU source-copy/dispatch,
while `metal_raw_denoise.mm` owns only the standalone materialized-frame transaction;
`metal_raw_reconstruction.mm` owns tiled reconstruction and its optional resident denoise and
same-command DCP continuations plus exact pre-denoise sensor-clipping projection;
`metal_dcp_color_encoding.*` owns the compact mirrored DCP ABI, table buffers, and reusable
encoder; `metal_dcp_color_rendering.mm` owns the standalone fallback-facing whole-frame execution.
An eligible RawFrame is therefore uploaded once, keeps its denoised CFA resident through
reconstruction, derives the display-oriented zebra diagnostic from the original sensor buffer,
and applies each tile's camera rendering before its only host readback.
`metal_raw_development.hpp` remains the narrow fallback-facing contract. Editing a host executor
requires checking its local layout assertions and corresponding shader entry-point; editing the
runtime requires checking all four entry-point names.

DCP color development has a one-way internal owner graph.
`src/raw/dcp_color_matrix_math.hpp` owns the shared 3×3 algebra, standard white points, and
Bradford adaptation. `src/raw/dcp_color_rendering.*` owns HueSatMap/LookTable/tone-curve
preparation plus CPU post-matrix execution and Metal backend selection; the reusable Metal
encoding owner above serves both standalone and fused execution. `src/raw/dcp_color_development.cpp`
retains single/dual-illuminant calibration, matrix-route selection, immutable transform
composition, and receipt identity; `src/raw/raw_white_balance.cpp` owns camera-neutral
interpretation and the reversible temperature/tint presentation used by that calibration.

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

For bounded RAW diagnostics, `shadow-raw-probe --raw-frame-only` stops after provider-neutral
sensor extraction and reports the exact resolved noise model. `--neural-raw-only` additionally
executes the configured RAW-to-RAW neural node and reports its model/runtime identity plus numeric
sample deltas, but deliberately does not claim that later declared DNG opcodes or final rendering
have executed.

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

`shadow-image-decode-helper raw-frame-staging` is a separate production input boundary for local
AI sidecars. It opens the original through the same private-provider router, publishes one
nonce-bound provider-neutral Bayer staging pair with its complete active-frame colour/orientation
descriptor, and never asks the AI provider to re-decode a proprietary RAW container. The same
request-private pair is the canonical handoff into AI preview/detail preparation: native rendering
strictly reconstructs the provider-neutral `RawFrame`, then releases the files after it owns the
bounded preview basis or full detail source. Follow `raw_frame_staging.*` for this read/write
contract and `raw_frame_source_preparation.*` for its development binding.

Lensfun optics has responsibility-named production owners behind the stable `OpticsProvider` API.
`src/optics/lensfun_profile_catalog.*` owns database selection and loading, normalized camera
identity lookup, compatible-lens projection, explicit/manual profile resolution, synchronization,
and the match cache. `src/optics/manual_optics.*` owns settings validation plus
provider-independent manual distortion, transverse chromatic aberration, vignetting, and CPU
fallback execution. `src/optics/metal_manual_optics.*` owns the bounded scene-linear fp32 Metal
executor; automatic mode uses it for full-resolution manual optics while explicit CPU mode retains
the f64 coordinate oracle. `src/optics/lensfun_optics.cpp` consumes catalog matches, preserves provider fallback semantics, and selects the stable correction receipt/plan boundary.
`src/optics/lensfun_modifier_plan.*` clones resolved Lensfun camera/lens state into provider-independent immutable plan ownership.
`src/optics/lensfun_region_plan.cpp` compiles absolute RGB inverse maps, exact bilinear source preimages, and source-aligned profile-vignette gains; `src/optics/lensfun_cpu_reference.cpp` owns full-frame and regional CPU oracle execution.
`src/optics/scene_linear_region_optics.*` classifies the prepared plan as neutral, owned pointwise, coordinate-remapping, or materialization-only; CPU resident RAW currently admits only neutral or owned pointwise plans and fails closed for the other classes.
`src/acceleration/image_acceleration_policy.*` is the single parser for
`SHADOW_IMAGE_ACCELERATION`; RAW, edit, display, and optics boundaries project its neutral choice
into their own typed backend errors.

Decoder contract tests follow the production responsibilities instead of one aggregate executable:

- `tests/raw_frame_contract_test.cpp` owns `RawFrame` storage/identity validation and explicit
  sensor-noise calibration.
- `tests/sensor_clipping_contract_test.cpp` owns sensor-domain highlight/shadow projection,
  orientation, and exact downsample reduction.
- `tests/bayer_demosaic_contract_test.cpp` owns normalized Bayer reconstruction, receipts, and
  unsupported-layout rejection.
- `tests/raw_foundation_contract_test.cpp` owns verified RawNIND provenance, source-crop
  admission, bounded camera-RGB preview resampling, original/AI strength blending before the
  camera transform, orientation, and fail-closed invalid-input behavior.
- `tests/raw_foundation_source_route_contract_test.cpp` owns explicit source routing, adjusted-plan
  audit, artifact-sensitive cache identity, clipping diagnostics, and the no-fallback boundary.
- `tests/raw_foundation_edit_surfaces_contract_test.cpp` owns bounded warm-preview and materialized
  full-detail publication, real RGB8/tile rendering, and surface-level no-fallback behavior.
- `tests/raw_preview_rebinding_contract_test.cpp` owns one-decode RAW/AI camera-space reuse,
  independent old/new white-balance receipts, changed output pixels, and sensor-stage rejection.
- `tests/raw_development_plan_contract_test.cpp` owns default intents, cache identity, provider
  capability negotiation, schema rejection, and the explicit absence of RAW provenance.
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
- `tests/optics_region_contract_test.cpp` owns optics-locality classification, unknown-provider
  fail-closed behavior, and byte-identical pointwise manual-vignette regions versus full CPU
  correction in global coordinates.
- `tests/manual_optics_metal_execution_contract_test.cpp` owns real-device CPU/Metal parity,
  forced tiled execution, determinism, and the opt-in
  `SHADOW_TEST_MANUAL_OPTICS_METAL_BENCHMARK` timing.
- `tests/proxy_output_contract_test.cpp` owns encoded proxy limits and the explicit display-sRGB
  output boundary.
- `tests/edit_preview_session_contract_test.cpp` owns immutable warm-preview preparation, receipt
  retention, repeated rendering, geometry-derived radius, bounds, and preflight validation.
- `tests/edit_preview_frame_contract_test.cpp` owns moved RGB8/R8 allocation retention, stable
  addresses, and fail-closed interactive descriptor pairing.
- `tests/edit_preview_execution_contract_test.cpp` owns output analysis, cancellation, backend
  receipts, and execution identity.
- `tests/edit_preview_layer_execution_contract_test.cpp` owns fused resident layer receipts,
  continuous-brush routing, atomic CPU replay, and forced-backend failure semantics.
- `tests/detail_tile_session_contract_test.cpp` owns one-time source preparation, retained-source
  immutability, exact crop coordinates, render-local edit isolation, resident Metal tile reuse,
  and whole-tile CPU fallback receipts.
- `tests/detail_tile_display_output_contract_test.cpp` owns processed-linear admission, padded
  rows, scene-to-display rolloff, shared-channel dithering, and bounded Oklab gamut mapping.
- `tests/detail_tile_seam_contract_test.cpp` owns full-versus-irregular tile equivalence for
  pixel-local, accumulated-neighborhood, and guided selective-tone execution.
- `tests/detail_tile_layer_seam_contract_test.cpp` owns full-versus-irregular tile equivalence and
  effective resident-Metal routing for opacity, linear/radial/continuous-brush masks, and creative
  detail.
- `tests/detail_tile_validation_contract_test.cpp` owns apron/allocation limits, rectangle and plan
  rejection order, overflow safety, and metadata preflight before pixel I/O.
- `tests/detail_tile_contract_test_support.hpp` owns only their synthetic decode session, source
  fixtures, neutral plan, and typed decode-error assertion.
- `tests/libraw_provider_contract_test.cpp` owns LibRaw settings, provider identity, real-fixture
  metadata, and source-development provenance.
- `tests/source_rendering_contract_test.cpp` owns the consistency of DNG baseline exposure across
  source-rendering outputs.
- `tests/raw_pipeline_routing_contract_test.cpp` owns host-versus-provider route selection,
  backend identity, explicit fallback, exact-DCP admission, and host capability negotiation.
- `tests/resident_raw_source_contract_test.cpp` owns one-decode/one-CFA resident reuse,
  full-versus-region equivalence across orientation, active margins, CFA phase, demosaic/denoise
  halos, exact DCP receipts, source-bound optics rejection, and forced-CPU materialization fallback
  for unknown optics providers.
- `tests/metal_resident_raw_source_contract_test.cpp` owns provider-neutral Canon/Sony/Nikon/private
  RawFrame admission, source-owner isolation, repeated/concurrent regions, byte/device admission,
  one shared retained-byte allowance, terminal post-publication failure, and the zero full-frame
  readback contract.
- `tests/metal_scene_linear_region_optics_contract_test.cpp` owns real-device C-b→C-c evidence
  binding, DCP/denoise/Lensfun parity, same-size cross-lens rejection, completion lifetime, and zero
  nominal source re-upload/fp32 readback.
- `tests/full_edit_detail_metal_raw_contract_test.cpp` owns the native
  RAW→optics→source-render→warm-edit tile transaction, same-viewport reuse, CPU/display precision,
  zero intermediate readback, and the opt-in `SHADOW_BENCH_FULL_EDIT_DETAIL_METAL_RAW` 4096×3072
  timing report.
- `tests/raw_sensor_preparation_contract_test.cpp` owns CFA-preserving denoise, calibration/cache
  identity, highlight treatment, reconstruction quality, and preview/detail source calibration.
- `tests/fused_raw_cpu_development_contract_test.cpp` owns fused CPU orientation, preview
  footprint, active-sensor bounds, and high-quality reconstruction against the two-stage oracle.
- `tests/fused_raw_metal_execution_contract_test.cpp` owns Metal determinism and numerical
  agreement for balanced, high-quality, and CFA-area preview execution, byte-identical
  staged-versus-fused DCP tiles, byte-identical staged-versus-resident CFA denoise, exact CPU/Metal
  clipping projection across orientations and scales, and the opt-in
  `SHADOW_TEST_EDGE_AWARE_METAL_BENCHMARK`, `SHADOW_TEST_FUSED_RAW_DCP_BENCHMARK`,
  `SHADOW_TEST_FUSED_RAW_SENSOR_BENCHMARK`, and
  `SHADOW_TEST_FUSED_SENSOR_CLIPPING_BENCHMARK` timings.
- `tests/fused_raw_highlight_treatment_contract_test.cpp` owns measured clipped-highlight source
  preservation, explicit policy provenance, saturated-colour preservation, and CPU/Metal parity.
- `tests/fused_raw_input_validation_contract_test.cpp` owns typed rejection of unsupported
  orientation, transforms, highlight modes, and degenerate Bayer storage.
- `tests/fused_raw_contract_test_support.hpp` owns only the synthetic RAW frame shared by those
  four contracts; required-Metal gates, reference oracles, and highlight fixtures stay with their
  semantic owners.
- `tests/raw_pipeline_contract_test_support.hpp` owns only their common assertions, base Bayer
  frame, processed fallback, and synthetic decode session. Routing-only DCP fixtures and
  preparation-only noise/gradient frames live in the adjacent responsibility-named support
  headers rather than in a false-common fixture module.
- `tests/camera_profile_catalog_contract_test.cpp` owns content-addressed profile discovery,
  exact camera matching, duplicate admission, and optional public-profile parsing.
- `tests/dcp_color_transform_contract_test.cpp` owns forward/inverse matrix route selection,
  exposure headroom, chromatic adaptation, and dual-illuminant interpolation.
- `tests/dcp_color_rendering_contract_test.cpp` owns post-matrix HueSatMap, LookTable, tone-curve,
  parallel-frame, and CPU/Metal execution behavior. Its shared transform inputs live in
  `tests/dcp_color_contract_test_support.hpp`; catalog file fixtures remain with the catalog.
- Test-only support is responsibility-named: shared assertions, processed-RGB sessions, optics
  observations, and scoped environment overrides live in separate narrow headers.

## CPU edit reference

`include/shadow/image/edit.hpp` is the compatibility and navigation entry for the edit kernel.
New production code should include the narrow semantic owner directly:

- `working_rgb.hpp` owns the in-process float raster, scene/display reference, and named working
  color-space contract.
- `photo_geometry.hpp` owns crop/orientation/straighten/perspective state, the shared integer
  layout, coordinate mapping, and geometry application. Perspective uses one bounded exact
  homography from the final rectangle into real source pixels, preserving lines without an
  output-stage desaturation, fill, or fabricated corner policy. `src/edit/photo_geometry_sampling.hpp`
  is the narrow internal inverse mapping shared by RGB geometry and scalar selection coverage.
- `photo_liquify.hpp` / `src/edit/photo_liquify.cpp` own validated ordered Push/Reconstruct
  preparation and inverse coordinate-field replay. Reconstruct attenuates earlier deformation
  toward identity rather than synthesizing a reverse push; these owners do not own Canvas order,
  tiling, or backend selection.
- `photo_structural_rendering.hpp` / `src/edit/photo_structural_rendering.cpp` own the fixed
  Liquify-to-Canvas CPU structural order, conservative tile preimages, and the fused single-sample
  execution used by warm preview and full-detail rendering.
- `edit_error.hpp` owns edit failure categories and their optional source-node location.
- `adjustment_parameters.hpp` owns the complete authored parameter registry and its stable variant
  order; `adjustment_graph.hpp` owns node identity and operation mapping.
- `edit_execution_plan.hpp` owns locality, footprints, validation, compiled segments, and their
  source-node index lifetime.
- `cpu_edit_reference.hpp` owns the deterministic flat-node oracle; `tone_curve.hpp` owns
  standalone Oklab Lightness application and exact smooth-curve sampling; `retouch.hpp` owns
  spot/continuous-brush geometry, coverage and donor selection;
  `src/edit/retouch_source_transform.*` owns the shared affine donor mapping, whole-patch edge
  clamping, and exact full-detail source reach for rotation, scale, and mirroring, while
  `src/edit/retouch_heal_blending.*` owns Heal's robust local-illumination boundary fit and
  scale-aware bounded screened gradient-domain texture blend; `adjustment_layers.hpp` owns masks,
  layer composition, and masked execution.
- `warm_edit_preview.hpp` owns the reusable interactive preview session, analysis, cancellation,
  execution provenance, transient display-sRGB RGB8 rendering, and settled JPEG output;
  `edit_preview_frame.hpp` owns the immutable moved RGB8/R8 presentation frame and paired mask
  coverage descriptor; `src/proxy/warm_edit_gpu_presentation_surface.*` owns the Apple Metal
  buffer-backed RGBA8-sRGB presentation texture and its explicit packed-RGB fallback;
  `full_edit_detail.hpp` owns bounded full-resolution tile sessions.
- `edited_proxy_rendering.hpp` owns one-shot adjusted proxy orchestration, while
  `proxy_rendering.hpp` remains the unedited encoded-proxy owner.

The implementation follows the same map. `src/proxy/developed_source_raster.*` owns validation,
dimensions, resizing, and bounded rectangular extraction for the decoder's two developed-source
representations. `src/proxy/jpeg_proxy_encoding.*` owns the bounded libjpeg 4:4:4 encoder shared
by reference and edited proxies. `src/proxy/proxy_render_request_validation.*` owns the shared
proxy-size/JPEG-quality boundary and RAW-plan schema/intent checks. Lifecycle-specific preparation
and rendering stay with the warm-preview, full-detail, and proxy owners rather than with these
leaf modules. `src/proxy/full_edit_detail_source_preparation.*` owns representation-specific
metadata admission, owner-bound optical preparation, provider compatibility, and the complete
forced-CPU versus automatic/forced-Metal source-routing transaction. Its result is a tagged
materialized-or-resident owner with no null/dual state. Eligible forced-CPU RawFrame input retains
`ResidentRawSource`; eligible automatic/Metal input publishes the same aggregate as a
device-resident source. Pre-publication automatic failures may return the intact prepared owner to
the existing materializer, while forced Metal fails closed. Unknown/non-resident optics on forced
CPU continue through complete CPU materialization. `src/proxy/full_edit_detail.cpp` owns the
resulting session, apron/geometry composition, CPU tile execution, and terminal resident-device
failure: after publication it never silently substitutes CPU pixels or provenance. Materialized
paths preserve the public `DevelopedSourcePixels` contract.
`src/proxy/full_edit_detail_metal_source.*` owns the native
CFA→DCP/demosaic→region-optics→source-rendering transaction and adopts its same-device fp32 tile into
warm editing without a RAW re-upload, fp32 readback, or full-frame fp32 allocation.
`src/proxy/full_edit_detail_gpu_cache.cpp` owns the bounded materialized-source LRU;
`src/proxy/full_edit_detail_gpu_cache_resident.cpp` owns one serialized resident viewport session,
combined device-budget accounting, exact core readback, and reuse. Neither owns source geometry,
fallback semantics, or durable cache identity. `src/proxy/proxy_rendering.cpp` owns the ordinary
one-shot reference-proxy pipeline and the canonical aspect-preserving proxy dimension calculation.
`src/proxy/edited_proxy_rendering.cpp` owns only the one-shot adjusted-proxy entry points and
delegates the retained preview lifecycle to `WarmEditPreviewSession`.
`src/proxy/edit_preview_rendering.*` owns stateless flat-node/layer execution, CPU/Metal fallback
receipts, display projection, and histogram/clipping/HDR analysis.
`src/proxy/edit_preview_frame.cpp` validates and retains one completed packed RGB8 allocation plus
its optional generation-paired R8 coverage without copying either vector.
`src/proxy/warm_edit_preview.cpp` owns the retained interactive lifecycle: bounded source and
optics preparation, resident GPU session creation, cancellation result orchestration, transient
RGB8 versus settled analysis/JPEG output policy, and source/execution receipt delivery.
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
`src/edit/local_mask_validation.*` owns the shared layer, mask, node, and full-image-coordinate
admission contract. CPU layer execution and resident Metal lowering both call it before bypassing
disabled or neutral content, so malformed persisted recipes cannot acquire backend-dependent
validation.
`src/edit/local_mask_coverage.*` owns CPU mask dispatch, pre-adjustment-input capture, continuous
brush capsules, condition-mask color conversion, and scalar geometry/R8 projection;
`src/edit/managed_raster_mask.*` separately owns the bounded portable Gray8/Gray16Float contract,
validation, binary16 decoding, and pixel-center bilinear sampling for application-managed masks.
`src/edit/local_mask.cpp` consumes that evaluator for layer blending; a selected active layer
reuses its captured float raster rather than evaluating color or luminance conditions twice.
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
`src/proxy/warm_edit_gpu_msl.hpp`. Local-mask evaluation, R8 capture, and layer blending are one
cohesive DSL fragment in `src/proxy/warm_edit_gpu_mask_msl.hpp`; post-edit crop/orientation
sampling is isolated further in `src/proxy/warm_edit_gpu_geometry_msl.hpp`. The Objective-C++
runtime consumes these fragments without owning their kernel implementations. Their mirrored host
records and checked buffer layouts live in `src/proxy/warm_edit_gpu_kernel_contract.hpp`.
Pure, cross-platform lowering of one neighborhood operation into immutable kernel parameters lives
in `src/proxy/warm_edit_gpu_neighbourhood_plan.*`. `src/proxy/warm_edit_gpu_render_plan.*` is the
smaller composition owner: it preserves every pixel-local gap while collecting any number of
supported neighborhood operations into one ordered resident transaction. Bounded dynamic Gaussian
loops admit native-resolution Texture and Clarity (including Clarity's 36-pixel large band).
Local Contrast uses row/column sliding-window box filters, so its native 20-through-80-pixel
support remains linear in raster size instead of multiplying per-pixel work by radius. Texture,
Clarity, and Local Contrast prepare their source-lightness bands independently, then join one
creative-detail kernel that preserves the CPU reference's ordered Oklab-lightness composition and
single RGB conversion. Unusually enlarged rasters still fail closed to the CPU reference.
Process-wide Metal device, queue, runtime compilation, and the all-or-nothing pipeline registry
are owned by `src/proxy/warm_edit_gpu_pipeline_context.*`.
Session-resident source buffers, side-table caches, lazy neighborhood rasters, slot leases,
working-set admission, synchronization, and GPU statistics move together in
`src/proxy/warm_edit_gpu_resident_resources.*`; the dispatcher only receives leased buffer views.
`src/proxy/warm_edit_gpu_transaction.*` lowers one complete ordered edit plan, retains its
side-table leases, and owns the shared operation-buffer offsets. Its paired
`warm_edit_gpu_transaction_encoder.*` binds and encodes that prepared plan without submitting or
reading back a command, so ordinary renders and sequential masked layers can share one execution
contract.
`src/proxy/warm_edit_gpu_brush_index.*` converts each authored stroke into continuous segment
capsules (retaining point capsules only for isolated strokes) and builds one bounded CSR grid in
full-image coordinates. The exact packed words are cacheable as one immutable resident buffer;
per-pixel Metal work examines only the current cell's candidates instead of every authored point.
`src/proxy/warm_edit_gpu_mask_plan.*` is the single host lowering owner for all five mask kinds,
including working-space condition matrices and empty-brush semantics. Layer blending and optional
coverage capture consume the same prepared record and brush index.
`src/proxy/warm_edit_gpu_retouch_plan.*` independently lowers each ordered continuous Heal or
Clone region into raster-space capsules and a bounded CSR grid. Its immutable packed geometry is
cached by the resident-resource owner, while `warm_edit_gpu_retouch_encoder.*` preserves every
region's complete source snapshot in resident RGB buffers exactly as the CPU oracle does. Clone
copies through the indexed continuous coverage and shared affine donor mapping directly. Heal
computes a deterministic two-pass robust donor statistic plus a bounded affine boundary-light fit,
initializes the correction field, runs a brush-scale-aware bounded screened-Poisson Jacobi solve,
and feathers the result without leaving Metal. Mixed ordered Heal and Clone therefore remain in
the same command transaction as surrounding pixel-local and neighborhood stages.
`src/proxy/warm_edit_gpu_geometry_plan.*` seals the authoritative `PhotoGeometryLayout`, complete
  output canvas, bounded source tile, and output tile into one portable sampling contract. Its
  paired `warm_edit_gpu_geometry_encoder.*` performs crop, quarter-turn, mirror, fine straighten,
  bounded perspective, and bilinear resampling after every source-coordinate edit but before
  display conversion.
Geometry output never exceeds the resident source allocation, so previews and full-detail tiles
reuse the existing two synchronized RGB slots without another upload or host round trip.
`src/proxy/warm_edit_gpu_layer_plan.*` is the portable layer-composition admission and lowering
owner. It maps opacity, unmasked layers, normalized linear/radial gradients, and indexed
continuous brushes to the mirrored Metal blend ABI. `src/proxy/warm_edit_gpu_layer_dispatcher.*`
executes every admitted layer sequentially in one command buffer, snapshots only layers that
require blending, captures a requested mask before its own adjustment, reuses that resident float
coverage for the target blend, applies the same geometry into tightly packed R8, preserves the
settled linear analysis result, and publishes the paired RGB/coverage result only after the one
command transaction completes.
`src/proxy/warm_edit_gpu_stage_encoder.*` owns stage-specific resource admission, Metal kernel
order, and intermediate-buffer selection. `src/proxy/warm_edit_gpu_dispatcher.*` packs the
prepared transaction into one command buffer, interprets status, and performs the single final
readback. `warm_edit_gpu.mm` is the thin resident-raster session facade
shared by complete warm proxies and bounded full-detail working tiles. Callers pass the full-image
adjustment and display origins explicitly so tiled finishing effects and dithering do not acquire
seams.
`tests/warm_edit_gpu_contract_test.cpp` is the thin runner for the corresponding real-device
contract. Its responsibility-named children mirror resident session lifecycle, technical and
creative detail dispatch, guided Selective Tone, composed neighborhood order, and resident
side-table caches; their only shared fixture owns CPU-oracle parity inputs and comparisons.
The retouch child owns mixed continuous Heal/Clone parity, ordered source snapshots,
geometry-cache reuse, and the opt-in `SHADOW_TEST_WARM_RETOUCH_BENCHMARK`; its portable plan
contract proves tile-coordinate mapping and indexed-candidate completeness, while the focused
retouch seam contract crosses irregular full-detail tiles on real Metal. The separate portable
retouch-quality contract uses deterministic photographic stress fields to hold local illumination
adaptation and high-frequency Clone transfer stable without checking photo payloads into Git.
The geometry child owns node and layer CPU parity, transposed native-scale propagation, encoded
display parity, and the opt-in `SHADOW_TEST_WARM_GEOMETRY_BENCHMARK`; its portable plan contract
proves complete and bounded-tile coordinate lowering, while the focused geometry seam contract
proves crop, quarter-turn, mirror, straighten, and perspective remain byte-identical across
irregular node and layer tiles on real Metal.
Selective Tone and composed-stage children own opt-in CPU-versus-resident-Metal benchmarks, while
the detail-tile seam contract proves both one guided mask and a composed Selective Tone,
capture-sharpening, and full-resolution Texture/Clarity/Local Contrast plan remain invariant across
apron-expanded tiles and confirms that the composed plan uses resident Metal when available.
The layer-composition child owns opacity/gradient/continuous-brush CPU parity, resident brush-index
reuse, and the opt-in `SHADOW_TEST_WARM_LAYER_BENCHMARK` and
`SHADOW_TEST_WARM_BRUSH_BENCHMARK`; the portable brush-index contract proves stroke breaks and
candidate completeness. The focused detail-tile layer seam contract verifies that the same
normalized masks, continuous capsules, and creative-detail apron produce byte-identical whole
and irregular tiled output on resident Metal.
`tests/local_mask_coverage_contract_test.cpp` owns the public CPU paired-frame contract, all five
mask kinds, inactive/no-op targets, geometry, typed target rejection, and cancellation.
`tests/managed_raster_mask_contract_test.cpp` owns immutable raster encoding, bilinear sampling,
malformed-payload rejection, inversion, and the explicit resident-Metal-to-CPU fallback boundary.
`tests/warm_edit_gpu_contract/mask_coverage_contract_test.cpp` owns real-device CPU/Metal R8
parity, pre-adjustment-input order, resident target-blend reuse, inactive targets, geometry, and
atomic cancellation.

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
sequential Normal-blend layers and their local masks are a separate composition contract already
shared by CPU and Metal, while branching and additional blend modes remain separate work.
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
Each render owns its output/edit buffers, so const renders may safely run concurrently; the
original decoder session is neither retained nor revisited during slider interaction.
`render_rgb8` returns the tightly packed display-sRGB bytes produced by that render without
encoding them. The desktop uses this transient path while a gesture is active, then requests a
settled JPEG plus analysis for durable cache/publication only after interaction stops.

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
