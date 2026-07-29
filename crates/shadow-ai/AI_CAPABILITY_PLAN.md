# Shadow AI capability plan

This document is the executable roadmap for adding model-backed capabilities to
Shadow. It does not describe shipped inference. The current implemented
contracts and application integration remain documented in
[`README.md`](README.md).

The plan extends the existing model-independent manifests, resource/privacy
admission, generated-artifact contracts, feedback evidence, and explainable
group-relative scoring. It does not create a second AI architecture beside
`shadow-ai`.

## Status vocabulary

- **Current** means the contract or behavior exists in the repository today.
- **Candidate** means an upstream implementation is suitable for a measured
  prototype, subject to license and artifact review.
- **Planned** means Shadow has accepted the boundary but has not implemented it.
- **Deferred** means the option must not enter the product until the stated
  blocker changes.

No candidate model, provider, service, latency, or quality result in this
document is an implemented fact.

## Product invariants

Every AI capability must preserve these invariants:

1. AI never deletes an original or writes the manual Pick/Reject/rating ledger.
2. A generated result is rebuildable until the user accepts it. An accepted
   edit is an immutable, non-destructive dependency with exact provenance.
3. Local processing is the default. Remote processing is capability-specific,
   visibly opt-in, and never receives a RAW file by default.
4. A model result never hides its source revision, model revision, preprocessing
   contract, confidence, or fallback route.
5. Manual protection and manual edits outrank model policy. Low confidence
   produces an editable proposal, not a silent product decision.
6. A feature remains unavailable when license, privacy, resource, or artifact
   identity cannot be admitted truthfully. Shadow does not substitute fabricated
   scores, masks, or pixels.

## Capability-provider protocol

The existing [`AiCapability`](src/contract.rs) and
[`AiTaskKind`](src/contract.rs) enums are the only canonical capability
taxonomy. Provider routes refine those values; they must not add parallel
capability enums for promptable versus semantic masks, local versus generative
inpaint, or RGB versus sensor-domain denoise.

### Current owners to extend

| Current owner | Current contract | Planned extension |
| --- | --- | --- |
| [`src/contract.rs`](src/contract.rs) | `AiCapability`, `AiTaskKind`, `AiJobRequest`, `AiObservation`, `TaskPriority`, and `PrivacyClass` | Wrap the current request/result values in a provider execution lifecycle without renaming their semantics |
| [`src/manifest.rs`](src/manifest.rs) | Exact model artifacts, tensor contracts, execution targets, resource requirements, and license/distribution facts | Make a package registry and provider catalog consume the manifest instead of inventing package metadata |
| [`src/resource.rs`](src/resource.rs) | Deterministic `admit()` policy, `RemoteExecutionPolicy`, and immutable `RunPlan` | Add runtime resource leases, cancellation, and measured usage around an admitted plan |
| [`src/generated.rs`](src/generated.rs) | Typed subject-mask and denoise parameters, generated soft-mask/denoised-raster payloads, storage class, and `DenoiseDomainPolicy` | Add missing inpaint and super-resolution values; express mask, inpaint, and denoise variants as routes beneath the existing tasks |
| [`src/technical.rs`](src/technical.rs) and [`src/score.rs`](src/score.rs) | Model-free technical evidence and explainable group-relative selection | Admit model-derived defect, similarity, aesthetic, and uniqueness evidence without creating a generic “culling observation” capability |
| [`src/feedback.rs`](src/feedback.rs) | Append-only presentation/decision evidence and explicit-only training admission | Feed only admitted local learning pipelines; do not let a provider infer implicit negatives |

The roadmap routes map onto the current taxonomy as follows:

| Canonical capability/task | Planned provider routes |
| --- | --- |
| `SimilarityEmbedding` / `ExtractSimilarityEmbedding` | Comparable visual feature extraction and a frozen distance contract |
| `TechnicalQuality`, `BurstGrouping`, and `PersonalRanking` | Separate culling signals consumed by the current scoring policy |
| `SubjectMask` / `ProposeSubjectMask` | Point/box prompting, semantic selection, and optional matte refinement, all producing the current editable soft-mask contract |
| `DepthEstimation` / `EstimateDepth` | Depth evidence used by a conditional-mask generator |
| `InpaintPatch` / `GenerateInpaintPatch` | Deterministic local removal or explicitly generative provider routes under one provenance-bearing patch task |
| `Denoise` / `Denoise` | RGB and sensor-domain routes selected through the current `DenoiseDomainPolicy` |
| `SuperResolution` / `SuperResolve` | Fidelity-first enlargement with a future generated-raster payload contract |

