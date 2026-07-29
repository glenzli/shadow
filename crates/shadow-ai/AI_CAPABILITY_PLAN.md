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

## Implemented foundation snapshot

The repository now contains the first provider-neutral execution contract, not
model-backed inference:

- `AiJobRequest` is only application intent. It no longer carries a provider,
  model, checkpoint, or service choice. Admission binds that intent to an exact
  local artifact set, system-framework request revision, or remote service
  identity. `AiObservation` is an exact-v1 result envelope constructed normally
  only by consuming a successful lease output; duplicated request/task/target
  fields must match runtime provenance, and explanation evidence is capped at
  64 signals.
- A move-only runtime lease supports cooperative cancellation, validated
  monotonic progress, complete provider/model/framework/service route matching,
  exact full-plan matching, and one terminal receipt. On success the consumed
  lease, not the provider, creates a move-only payload envelope whose provenance
  binds the complete request, input inventory, route, and execution plan.
  The receipt records that plan, usage, and whether the route was primary or an
  explicitly declared fallback. The application scheduler still owns unique
  lease issuance and revocation; this portable value is not an authentication
  token.
- Local manifests identify a complete content-addressed artifact set. Core ML
  packages are reproducible downloaded archives/blobs in that set. A
  domain-separated, length-prefixed BLAKE3 digest of the canonical-path-sorted
  inventory is the set identity, and installed availability must match it
  exactly. Extracted `.mlpackage` trees, compiled `.mlmodelc`, and device
  specialization are rebuildable runtime caches, not distributed model identity.
  Exact-v1 manifest/request/route/plan/provenance decoding rejects unknown
  nested fields, duplicate sets, unsupported versions, and oversized streamed
  vectors. Portable package paths reject traversal, drive/UNC/ADS syntax,
  reserved device names, leading/trailing-space, short-name `~`, and case
  aliases, and Unicode normalization ambiguity before inventory hashing.
- Remote provider manifests are separate from local model manifests. They
  declare rendered-RGB/mask upload scope, privacy ceiling, retention, training
  use, terms revision, offline behavior, idempotency, and cancellation. The
  implemented path first requires application-store receipts for sanitized,
  verified outbound objects, then accounts outbound rather than source bytes.
  Its opaque expiring grant binds the complete request, manifest/legal facts,
  policy and consent revisions, exact receipt inventory, and stable idempotency
  key. Receipts are canonicalized by request input index before both identity
  hashing and grant/transport inventory. RAW files, sensor mosaics,
  scene-linear tiles, and frozen feature vectors fail closed. The legacy local
  manifest planner cannot authorize `RemoteApi`.
- Generated pixels still have no durable store in this crate. The implemented
  promotion transaction consumes only a lease-issued successful-output envelope
  and invokes an application-owned managed-store authority for the exact bytes
  before a Recipe integration may consume them. Managed authority is move-only
  and non-deserializable; a persisted descriptor must pass store verification
  before reload reconstructs it.
- The first culling boundary validates complete feature-print distance evidence,
  applies a deterministic greedy complete-link partition, and suggests only a
  similarity medoid as the first photo to review. It produces no Pick/Reject
  mutation and does not claim the medoid is the sharpest, most aesthetic, or
  otherwise “best” photo.
- An Apple Vision FeaturePrint provider skeleton pins request revision 1 and
  the OS build. The current Rust build does not link Vision, so it returns an
  explicit `adapter_not_linked` terminal on macOS (or
  `platform_unsupported` elsewhere) and never fabricates distances.

These are pre-release v1 contract replacements. There is no persisted or
cross-process `AiJobRequest`/`ModelManifest` consumer in the repository today;
deserialization validates exact versions recursively, rejects unknown fields
and oversized inventories, and fails closed on removed request fields instead
of silently honoring stale model selection.

## Current route decisions