**Current:** `AiJobRequest` is a serializable envelope and `admit()` can return a
deterministic `RunPlan`; no provider executes that plan. **Planned:** the first
runtime milestone adds three provider-neutral lifecycle operations:

1. planning validates the current request and task parameters, chooses a
   provider route, calls current resource admission, and returns an immutable
   execution identity;
2. execution consumes that admitted identity, reports monotonic progress, and
   publishes exactly one terminal result or error;
3. cancellation stops new admission immediately and cooperatively stops
   in-flight work; late results retain their identity and are discarded by the
   caller.

The application, not `shadow-ai`, owns job scheduling, process isolation,
download UX, persistence, and Recipe integration.

### Stable job identity

A job identity must include:

- source representation and source-revision digests;
- Recipe/render revision when inference observes edited pixels;
- orientation, image extent, color space, transfer function, sample format, and
  alpha convention;
- region of interest, expanded context, and normalized source coordinates;
- prompt/mask digest and operation parameters;
- provider, model, artifact, preprocessing, and execution-plan revisions;
- seed when a provider exposes one.

The result receipt adds output digests, confidence, actual execution route,
timing/resource measurements, and provider request identity. A remote receipt
must not persist credentials or provider response bodies containing private
pixels.

## Model packages, licenses, and privacy admission

The existing manifest is the source of truth. The planned package manager must
download or side-load model packages outside the repository, address them by
content, verify their signatures and exact hashes, and version them. Git must
contain neither model binaries nor implicit download artifacts. There is no
package manager, downloader, or signature verifier today.

### License ledger

Admission must review these as independent fields:

- host/inference code license;
- model architecture or repository license;
- exact weight-file license;
- training-data and benchmark-data terms;
- bundled runtime and transitive dependency licenses;
- redistribution, commercial-use, attribution, and acceptable-use conditions.

A permissive code repository does not prove that one checkpoint or its training
data may be redistributed. Every artifact admitted by the planned package
registry must record its upstream URL, revision, digest, local conversion,
notices, and audit decision. Non-commercial weights are excluded from
distributed product packages even when their code is permissive.

Initial upstream classification:

| Candidate | Upstream terms to verify | Planning decision |
| --- | --- | --- |
| [SAM 2](https://github.com/facebookresearch/sam2/blob/main/LICENSE) / [Apple Core ML SAM 2.1](https://huggingface.co/apple/coreml-sam2.1-tiny) | Apache-2.0 is declared by upstream model repositories | Candidate after exact checkpoint audit |
| [Depth Anything V2 Small](https://github.com/DepthAnything/Depth-Anything-V2#license) | Apache-2.0 | Candidate |
| [Depth Anything V2 Base/Large/Giant](https://github.com/DepthAnything/Depth-Anything-V2#license) | CC-BY-NC-4.0 in the upstream repository | Deferred for commercial distribution |
| [Grounding DINO](https://github.com/IDEA-Research/GroundingDINO/blob/main/LICENSE) | Apache-2.0 | Candidate after checkpoint/data audit |
| [ViTMatte](https://github.com/hustvl/ViTMatte/blob/main/LICENSE) | MIT | Candidate after checkpoint/data audit |
| [LaMa](https://github.com/advimman/lama/blob/main/LICENSE) | Apache-2.0 | Candidate after checkpoint/data audit |
| [NAFNet](https://github.com/megvii-research/NAFNet/blob/main/LICENSE) | MIT for NAFNet code; dependency terms remain separate | Candidate only after complete package audit |
| [Restormer](https://github.com/swz30/Restormer/blob/main/LICENSE.md) | MIT | Candidate after checkpoint/data audit |
| [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN/blob/master/LICENSE) | BSD-3-Clause | Candidate after checkpoint/data audit |
| [SwinIR](https://github.com/JingyunLiang/SwinIR/blob/main/LICENSE) / [HAT](https://github.com/XPixelGroup/HAT/blob/main/LICENSE) | Apache-2.0 | Candidate after checkpoint/data audit |
| [FLUX.1 Fill dev](https://huggingface.co/black-forest-labs/FLUX.1-Fill-dev) | Black Forest Labs non-commercial terms | Deferred |
| [SUPIR](https://github.com/Fanghua-Yu/SUPIR) | Non-commercial upstream terms | Deferred |

Cloud APIs remain proprietary services. Their API documentation is technical
evidence, not approval of price, retention, training use, regional availability,
or data-processing terms. Legal/privacy review is repeated before each provider
is enabled.

### Privacy and upload scope

**Current:** every request and input artifact uses one of the canonical
`PrivacyClass` values from [`src/contract.rs`](src/contract.rs): `Public`,
`Personal`, or `SensitiveBiometric`. Current deterministic admission applies
`RemoteExecutionPolicy` from [`src/resource.rs`](src/resource.rs):
`Disabled`, `PublicOnly`, `PersonalAllowed`, or
`SensitiveBiometricAllowed`. Those values describe information sensitivity and
remote-policy ceilings; they do not claim that a remote adapter exists.

**Planned:** a remote adapter must record payload extent as an orthogonal upload
scope, such as a bounded rendered crop or a full rendered image. Upload extent
must not become another `PrivacyClass`, and an exact enum is not accepted until
an adapter prototype establishes its contract. Remote execution must remain
disabled by default and require capability-level consent plus a visible
destination/provider.

Before any upload, the planned adapter must strip GPS and unrelated EXIF, apply
orientation, encode only the admitted raster and mask, estimate cost, and
enforce the configured ceiling. Full RAW upload remains prohibited by default.
Remote `SensitiveBiometric` inference is explicitly deferred pending a stronger
product, privacy, and legal decision.

No background task may upgrade a local job to remote execution. A remote failure
may fall back only to a declared local route; it may not silently select another
remote provider.

## Scheduling and GPU-resident pixels

**Current:** [`TaskPriority`](src/contract.rs) has nine values and current
resource admission consumes them. The planned application scheduler may derive
four lanes, but those lanes do not replace the enum:

| Planned scheduler lane | Current `TaskPriority` values |
| --- | --- |
| Interactive input | `CurrentInput` |
| Visible presentation and prefetch | `CurrentViewport`, `VisibleThumbnail`, `NearbyPrefetch` |
| Active analysis and background maintenance | `ImportValidation`, `CurrentCollectionAnalysis`, `LibraryBackgroundAnalysis`, `CacheMaintenance` |
| User-requested completion | `UserExport` |

The planned runtime must let one resource lease own model residency, predicted
and measured memory, device queue use, cancellation, and unload policy.
Base-memory devices should admit at most one heavyweight local model until
measurements prove safe concurrency. Background jobs must yield rather than
causing slider, brush, or viewport latency.

On Apple platforms, Core ML is the first local runtime candidate. It can select
CPU, GPU, and Neural Engine through
[`MLComputeUnits`](https://developer.apple.com/documentation/coreml/mlcomputeunits).
Compression and compute-unit choices are benchmark decisions, because Apple's
[optimization guidance](https://apple.github.io/coremltools/docs-guides/source/opt-overview.html)
states that their benefit is model- and hardware-dependent.

The planned resident pixel route is:

```text
Shadow render texture
  -> Metal/IOSurface-backed CVPixelBuffer
  -> Core ML or a narrow Metal preprocessor
  -> GPU-resident mask/result texture
  -> display composition
```

The planned interactive path must not encode JPEG, decode JPEG, or copy a
complete image through QML. It should read pixels back to the CPU only when a
generated artifact is promoted to managed derived storage or a provider cannot
consume the resident contract. That fallback must be explicit in the receipt.

The planned mask runtime must cache embeddings by exact source/render revision,
model artifact, preprocessing, orientation, and working extent. Planned
denoise, inpaint, and super-resolution providers must use bounded tiles with
overlap/halo and a seam contract. Repeated preview should reuse the same
resident resource lease; cache telemetry is not part of durable edit identity.

Core ML conversion should test FP16 first, then measured weight compression.
Apple's [typed execution](https://apple.github.io/coremltools/docs-guides/source/typed-execution.html)
and [palettization](https://apple.github.io/coremltools/docs-guides/source/opt-palettization-overview.html)
guides define the platform constraints. An
[`MLComputePlan`](https://apple.github.io/coremltools/docs-guides/source/mlmodel-utilities.html)
and Xcode performance report should accompany each accepted package.
ExecuTorch's [Core ML and MPS backends](https://docs.pytorch.org/executorch/stable/using-executorch-building-from-source.html)
remain fallback candidates only when direct Core ML conversion cannot preserve
the model contract.

## AI-assisted culling

### Current boundary

Shadow currently has model-free technical observations, exact presentation
evidence, manual review ledgers, group-relative scoring policy, and a small
deterministic preference head. It has no active feature extractor, aesthetic
model, automatic ranking, or automatic Pick/Reject writer.

### First measured pipeline

The first candidate is a local evidence ensemble:

1. Partition a shoot using capture time and user-visible grouping controls.
2. Cluster near-duplicates with Apple Vision
   [`VNFeaturePrintObservation`](https://developer.apple.com/documentation/vision/vnfeatureprintobservation)
   and its
   [distance contract](https://developer.apple.com/documentation/vision/vnfeatureprintobservation/computedistance(_:to:)).
3. Add existing comparable technical evidence only when the complete analysis
   artifact and preprocessing revision match.
4. For detected faces, add Vision
   [face capture quality](https://developer.apple.com/documentation/vision/vndetectfacecapturequalityrequest)
   as a separate signal, never as an identity embedding.
5. Evaluate Vision
   [image aesthetics](https://developer.apple.com/documentation/vision/calculateimageaestheticsscoresrequest)
   and the published
   [MUSIQ method](https://research.google/pubs/musiq-multi-scale-image-quality-transformer/)
   against the same held-out shoots. No MUSIQ implementation, checkpoint, or
   training artifact is selected; any prototype must enter the complete
   license and artifact ledger first.
6. Apply the existing explainable group-relative policy and learn only the small
   preference head from explicit comparable pairwise evidence.

The [LAION aesthetic predictor](https://github.com/LAION-AI/aesthetic-predictor)
is research-only: its small aesthetic-label set and CLIP-derived prior create a
material domain and cultural-bias risk. It is not a default candidate.

The UI may propose a group representative, a duplicate fold, or a ranked review
order. It cannot hide a manually protected or unique photo, write Reject, or
claim that one global aesthetic score represents the user's intent. Each
recommendation exposes reason categories and confidence.

### Data and acceptance

Use rights-cleared, shoot-disjoint bursts from portrait, event, wildlife, street,
product, and landscape photography. At least three independent raters label the
evaluation subset; one user's explicit feedback trains only that user's local
preference state unless they separately opt in to contribute data.

Report:

- keeper recall at each proposed review-compression ratio;
- NDCG@k and rank correlation inside comparable groups;
- duplicate-cluster pairwise F1 and adjusted Rand index;
- false suppression of unique or manually protected photos, which must be zero;
- worst-subgroup results by camera, genre, lighting, skin tone, and face count;
- cold/warm throughput, peak memory, energy, and UI interference.

## Promptable, semantic, and conditional masks

### First local mask provider

The first universal interactive provider should benchmark Apple's Core ML
[SAM 2.1 Tiny](https://huggingface.co/apple/coreml-sam2.1-tiny) and
[Small](https://huggingface.co/apple/coreml-sam2.1-small) packages. Upstream
[SAM 2](https://github.com/facebookresearch/sam2) accepts point, box, and mask
prompts. Tiny is the base-device candidate; Small is an optional quality package
only if it produces a measured boundary improvement.

The prototype should run the encoder once for one working source revision.
Foreground/background points and boxes should then run the prompt decoder
against the cached embedding, as demonstrated by the upstream
[`sam2-studio`](https://github.com/huggingface/sam2-studio) sample. The output is
required to remain an editable soft mask; include/exclude brush corrections are
first-class evidence rather than being discarded after refinement.

Apple Vision's
[`GenerateForegroundInstanceMaskRequest`](https://developer.apple.com/documentation/vision/generateforegroundinstancemaskrequest)
is a lower-cost candidate for supported foreground/person cases. The 2026 Beta
[`GenerateIterativeSegmentationRequest`](https://developer.apple.com/documentation/vision/generateiterativesegmentationrequest)
supports points, boxes, and scribbles, but must remain availability-gated until
the shipping SDK/OS and downloadable-asset behavior are validated. It cannot be
the only implementation while Shadow's desktop baseline does not guarantee it.

### Conditional-mask taxonomy

Every planned generator must produce the same normalized soft-alpha contract
and support combination through `AND`, `OR`, `NOT`, add, subtract, and
intersect operations:

| Family | Conditions |
| --- | --- |
| Tone | luminance, shadows, highlights, bounded ranges |
| Color | hue, chroma, perceptual distance from sampled color, individual channels |
| Structure | edges, texture/frequency, detail, focus/sharpness, estimated noise |
| Geometry | linear/radial gradient, distance, direction, shape |
| Depth | near/middle/far and bounded depth ranges |
| Semantic | foreground/background, people, face, skin, hair, clothes, sky, ground, water, vegetation, architecture, animals, text, prompted object |
| Relational | intersection or exclusion between any condition and a manual brush/gradient |

The planned mask UX keeps painted masks anonymous and node-local by default;
their pixels are not useful presets. A smart/conditional mask may save and name
its generator recipe because that recipe can be reused, but its generated
pixels remain tied to one source revision.

Later measured candidates are:

- [Grounding DINO](https://github.com/IDEA-Research/GroundingDINO) plus SAM 2
  for text-selected concepts;
- [Depth Anything V2 Small](https://github.com/DepthAnything/Depth-Anything-V2)
  for depth conditions;
- [ViTMatte](https://github.com/hustvl/ViTMatte) for trimap-driven hair, fur,
  glass, and other difficult alpha boundaries.

The larger Depth Anything V2 weights are deferred because upstream assigns them
non-commercial terms. Florence-2 remains exploratory until conversion,
autoregressive-decoder latency, checkpoint licensing, and memory are measured;
its [official model card](https://huggingface.co/microsoft/Florence-2-base)
does not by itself establish a product-quality photo mask.

### Mask acceptance

The photo-domain corpus must include hair, fur, branches, wires, transparent
objects, smoke, low-contrast boundaries, overlapping people, tiny objects, and
strongly edited color. Report mIoU, Dice, boundary F-score, first-click quality,
clicks-to-IoU-90, encoder P50/P95, warm prompt P50/P95, peak memory, and
tile/full-image parity.

The provisional interactive gate on the minimum supported Mac is:

- no inference work on the UI thread;
- warm prompt-to-visible-mask P95 at or below 100 ms on the fixed benchmark
  proxy;
- first usable mask P95 at or below 1.5 seconds after a cold model load;
- cancellation acknowledged by the scheduler within 250 ms;
- no seams or coordinate drift between FIT, 100%, and tiled views.

Failure to meet an interactive gate keeps the provider experimental; it does not
justify faking a synchronous result.

## Local object removal and remote generative fill

Clone and Heal remain deterministic correction tools. Generative fill is a
separate capability because it may invent semantic content.

### Local removal

[LaMa](https://github.com/advimman/lama) is the first local large-mask inpaint
candidate. It should be evaluated as **Object Remove (Local)** without a text
prompt. Before integration, its exact checkpoint, training-data provenance,
color behavior, tile overlap, memory, and redistribution must pass admission.

The LaMa prototype must receive a bounded expanded context around the mask. Its
result must be composited only inside an explicitly expanded and feathered edit
region. Outside-region pixel invariance is a hard acceptance contract. The
eventual interaction must let the user compare, regenerate, discard, or accept
the proposal.

### Remote fill

Two initial providers should be evaluated behind the same adapter contract:

- Adobe Firefly Services
  [Photoshop API v2 GA](https://developer.adobe.com/firefly-services/docs/photoshop/getting-started/v2-ga/)
  and the
  [Photoshop API v2 reference](https://developer.adobe.com/firefly-services/docs/photoshop/api/photoshop-v2/),
  where generative fill/expand is exposed through Actions;
- Black Forest Labs
  [FLUX.1 Fill](https://docs.bfl.ml/flux_tools/flux_1_fill).

Provider APIs and model names are volatile. The exact Adobe action schema and
BFL service/model endpoint must be selected and revalidated when the adapter
prototype starts; this shortlist is not a frozen wire contract.

[Stability inpaint](https://platform.stability.ai/docs/api-reference) and
[Vertex Imagen editing](https://docs.cloud.google.com/vertex-ai/generative-ai/docs/image/edit-images-overview)
remain comparison candidates. Vertex's published model/endpoint migration
notices mean the exact supported route must also be reselected at prototype
time. Alibaba's
[Qwen Image editing API](https://help.aliyun.com/en/model-studio/qwen-image-edit-api)
is an additional natural-language editing comparison, not a replacement for
exact mask control.

Local [Qwen-Image-Edit](https://huggingface.co/Qwen/Qwen-Image-Edit) is deferred
despite its permissive repository terms: its 20B-class resource envelope and
exact-mask behavior are unsuitable for the current local product baseline.

Provider selection follows a blind corpus evaluation, privacy/retention review,
regional availability, deletion guarantees, cost ceiling, ICC/color behavior,
and retry/idempotency behavior. Remote fill is never a silent fallback from
local Heal or LaMa.

Every candidate version must record source revision, context crop, mask digest,
prompt, seed when available, provider/model version, request identity, and
output digest. Accepted pixels must be promoted to managed derived storage
before a Recipe can depend on them.

Acceptance reports outside-region invariance, boundary gradient discontinuity,
seam color difference, LPIPS/DISTS inside the context, face/text/identity
preservation, multi-seed failure rate, provider refusal rate, and blinded human
preference. Repeating lines, architecture, hair, text, faces, sky, and repeated
textures are mandatory failure sets.

## RGB denoise versus true RAW denoise

The two capabilities must remain different contracts.

### RGB denoise candidate

[NAFNet](https://github.com/megvii-research/NAFNet) and
[Restormer](https://github.com/swz30/Restormer) are candidates for a Core ML
bake-off on color-managed RGB working pixels. They may support JPEG cleanup or a
late RGB denoise stage. They must not be described as sensor-aware RAW denoise.

The bake-off uses the same tiling, halo, color, and resource receipt. A classical
deterministic GPU denoiser remains the fallback. Topaz's
[Image API models](https://developer.topazlabs.com/image-api/available-models)
may serve as a closed, remote quality reference after privacy and service-term
review; they do not enter the RAW CFA pipeline.

### True RAW denoise direction

Product-quality RAW denoise needs a Shadow-owned sensor-domain model conditioned
on:

- CFA pattern and phase;
- black and white levels;
- sample layout and bit depth;
- ISO, exposure, and available camera noise calibration;
- hot-pixel, row/column, shot, and read-noise characteristics.

The training program needs rights-cleared paired short/long exposures, aligned
bursts, dark frames, flat fields, multiple temperatures and ISO levels, plus
Poisson-Gaussian, banding, and hot-pixel augmentation. The
[PMN research implementation](https://github.com/megvii-research/PMN) is a
method reference for paired real and physics-guided noise synthesis, not a
universal pretrained product model. Public datasets such as SIDD, SID, ELD, and
DND require separate use and redistribution review before they enter training
or release evidence.

Evaluation includes sensor-domain PSNR/SSIM, post-color Delta E 2000,
LPIPS/DISTS, MTF/detail retention, residual noise power spectrum, hot pixels,
banding, demosaic artifacts, and tile seams. Stars, fur, hair, text, skin, and
fine repeating texture explicitly test waxiness and hallucinated detail.

No RAW AI denoise ships until the model beats the current deterministic path on
held-out cameras without unacceptable color shift, texture loss, or
camera-domain regression.

## 2x super-resolution

[Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) is the first 2x Core ML
candidate because its convolutional path is comparatively conversion-friendly.
[SwinIR](https://github.com/JingyunLiang/SwinIR) and
[HAT](https://github.com/XPixelGroup/HAT) are measured alternatives when their
quality gain justifies latency and memory.

The planned first product mode is **Fidelity 2x**:

- local and deterministic for a fixed model revision;
- export-time or explicitly requested, never blocking ordinary adjustment
  preview;
- tiled with overlap/halo and exact scale;
- run in the model's admitted display/RGB domain, not advertised as recovered
  scene-linear RAW truth.

A future **Creative Detail** route is separate, visibly generative, and disabled
by default. Topaz or another cloud service may define a comparison ceiling, but
does not become the default without the same privacy and provenance contract.

Evaluate 12, 24, and 45 megapixel sources using synthetic and real degradation.
Report PSNR/SSIM, LPIPS/DISTS, Delta E, ringing/halo measures, OCR correctness,
face-identity drift, false texture/hallucination audit, seams, P50/P95 export
time, peak memory, and energy. A 4x model is deferred until conservative 2x has
demonstrated value.

## Delivery sequence

### Phase 0: runtime and evidence foundation

1. Freeze provider plan/execute/cancel values and stable job/result identity.
2. Add signed model-package registry, exact artifact audit, download/side-load,
   and versioned eviction outside the repository.
3. Implement local worker isolation, resource leases, cancellation, crash
   recovery, and truthful fallback receipts.
4. Establish the Metal/CVPixelBuffer/Core ML resident-pixel path.
5. Build the benchmark harness, rights-cleared golden corpus, and immutable
   result format.
6. Implement managed derived-raster storage plus an immutable promotion and
   revision contract for generated raster masks before accepted generated
   pixels can enter a Recipe. Recipe-local vector/spatial mask revisions already
   exist and are not reimplemented by this phase.

No user-visible model feature should bypass this phase.

### Phase 1: bounded local assistance

1. Benchmark Vision FeaturePrint, face quality, and aesthetics with the existing
   culling evidence path; expose recommendations only after held-out gates pass.
2. Integrate SAM 2.1 Tiny as an editable promptable-mask prototype; compare
   Small and Vision routes.
3. Add deterministic tone, color, structure, geometry, and composition
   operators to the common conditional-mask contract.

### Phase 2: pixel-generating local tools

1. Benchmark LaMa local object removal.
2. Benchmark NAFNet and Restormer as RGB-domain denoise only.
3. Benchmark Real-ESRGAN 2x, then compare SwinIR/HAT-S.
4. Promote only accepted outputs through managed derived storage and Recipe
   provenance.

### Phase 3: semantic depth and optional services

1. Evaluate Grounding DINO + SAM 2, Depth Anything V2 Small, and ViTMatte.
2. Run the Adobe-versus-BFL remote-fill blind evaluation after legal/privacy
   approval.
3. Start rights-cleared camera-noise capture and the true RAW denoise training
   program.
4. Evaluate a creative-upscale provider only as a separate, explicit mode.

## Shared benchmark and release gates

Each candidate uses the same immutable benchmark record:

- repository revision, provider/model/artifact/preprocessing revisions;
- exact fixture identities and rights classification;
- hardware, OS, runtime, compute units, power state, and thermal state;
- input size/domain and cold/warm state;
- P50/P95 latency, throughput, peak RSS/unified/device memory, energy, and
  cancellation/unload latency;
- quality metrics, subgroup results, failures, and retained visual examples;
- CPU/fallback parity, tile/full-image parity, and deterministic replay where
  promised.

Phase 0 must select and record the minimum supported Apple Silicon memory tier.
When available, M1 Pro 32 GB and Windows 64 GB/RTX 4070 Ti machines are named
benchmark fixtures, not product support promises. The Windows fixture becomes
relevant only after a Windows provider is implemented. A current higher-tier
Apple Silicon fixture should measure scaling. Image sizes are at least 12, 24,
and 45 megapixels.

A candidate cannot become default unless:

1. its exact artifact and complete license ledger pass admission;
2. its privacy class and remote behavior are represented accurately;
3. the minimum hardware tier rejects or degrades safely under low memory;
4. no heavy work blocks the UI thread and interactive gates pass where relevant;
5. failure, cancellation, crash, stale-result, and offline paths are tested;
6. quality beats the named deterministic/current baseline on held-out data and
   does not regress a protected subgroup beyond the accepted bound;
7. generated pixels and masks round-trip through immutable provenance and
   non-destructive undo;
8. the package can be removed or upgraded without making accepted edits
   uninterpretable.

## Explicitly deferred

The following are outside the current implementation sequence:

- automatic deletion, automatic hard Reject, or a model writing the manual
  review ledger;
- background cloud upload, implicit provider selection, or full RAW upload by
  default;
- local FLUX.1 Fill `[dev]`, SUPIR, or any other non-commercial weight in a
  distributed product;
- local 12B/20B-class generative editing until license, memory, latency, and
  exact-mask behavior change materially;
- Depth Anything V2 Base/Large/Giant under their current non-commercial terms;
- claiming an RGB restoration model is true RAW denoise;
- default generative super-resolution or language suggesting invented detail is
  recovered truth;
- relying exclusively on a Beta Apple Vision API or downloadable system model;
- accepting a model because of leaderboard or vendor marketing without
  Shadow-owned photo-domain evidence;
- provider implementations for the existing `AutoDevelopRecipe` capability;
- provider implementations for `SemanticEmbedding`, `SemanticCaption`, a VLM,
  or other semantic caption/search indexing;
- `QueryPlanning`, natural-language library search, or an LLM search agent;
- face identity recognition, persistent person clustering, or a biometric
  identity store;
- remote `SensitiveBiometric` inference, even though the current policy contract
  can represent a future opt-in ceiling;
- implicit, federated, or remote upload of feedback, preference examples, or
  user-learning state.

## Upstream evidence index

Primary sources used by this plan:

- Apple Vision:
  [overview](https://developer.apple.com/documentation/vision),
  [aesthetics](https://developer.apple.com/documentation/vision/calculateimageaestheticsscoresrequest),
  [face capture quality](https://developer.apple.com/documentation/vision/vndetectfacecapturequalityrequest),
  [feature prints](https://developer.apple.com/documentation/vision/vnfeatureprintobservation),
  [foreground masks](https://developer.apple.com/documentation/vision/generateforegroundinstancemaskrequest),
  and
  [iterative segmentation](https://developer.apple.com/documentation/vision/generateiterativesegmentationrequest).
- Apple Core ML:
  [compute units](https://developer.apple.com/documentation/coreml/mlcomputeunits),
  [optimization](https://apple.github.io/coremltools/docs-guides/source/opt-overview.html),
  [typed execution](https://apple.github.io/coremltools/docs-guides/source/typed-execution.html),
  and
  [palettization](https://apple.github.io/coremltools/docs-guides/source/opt-palettization-overview.html).
- Promptable and conditional masks:
  [SAM 2](https://github.com/facebookresearch/sam2),
  [Apple SAM 2.1 Tiny](https://huggingface.co/apple/coreml-sam2.1-tiny),
  [Apple SAM 2.1 Small](https://huggingface.co/apple/coreml-sam2.1-small),
  [Grounding DINO](https://github.com/IDEA-Research/GroundingDINO),
  [Depth Anything V2](https://github.com/DepthAnything/Depth-Anything-V2),
  and [ViTMatte](https://github.com/hustvl/ViTMatte).
- Restoration:
  [LaMa](https://github.com/advimman/lama),
  [NAFNet](https://github.com/megvii-research/NAFNet),
  [Restormer](https://github.com/swz30/Restormer),
  [PMN](https://github.com/megvii-research/PMN),
  [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN),
  [SwinIR](https://github.com/JingyunLiang/SwinIR),
  and [HAT](https://github.com/XPixelGroup/HAT).
- Remote APIs:
  [Adobe Photoshop API v2](https://developer.adobe.com/firefly-services/docs/photoshop/api/photoshop-v2/),
  [BFL FLUX.1 Fill](https://docs.bfl.ml/flux_tools/flux_1_fill),
  [Stability](https://platform.stability.ai/docs/api-reference),
  [Vertex Imagen editing](https://docs.cloud.google.com/vertex-ai/generative-ai/docs/image/edit-images-overview),
  [Qwen Image editing](https://help.aliyun.com/en/model-studio/qwen-image-edit-api),
  and [Topaz Image API](https://developer.topazlabs.com/image-api/available-models).