| Capability | Route and openness | Deployment/identity | Decision now |
| --- | --- | --- | --- |
| Similar burst grouping | Apple Vision FeaturePrint, closed system framework | Pinned Vision request revision + OS build; no weight claim | First native adapter to link and benchmark; similarity evidence only |
| Promptable mask | Apple [SAM 2.1 Tiny Core ML](https://huggingface.co/apple/coreml-sam2.1-tiny), Apache-2.0 model card, float16 encoder/prompt/decoder set | Exact multi-archive artifact set; local Core ML | First universal mask prototype after package registry/runtime; Small is measured optional quality tier |
| System foreground mask | Apple Vision foreground-instance mask, closed system framework | Pinned request/OS identity | Lower-cost supported-case comparison, not universal fallback |
| Local model object remove | LaMa method/source is open; currently referenced checkpoint rights are unresolved | Side-loaded local prototype plus generated-raster provenance | Synthesized pixels require the same disclosure/promotion contract as a remote generator; do not bundle or auto-download before exact checkpoint/data audit |
| Remote generative fill | Adobe async Fill, BFL FLUX.1 Fill, Stability inpaint; closed services | Separate remote manifest, explicit rendered crop/mask upload, provider request receipt | Opt-in comparison; never implicit Heal fallback and never RAW upload |
| RGB denoise | NAFNet/Restormer open-source candidates | Exact local package, RGB working-domain receipt | Core ML bake-off only; never call it RAW denoise |
| Mac system true-RAW render | Apple [Core Image RAW 9](https://developer.apple.com/videos/play/wwdc2026/305/), closed system framework on macOS 27 | `CIRAWFilter` decoder v9 + OS build + exact camera/support-property snapshot; OS-delivered tiled Core ML joint demosaic/denoise on ANE | First callable Mac-only true-RAW quality/performance benchmark; availability-gated, not a portable replacement |
| Shadow-owned true RAW denoise | No product checkpoint selected; LED/PMN are research references | Future CFA/noise-profile model and benchmark identity | Longer-term portable/controllable route; deterministic RAW denoise remains fallback |
| Fidelity 2x | Classical SwinIR x2 open-source candidate | Exact local package; deterministic fixed revision | First conservative baseline |
| Creative/restorative upscale | Real-ESRGAN open-source candidate; Adobe/Stability closed services | Generated-raster provenance and promotion | Separate generative mode/comparison, never fidelity default; Imagen 4 preview is deferred |

## Product invariants

Every AI capability must preserve these invariants:

1. AI never deletes an original or writes the manual Pick/Reject/rating ledger.
2. A generated result is rebuildable until the user accepts it. An accepted
   edit is an immutable, non-destructive dependency with exact provenance.
   Model-inpainted pixels remain synthesized when execution is local,
   deterministic, or repeatable; fidelity-first model upscales remain derived
   rasters with exact provenance.
3. Local versus remote is a deployment, privacy, cost, and availability axis;
   it does not weaken generated-content disclosure or provenance. Local
   processing is the default. Remote processing is capability-specific, visibly
   opt-in, and never receives a RAW file by default.
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
| [`src/contract.rs`](src/contract.rs) | Provider-neutral `AiJobRequest`, `AiCapability`, `AiTaskKind`, `AiObservation`, `TaskPriority`, and `PrivacyClass` | Keep application intent independent of route selection |
| [`src/runtime/`](src/runtime/) | Exact admitted route and full plan, route-owned estimate, move-only lease, cancellation, monotonic progress, runtime-issued provenance envelope, terminal receipt, and explicit fallback disclosure | Add application scheduling/process isolation and real provider adapters |
| [`src/manifest.rs`](src/manifest.rs) | Exact multi-blob artifact sets, including Core ML package archives, tensor contracts, execution targets, resource requirements, and license/distribution facts | Make a content-addressed package registry consume the manifest |
| [`src/remote.rs`](src/remote.rs) | Index over exact remote-service manifests, prepared outbound-store receipts, and opaque fail-closed request grants | Add no transport adapter until legal/privacy/product approval |
| [`src/resource.rs`](src/resource.rs) | Deterministic local RAM/VRAM/thread `admit()` policy and immutable `RunPlan`; `RemoteApi` is never locally admitted | Give scratch storage its own authority; feed measured route usage into estimates without trusting request hints as safety budgets |
| [`src/generated.rs`](src/generated.rs) | Typed subject-mask and denoise parameters, rebuildable generated soft-mask/denoised-raster identities, and `DenoiseDomainPolicy` | Add missing inpaint and super-resolution values; express mask, inpaint, and denoise variants as routes beneath the existing tasks |
| [`src/derived_raster.rs`](src/derived_raster.rs) | Lease-provenance-consuming store promotion plus store-verified reload of opaque managed authority | Implement the application-owned managed derived store and Recipe reference |
| [`src/culling.rs`](src/culling.rs) | Complete pairwise feature-distance validation, deterministic greedy complete-link grouping, similarity-medoid review start | Calibrate Vision thresholds and combine only through separately evaluated evidence |
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

The current unlinked Apple Vision route intentionally advertises only
`BurstGrouping` / `ProposeBurstGroup`: it consumes a caller-selected photo
window and would return a complete distance batch for the grouping owner. It
does not export or persist Vision's feature-print bytes as a general
`SimilarityEmbedding`, and it does not advertise `TechnicalQuality`.

**Current:** `AiJobRequest` is a provider-neutral exact-v1 serializable intent,
`admit_local_execution()` selects an exact artifact set using a provider-owned
resource estimate, and a runtime lease defines three provider-neutral lifecycle
operations:

1. planning validates the current request and task parameters, chooses a
   provider route, calls current resource admission, and returns immutable route
   and complete execution-plan identities;
2. execution consumes that admitted identity, reports validated monotonic
   progress, and publishes exactly one terminal result or error; success is
   wrapped with runtime-issued request/input/route/plan provenance;
3. cancellation stops new admission immediately and cooperatively stops
   in-flight work; late results retain their identity and are discarded by the
   caller.

No provider performs inference today. The application, not `shadow-ai`, still
owns scheduling, process isolation, download UX, durable storage, and Recipe
integration.

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
contain neither model binaries nor implicit download artifacts. Apple delivery
should use Background Assets where product distribution fits and `URLSession`
for an application-owned content-addressed registry; deprecated
`MLModelCollection` is not a new architecture dependency. There is no package
manager, downloader, or signature verifier today.

A Core ML artifact set stores hashable downloaded blobs. When upstream ships a
directory `.mlpackage`, Shadow first records a reproducible archive or a
canonical per-file inventory, verifies extraction, then compiles it. The
resulting `.mlmodelc`, its stable-path cache, and per-device specialization may
be retained for speed but remain rebuildable and never replace source artifact
identity.

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
| [LaMa](https://github.com/advimman/lama/blob/main/LICENSE) | Source repository declares Apache-2.0; the currently linked checkpoint hosting/redistribution grant is not explicit enough | Side-load research prototype only until exact checkpoint rights and provenance are resolved |
| [NAFNet](https://github.com/megvii-research/NAFNet/blob/main/LICENSE) | MIT for NAFNet code; dependency terms remain separate | Candidate only after complete package audit |
| [Restormer](https://github.com/swz30/Restormer/blob/main/LICENSE.md) | MIT | Candidate after checkpoint/data audit |
| [LED](https://github.com/Srameo/LED/blob/main/LICENSE) | Repository code is CC BY-NC 4.0 and requires formal permission for commercial use; exact weights and datasets still need independent terms and provenance audits | Deferred from product/commercial distribution unless formal commercial permission covers the admitted artifacts; research reference only |
| [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN/blob/master/LICENSE) | BSD-3-Clause repository; exact weights/data still require audit | Generative/restorative comparison, never the fidelity default |
| [SwinIR](https://github.com/JingyunLiang/SwinIR/blob/main/LICENSE) / [HAT](https://github.com/XPixelGroup/HAT/blob/main/LICENSE) | Apache-2.0 repositories; exact weights/data still require audit | Classical SwinIR x2 is the first fidelity baseline; HAT remains measured follow-up |
| [FLUX.1 Fill dev](https://huggingface.co/black-forest-labs/FLUX.1-Fill-dev) | Black Forest Labs non-commercial terms | Deferred |
| [SUPIR](https://github.com/Fanghua-Yu/SUPIR) | Non-commercial upstream terms | Deferred |

Cloud APIs remain proprietary services. Their API documentation is technical
evidence, not approval of price, retention, training use, regional availability,
or data-processing terms. Legal/privacy review is repeated before each provider
is enabled.

### Privacy and upload scope

**Current:** every request and input artifact uses one of the canonical
`PrivacyClass` values from [`src/contract.rs`](src/contract.rs): `Public`,
`Personal`, or `SensitiveBiometric`. The separate remote admission path applies
`RemoteExecutionPolicy` from [`src/remote/admission.rs`](src/remote/admission.rs):
`Disabled`, `PublicOnly`, or `PersonalAllowed`. `SensitiveBiometric` remains
explicitly deferred. The deterministic local manifest planner does not inspect
remote consent and can never authorize a `RemoteApi` backend.

**Current contract:** an application-owned outbound store first strips and
encodes the allowed material, persists it, and returns a move-only prepared
receipt. That receipt binds the exact `AiJobRequest` source input to its
sanitized outbound hash, byte length, media type, upload scope, and raster
extent. Remote admission accounts the outbound lengths, not original source
lengths, and consumes the receipts into one expiring request grant. Upload
extent is not another `PrivacyClass`. RAW files, sensor mosaics, scene-linear
tiles, and frozen feature vectors have no remotely admissible scope.

Before any upload, the planned adapter must strip GPS and unrelated EXIF, apply
orientation, encode only the admitted raster and mask, estimate cost, and
enforce the configured ceiling. Full RAW upload remains prohibited by default.
Remote `SensitiveBiometric` inference is explicitly deferred pending a stronger
product, privacy, and legal decision.

No background task may upgrade a local job to remote execution. Runtime
validation rejects `RemoteApi` plans for local/system routes and rejects any
fallback whose selected route is remote. A remote primary may therefore fall
back only to a declared local/system route; it may not silently select another
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
2. Produce comparable distance evidence with Apple Vision
   [`VNFeaturePrintObservation`](https://developer.apple.com/documentation/vision/vnfeatureprintobservation)
   and its
   [distance contract](https://developer.apple.com/documentation/vision/vnfeatureprintobservation/computedistance(_:to:)),
   then form threshold-calibrated groups. Apple documents only that shorter
   distance means greater similarity; FeaturePrint is not a focus, expression,
   aesthetic, or “best photo” model. The current deterministic greedy
   complete-link partition and similarity medoid are review-navigation
   proposals, not quality ranking.
3. Add existing comparable technical evidence only when the complete analysis
   artifact and preprocessing revision match.
4. For eligible, burst-local face tracks, add Vision
   [face capture quality](https://developer.apple.com/documentation/vision/selecting-a-selfie-based-on-capture-quality)
   as a separate signal, never as an identity embedding. Apple limits the score
   to comparing captures of the same face and describes it as a holistic prior
   over lighting, blur, occlusion, expression, pose, focus, positioning, and
   other capture attributes—not as isolated sharpness evidence.

   Same-face correspondence must come from an explicitly admitted ephemeral
   track or user confirmation; the quality score cannot establish identity. A
   multi-face comparison is eligible only when every caller-designated face
   track is present and unambiguously corresponded in every candidate. Convert
   each track's raw scores to within-comparison percentile ranks, retain the raw
   values as evidence, then maximize the candidate tuple of lowest track
   percentile followed by median track percentile. This prevents one poor face
   from being hidden by several good ones without pretending scores from
   different people are directly comparable. Without correspondence, complete
   required track coverage, or at least one common eligible track, abstain from
   a multi-face score and ranking and expose only per-face evidence. Expression,
   pose, and occlusion remain opaque model priors that require subgroup review;
   they are not user intent or defect truth and must not be folded into
   FeaturePrint distance.
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

The UI may propose a group review start, a duplicate fold, or—only after
separate quality evidence is admitted—a ranked review order. It cannot hide a
manually protected or unique photo, write Reject, call a similarity medoid
“best”, or claim that one global aesthetic score represents the user's intent.
Each recommendation exposes its independent evidence categories and confidence.

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

## Model-generated object removal: local and remote routes

Clone and Heal remain source-directed deterministic correction tools.
Model-based inpaint is a separate capability because it synthesizes pixels and
may invent semantic content. A fixed local checkpoint can make execution
repeatable, but it does not make the output non-generated. Local versus remote
therefore changes privacy, cost, network, and deployment policy—not the
generated-raster provenance, visible disclosure, acceptance, undo, or comparison
contract.

### Local model route

[LaMa](https://github.com/advimman/lama) is a useful local large-mask inpaint
method reference. Its pixels are synthesized model output even without a text
prompt, so it may be evaluated only as a visibly generated, **side-loaded Object
Remove (Local)** prototype. The repository source license does not by itself
establish the hosted checkpoint's redistribution or data rights, so LaMa cannot
be bundled or automatically downloaded until the exact checkpoint, hosting
chain, training-data provenance, and notices pass admission.

The LaMa prototype must receive a bounded expanded context around the mask. Its
result must be composited only inside an explicitly expanded and feathered edit
region. Outside-region pixel invariance is a hard acceptance contract. The
eventual interaction must let the user compare, regenerate, discard, or accept
the proposal.

### Remote fill

Current callable providers should be evaluated behind the same adapter
contract, without confusing API availability with suitability as a default:

- Adobe Firefly Services
  [asynchronous API guide](https://developer.adobe.com/firefly-services/docs/firefly-api/guides/how-tos/using-async-apis/)
  lists Fill Image Async. Adobe's
  [changelog](https://developer.adobe.com/firefly-services/docs/firefly-api/getting-started/changelog/)
  records removal of the synchronous Fill Image v3 route in October 2025, so
  only the currently documented async route may enter a prototype;
- Black Forest Labs
  [FLUX.1 Fill](https://docs.bfl.ml/flux_tools/flux_1_fill).

Provider APIs and model names are volatile. The exact Adobe async endpoint,
schema, model/header revision, and BFL service/model endpoint must be selected
and revalidated when the adapter prototype starts; this shortlist is not a
frozen wire contract.

[Stability v2beta inpaint](https://platform.stability.ai/docs/api-reference) and
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

Stability's fast/conservative/creative upscale routes remain generated outputs:
even “conservative” accepts prompt/creativity controls and does not become a
fidelity default by name.

Every local or remote candidate version must record source revision, context
crop, mask digest, prompt, seed when available, provider/model version, request
identity, and output digest. Accepted pixels must be promoted to managed derived
storage before a Recipe can depend on them.

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

#### Mac-first closed-system RAW 9 benchmark

Apple's official
[WWDC26 Core Image RAW session](https://developer.apple.com/videos/play/wwdc2026/305/)
documents a callable, opt-in version 9 `CIRAWFilter` decoder on macOS 27. RAW 9
uses a tiled Core ML pipeline that jointly demosaics and denoises on the Apple
Neural Engine. This is a genuine sensor-mosaic route, not an RGB restoration
model, and therefore becomes the first closed-system true-RAW benchmark on
supported Macs.

Admission must first observe `supportedDecoderVersions` containing version 9
and verify the camera through `supportedCameraModels(for:)`. Apple says the
initial list covers hundreds of models across major vendors, native-DNG cameras
are supported automatically, and the list can change through OS over-the-air
updates. Consequently, “decoder 9” alone is not a reproducible route identity.
A future system-framework adapter and result receipt must also bind:

- normalized camera make/model and native-DNG status;
- exact OS build plus decoder version 9;
- a digest of the observed version-9 supported-camera list;
- the exact supported calibrated-property set for that filter instance;
- every applied calibrated value, including
  `luminanceNoiseReductionAmount`, exposure, sharpness, and contrast;
- scale factor, output extent/domain/color contract, and interactive versus
  export context policy.

RAW 9 removes or ignores some older controls, including the old color-noise,
detail, and moiré adjustments, so the adapter must query property support
instead of projecting Shadow controls by name. The current generic
`SystemFramework` identity is not sufficient for this camera- and
property-sensitive route; extending that identity is a prerequisite to linking
the adapter.

The benchmark must treat RAW 9 as one end-to-end demosaic/denoise/render route,
not claim that its denoise stage can be isolated. Compare cold first render,
warm parameter edits, full-resolution export, memory, power, color, detail,
noise, and camera coverage against Shadow's current deterministic RAW path.
Interactive evaluation follows Apple's documented fast path: reduced
`scaleFactor`, one caching `CIContext` per view, and direct Metal-backed
presentation. Export evaluation uses a non-caching context and records the
explicit memory limit. Unsupported cameras, pre-macOS-27 systems, or a changed
support snapshot fall back visibly to Shadow's existing pipeline.

RAW 9 remains closed, Apple-platform-only, OS-delivered, and outside Shadow's
weight, training-data, and update control. It is therefore a Mac-first product
candidate and comparison ceiling—not the universal Windows/Linux route and not
a substitute for the longer-term Shadow-owned model below.

#### Shadow-owned portable direction

Product-quality portable RAW denoise needs a Shadow-owned sensor-domain model
conditioned on:

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
or release evidence. Current RAW restoration challenges still treat
cross-camera real RAW denoise as an open problem: an SIDD RGB checkpoint or
one-camera low-light checkpoint must not be packaged as universal RAW denoise.
LED/PMN are research baselines only until their exact weights/data rights and
Shadow's CFA, phase, black/white-level, and noise-profile contract have been
evaluated.

Evaluation includes sensor-domain PSNR/SSIM, post-color Delta E 2000,
LPIPS/DISTS, MTF/detail retention, residual noise power spectrum, hot pixels,
banding, demosaic artifacts, and tile seams. Stars, fur, hair, text, skin, and
fine repeating texture explicitly test waxiness and hallucinated detail.

The first productizable step is therefore the runtime plus immutable benchmark
harness. RAW 9 can enter that harness immediately on supported macOS 27
hardware; the current deterministic RAW path remains its explicit fallback.
The Shadow-owned model stays a longer-term portable program rather than a
prematurely selected checkpoint. Neither route becomes default until it beats
the deterministic path on held-out cameras without unacceptable color shift,
texture loss, camera-domain regression, or support instability.

## 2x super-resolution

[SwinIR](https://github.com/JingyunLiang/SwinIR)'s classical x2 route is the
first conservative fidelity baseline, subject to exact checkpoint/data audit
and Core ML conversion. [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)
is a separate generative/restorative comparison because its adversarial
real-world prior can invent texture; it cannot be the fidelity default.
[HAT](https://github.com/XPixelGroup/HAT) is a measured follow-up when quality
gain justifies latency and memory.

The planned first product mode is **Fidelity 2x**:

- local and deterministic for a fixed model revision;
- export-time or explicitly requested, never blocking ordinary adjustment
  preview;
- tiled with overlap/halo and exact scale;
- run in the model's admitted display/RGB domain, not advertised as recovered
  scene-linear RAW truth.

A future **Creative Detail** route is separate, visibly generative, and disabled
by default. Adobe's current
[Upscale guide](https://developer.adobe.com/firefly-services/docs/firefly-api/guides/how-tos/upscale/)
documents the asynchronous `/v1/images/upsample-async` route. Its
[April 2026 GA changelog](https://developer.adobe.com/firefly-services/docs/firefly-api/getting-started/changelog/)
names `precise_upsampler_v1` as the only supported model-header value. This is a
remote reference, not a frozen integration: Shadow must reselect and record the
exact endpoint, schema, model/header, and service terms when the prototype
starts. Adobe's source-fidelity positioning and 2x guidance still require
held-out evaluation; larger scales require additional hallucination review.
Stability upscale is also generative. Google Imagen 4 upscale remains preview
and cannot define a stable release route. Topaz or another cloud service may
define a comparison ceiling, but no service becomes the default without the
same privacy, cost, provenance, retention, and held-out fidelity contract.

Evaluate 12, 24, and 45 megapixel sources using synthetic and real degradation.
Report PSNR/SSIM, LPIPS/DISTS, Delta E, ringing/halo measures, OCR correctness,
face-identity drift, false texture/hallucination audit, seams, P50/P95 export
time, peak memory, and energy. A 4x model is deferred until conservative 2x has
demonstrated value.

## Delivery sequence

### Phase 0: runtime and evidence foundation

1. **Current contract:** freeze provider plan/execute/cancel/progress values,
   stable route plus full-plan identity, lease-issued provenance envelope, and
   terminal receipt. **Remaining:** application scheduler, process isolation,
   and real provider execution.
2. Add signed model-package registry, exact artifact audit, download/side-load,
   and versioned eviction outside the repository.
3. Implement local worker isolation, resource leases, cancellation, crash
   recovery, and truthful fallback receipts.
4. Establish the Metal/CVPixelBuffer/Core ML resident-pixel path. On macOS 27,
   add a read-only Core Image RAW 9 capability probe that records decoder,
   camera-list, calibrated-property, and OS-build identity without yet changing
   the default decode route.
5. Build the benchmark harness, rights-cleared golden corpus, and immutable
   result format.
6. **Current contract:** consume only a lease-issued output, run promotion
   through an application managed-store authority, and reconstruct opaque
   managed authority only after exact-byte store verification. **Remaining:**
   implement that durable store plus its immutable Recipe reference before
   accepted generated pixels can enter a Recipe. Recipe-local vector/spatial
   mask revisions already exist and are not reimplemented by this phase.

No user-visible model feature should bypass this phase.

### Phase 1: bounded local assistance

1. Link and benchmark Vision FeaturePrint (the current provider is explicitly
   unavailable), face quality, and aesthetics with the existing culling evidence
   path; expose recommendations only after held-out gates pass.
2. Integrate SAM 2.1 Tiny as an editable promptable-mask prototype; compare
   Small and Vision routes.
3. Add deterministic tone, color, structure, geometry, and composition
   operators to the common conditional-mask contract.

### Phase 2: pixel-generating local tools

1. Link and benchmark Core Image RAW 9 on supported Macs as a closed,
   availability-gated system route; preserve the existing pipeline for
   unsupported cameras/platforms and as the cross-platform baseline.
2. Benchmark side-loaded LaMa local object removal only after checkpoint-rights
   audit.
3. Benchmark NAFNet and Restormer as RGB-domain denoise only.
4. Benchmark classical SwinIR x2 as the fidelity baseline, then compare
   HAT-S and generative/restorative Real-ESRGAN separately.
5. Promote only accepted outputs through managed derived storage and Recipe
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
- for OS-delivered system routes, decoder/request revision, normalized camera
  identity, supported-camera-list digest, supported calibrated-property digest,
  applied property values, and whether the system support snapshot changed;
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
  [face capture quality](https://developer.apple.com/documentation/vision/selecting-a-selfie-based-on-capture-quality),
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
- Apple Core Image RAW:
  [WWDC26: Explore the new Core Image RAW pipeline](https://developer.apple.com/videos/play/wwdc2026/305/).
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
  [LED repository and license](https://github.com/Srameo/LED/blob/main/LICENSE),
  [PMN](https://github.com/megvii-research/PMN),
  [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN),
  [SwinIR](https://github.com/JingyunLiang/SwinIR),
  and [HAT](https://github.com/XPixelGroup/HAT).
- Remote APIs:
  Adobe Firefly
  [async APIs](https://developer.adobe.com/firefly-services/docs/firefly-api/guides/how-tos/using-async-apis/),
  [changelog](https://developer.adobe.com/firefly-services/docs/firefly-api/getting-started/changelog/),
  and
  [Upscale guide](https://developer.adobe.com/firefly-services/docs/firefly-api/guides/how-tos/upscale/);
  [BFL FLUX.1 Fill](https://docs.bfl.ml/flux_tools/flux_1_fill),
  [Stability](https://platform.stability.ai/docs/api-reference),
  [Vertex Imagen editing](https://docs.cloud.google.com/vertex-ai/generative-ai/docs/image/edit-images-overview),
  [Qwen Image editing](https://help.aliyun.com/en/model-studio/qwen-image-edit-api),
  and [Topaz Image API](https://developer.topazlabs.com/image-api/available-models).
