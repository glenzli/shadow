# shadow-ai

`shadow-ai` is Shadow's model-independent AI foundation. It intentionally does
not load a model, download weights, edit a photo, or access the Catalog. It now
owns provider-neutral runtime and promotion protocols, but runtime scheduling,
native framework linkage, package installation, durable storage, and
persistence remain application-layer responsibilities.

Use this file for the crate's current facts and navigation. The staged,
evidence-gated route from these contracts to real culling, masks, restoration,
denoise, and super-resolution providers lives in
[`AI_CAPABILITY_PLAN.md`](AI_CAPABILITY_PLAN.md). That plan describes candidates
and gates, not implemented inference.

## What is implemented now

- Stable task/capability, input artifact, observation, provenance, confidence,
  privacy, and proposal-review contracts. `AiJobRequest` is application intent:
  provider, model, checkpoint, framework, and remote-service identity enter only
  after admission. A successful lease is the only normal constructor for an
  `AiObservation`: its exact-v1 envelope copies request/task/target identity
  from runtime provenance, rejects cross-field substitution, and stream-bounds
  explanation evidence to 64 signals.
- Exact admitted route identities for local artifact sets, system-framework
  request revisions/OS builds, and remote services, plus an exact identity of
  the complete admitted backend/precision/thread/memory plan. A move-only
  execution lease supports cooperative cancellation, monotonic progress,
  complete route-and-plan matching, explicit fallback disclosure, measured
  usage, and one terminal receipt per consumed lease. A provider returns only
  its payload; the lease wraps success in a move-only envelope whose provenance
  binds the complete request, inputs, route, and plan. Local/system routes can
  never bind a remote backend, and a fallback can never select a remote route.
  The application scheduler still owns lease uniqueness and revocation.
- Typed subject-mask and denoise requests plus validated generated-artifact
  outputs. A soft mask records both its stored raster extent and the image
  coordinate extent it maps to. A denoise output records its exact source pixel
  contract, image domain, layout, sample format, tiling halo, and whether it is
  full resolution.
- An Infer Runtime RawNIND Bayer foundation consumer with cache-before-runtime
  resolution, typed handle leases, portable `.shadowrawf` provenance, and
  atomic managed-store publication. The same cache identity can resolve a
  verified hit after restart without admitting runtime work.
- A promotion boundary for generated pixels: workers emit rebuildable byte
  identities, while promotion consumes the runtime-issued successful-output
  envelope and runs through an application-owned managed-store transaction.
  Only that durability authority can return the exact verified commit consumed
  by the transaction. Managed raster and artifact authority values are
  move-only and non-deserializable; a persisted descriptor reconstructs
  authority only after the store verifies its object identity and bytes.
  Generated pixels are never intended to live as SQLite blobs or ordinary
  evictable thumbnail cache entries. `shadow-core` now implements the
  application-owned filesystem authority, and `shadow-domain` owns the exact
  immutable Recipe reference for accepted soft-mask bytes.
- A strict local model manifest covering one exact multi-blob artifact set,
  whose identity is a domain-separated, length-prefixed BLAKE3 digest of the
  canonical-path-sorted inventory. Before hashing, its portable ASCII path
  grammar rejects traversal, drive/UNC/ADS syntax, Windows device names,
  leading/trailing-space aliases, short-name `~` aliases, case aliases, and
  Unicode normalization ambiguity. Installed availability names that exact
  identity rather than a verification boolean. Exact-v1 deserialization is
  recursive, rejects unknown fields and duplicate sets, and stream-bounds all
  manifest/request vectors.
  The manifest also covers tensor I/O, preprocessing, execution targets,
  RAM/VRAM, code/weight/data license notes, redistribution, gating, and
  side-loading. Infer Runtime owns all model package and device-cache identity;
  Shadow does not stage an executable local model format.
- A separate remote-provider manifest covering rendered-RGB/mask upload scope,
  privacy, retention, training use, terms revision, offline behavior,
  idempotency, and cancellation. An application store must first sanitize,
  encode, persist, and verify each outbound object; its opaque move-only receipt
  binds source identity to outbound hash, byte length, media type, and extent.
  Remote admission accounts those outbound bytes and produces an opaque,
  expiring, move-only grant bound to the complete request, manifest/legal facts,
  consent/policy revision, exact receipt inventory, and stable idempotency key.
  Validated receipts are canonicalized by request input index before both grant
  identity and transport inventory are created, so caller ordering is irrelevant.
  RAW, sensor-mosaic, scene-linear, and frozen-feature inputs are represented
  locally but always rejected by remote admission.
- Deterministic resource admission against a supplied Mac/Windows hardware
  snapshot. It budgets RAM reserve, device-memory headroom, session limits, and
  CPU threads. This local-manifest planner always rejects `RemoteApi`; only the
  separate remote manifest, prepared-upload, consent, and grant path may
  authorize remote transport. Scratch disk, upload bytes, and duration remain
  metadata for their own authorities and are not falsely recorded as `RunPlan`
  reservations.
- Explainable group-relative selection scoring. Severe defects are a safety gate;
  technical quality, general prior, personal preference, and uniqueness remain
  separate signals. Unique or manually protected photos are never hidden by the
  gate. The API can propose or fold duplicates, but cannot delete originals or
  write Reject decisions.
- Complete feature-print distance evidence plus a deterministic greedy
  complete-link partition and similarity-medoid review start. This path never
  calls the medoid “best”, never hides a protected candidate, and keeps
  technical quality, face capture quality, aesthetics, and preference separate.
  The Apple Vision route pins FeaturePrint request revision 1 and the OS build,
  but the current provider is deliberately unlinked and returns `unavailable`
  rather than placeholder distances.
- Append-only feedback events that record the candidates the user actually saw.
  Explicit pairwise choices can become incremental examples; absence of a click,
  export, dwell time, and other ambiguous behavior are not silently turned into
  negatives.
- Deterministic CPU observations over a bounded, normalized display-luma plane:
  a fixed histogram, exact nearest-rank percentiles, near-black/near-white
  fractions, and two explicitly defined sharpness proxies. These measurements
  describe the supplied display proxy only; they are not RAW exposure readings,
  aesthetic scores, or automatic Pick/Reject decisions.
- Provider-neutral semantic-image contracts now admit bounded, redacted,
  L2-normalized image/text embeddings only inside one exact versioned space.
  A separate structured-analysis proposal carries at most 64 canonical scene,
  object, activity, or attribute suggestions plus one bounded language-tagged
  short caption. It deliberately has no confidence-to-commit path and cannot
  create an `AiAccepted` Library keyword without a later user action.
  The application does not currently compare sharpness across photos. Matching
  proxy scale and exact preprocessing revision is a minimum precondition, not
  proof of comparability; upstream resizing or sharpening changes their meaning.
- A small deterministic Bradley-Terry/logistic linear preference head over frozen
  feature vectors. It is a real, serializable CPU update path, but it is not a
  substitute for the still-unselected image feature extractor.
- A typed Infer Runtime SAM 2.1 soft-mask Consumer. Shadow verifies the native
  256×256 Gray8 probability raster and keeps only product-side staging and
  apply/discard authority; Infer Runtime owns the user-installed model,
  compiled cache, resident worker, and cancellation. Shadow has no local SAM
  provider, model inventory, or subprocess fallback. Model download and
  redistribution remain outside Shadow.
- An adapter over the official `infer-runtime-client` SDK at Git revision
  `8588a945047cedaea62035969e479e7fb7ff795c` for YuNet and SFace. The SDK owns
  strict `infra.discovery.registration@20260812.1` selection, exact
  `infer-runtime.consumer-core@20260813.1` and Capability Catalog negotiation,
  managed owner-only credentials, loopback HTTP, no-proxy/no-redirect policy,
  generation reconnect, headers, and public errors. Shadow validates
  the exact `input_pixels_no_exif_transform` coordinate contract, and rejects
  malformed model provenance or biometric responses. SFace vectors are fixed
  at 128 finite L2-normalized values in one exact embedding space; their Debug
  output is redacted and they have no serialization implementation.
- The same official SDK exposes infer-runtime's typed `SigLIP 2` image/text and
  Qwen image-understanding routes through separate Shadow evidence owners. They require normalized
  display orientation for JPEG/PNG inputs, sends explicit interactive or
  background priority, admits only finite normalized 768-dimensional cosine
  vectors, requires tokenizer provenance for text, and compares results only
  inside one exact versioned embedding space. The client does not own a photo
  index, retry checkpoint, or Catalog publication.
- [`infer_runtime/raw_foundation.rs`](src/providers/infer_runtime/raw_foundation.rs)
  owns the typed RawNIND request/cache identity, capability probe, handle lease,
  execution, cancellation, and provenance boundary. Shadow has no local
  RawNIND sidecar or generic RAW transport fallback.
- [`infer_runtime.rs`](src/providers/infer_runtime.rs) is now only Shadow's
  synchronous product/evidence adapter over the SDK. Product endpoint selection
  has no fixed-port fallback and no candidate-contract branch. An explicit
  literal-loopback endpoint remains available solely as a development or
  diagnostic override.
- Deterministic anonymous-person candidate grouping over request-local SFace
  evidence. It uses conservative complete-link grouping, never compares
  different embedding spaces, never groups two co-occurring faces from the
  same photo, and emits only occurrence references. The result is an anonymous
  review proposal, not a named identity or recognition claim.

Unaccepted model-derived data remains rebuildable. Human decisions, feedback
events, and accepted edit versions are durable application facts today. An
accepted soft mask must now pass the implemented store-authority transaction
before it can become a `managed_raster` Recipe definition. That immutable
reference records the canonical object identity, exact BLAKE3 and byte length,
stored extent, coordinate extent, and sample encoding; reopening it re-hashes
the managed bytes before returning a file handle. The current application stores
manual Pick/Reject/rating transitions in a separate immutable Catalog ledger,
but does not expose that ledger as AI training data or grant models write access
to it. A feedback candidate may also carry the exact encoded visual artifact and
the normalized decoded-frame receipt that were presented when the decision was
made. This provenance is an identity contract, not proof that two differently
authored proxies are comparable.

## Current application integration

The import/decode path now sends the preferred cached JPEG visual to a dedicated
single-worker actor with a bounded queue. It verifies the content-addressed blob,
decodes a maximum-512-edge display-luma plane through `shadow-bridge`, runs the
deterministic observer, and asks the Catalog to commit only against the
exact representation fingerprint, cached-artifact identity, observation schema,
implementation version, luma contract, and preprocessing revision. Stale jobs
are discarded. Invalid persisted observation data is treated as rebuildable and
does not make the Review page unreadable.

Review exposes a compact summary for the selected photo while retaining the full
histogram in Catalog. This remains a single-photo factual inspection path: it
does not rank a group, add quality badges, or compare observations made from
differently generated, resized, or sharpened proxies. An embedded camera preview
and a Shadow-generated proxy can have materially different upstream processing
even when the final JPEG-to-luma decoder revision matches.

The desktop Compare path freezes an exact cached-artifact record in a
session-authenticated handle, exchanges it for one-purpose presentation tickets,
and loads those content-addressed bytes without reselecting the current preferred
artifact. Qt normalizes the decoded image to unpremultiplied row-major RGBA8888 and
returns a SHA-256 frame receipt before the presentation can be confirmed or the
human outcome recorded. A later preferred-artifact change therefore does not
rewrite the evidence. The receipt intentionally stops before display ICC, GPU
sampling, compositing, and the physical screen. The accompanying technical
observation is displayed separately and is not yet copied into the feedback event.

The desktop AI Mask path renders a bounded JPEG from the active edit session,
hashes that exact input, and stages a rebuildable SAM proposal through Infer
Runtime. Foreground and background clicks can be refined
against the cached image embedding; applying the proposal promotes the packed
soft mask into the managed raster store before the Recipe references it.
Cancellation, stale-result rejection, reversible invert/opacity/feather
settings, and explicit apply/cancel boundaries remain application-owned.

People analysis is exposed in the desktop People workspace and through
`shadow-cli people-cluster`. `shadow-core` verifies current Review JPEGs, calls
Infer Runtime detection and embedding capabilities, and rejects stale inputs.
The desktop owns explicit consent and a `PeopleLibraryStore` for anonymous
photo references, groups, representative thumbnails, names, and merges. Face
embedding vectors remain transient. Clearing people data removes the retained
people artifacts without changing original photos or edits. Split corrections
and durable correction history are not yet implemented.

`shadow-core/src/vision_input.rs` prepares bounded local model inputs: semantic
search uses a 2048-pixel maximum edge and face analysis uses 4096. Oversized
embedded previews are resized with aspect ratio preserved; original photos and
Catalog geometry are unchanged. Request identity includes the preparation policy.

Semantic search uses cancellable requests and rebuildable image-vector records
keyed by exact source revision, geometry, and embedding space. New text queries
reuse valid image vectors. Desktop scanning uses an explicit coverage bound and
shows skipped/truncated coverage; relative similarity is not a calibrated
confidence score. The CLI keeps a smaller caller-controlled default budget.

## Deliberately not implemented

- No linked ONNX Runtime/Vision/CUDA/Metal/DirectML/Windows ML inference
  adapter. YuNet/SFace, SAM, and RawNIND execution are delegated to the
  separately managed local Infer Runtime; the Apple Vision module remains an
  availability-tested boundary only.
- No DINO, CLIP, depth, inpaint, diffusion, VLM, or LLM model inside Shadow.
- No fabricated quality score, embedding, mask, recipe, or generated patch.
- No bundled/downloaded model package and no promise that an arbitrary SAM
  conversion is compatible. Admission requires the exact pinned artifact set.
- No model downloader, cloud API call, Python runtime, or direct Catalog access
  from this crate. The infer-runtime adapter is restricted to loopback HTTP.
- No cross-photo quality rank derived from the current display-proxy metrics.
- No fixed hardware-name assumptions for M1 Pro or RTX 4070 Ti.

## Next navigation

- [`src/contract.rs`](src/contract.rs) owns provider-neutral task intent and
  result evidence.
- [`src/semantic.rs`](src/semantic.rs) owns image/text embedding-space safety,
  bounded keyword suggestions, and short-caption proposal validation.
- [`src/runtime/`](src/runtime/) owns route identity, admission, leases,
  progress/cancellation, runtime-issued provenance, and terminal receipts.
- [`src/manifest.rs`](src/manifest.rs) owns local artifact sets;
  [`src/remote/manifest.rs`](src/remote/manifest.rs) owns remote service facts
  [`src/remote/upload.rs`](src/remote/upload.rs) owns prepared outbound-store
  receipts, and [`src/remote/admission.rs`](src/remote/admission.rs) owns the
  exact request/grant gate.
- [`src/culling.rs`](src/culling.rs) owns similarity-only grouping evidence;
  [`src/people.rs`](src/people.rs) owns anonymous-person grouping and sensitive
  SFace value admission; [`src/providers/infer_runtime.rs`](src/providers/infer_runtime.rs)
  owns the synchronous Shadow evidence adapter over the official SDK, while
  [`src/providers/infer_runtime/semantic.rs`](src/providers/infer_runtime/semantic.rs)
  owns `SigLIP 2` image/text request and response admission;
  [`src/providers/`](src/providers/) owns the remaining platform availability
  boundaries.
- [`../shadow-core/src/people_analysis.rs`](../shadow-core/src/people_analysis.rs)
  owns Catalog/cache selection, source-revision binding, stale-result rejection,
  and the bounded, cancellable people-analysis proposal workflow.
- [`src/generated.rs`](src/generated.rs) owns typed generated outputs and
  [`src/derived_raster.rs`](src/derived_raster.rs) owns the managed-store
  promotion transaction boundary.
- [`../shadow-core/src/derived_raster_store.rs`](../shadow-core/src/derived_raster_store.rs)
  owns durable proposal promotion and verified reopening;
  [`../shadow-domain/src/recipe/local_mask/managed_raster.rs`](../shadow-domain/src/recipe/local_mask/managed_raster.rs)
  owns the immutable Recipe reference and packed soft-mask byte shape.
- Use [`AI_CAPABILITY_PLAN.md`](AI_CAPABILITY_PLAN.md) when selecting or
  integrating a runtime, model package, culling feature, mask generator,
  restoration provider, denoiser, or super-resolution route.
- [`src/providers/infer_runtime/subject_mask.rs`](src/providers/infer_runtime/subject_mask.rs)
  owns typed SAM capability consumption and product-side result admission.
- Keep application scheduling, persistence, Recipe integration, and UI ownership
  in their respective crates. This README should change only when the current
  crate boundary or implemented facts change.

## Personal learning evidence

[`feedback/readiness.rs`](src/feedback/readiness.rs) inventories a bounded page of explicit
pairwise and approved-edit references. It reports missing frozen features and excludes forgotten
or out-of-scope events; it does not load features, render Recipes, authorize learning or train a
model. `EditExampleConfirmed` binds an operator-approved baseline and result to immutable Recipe
commits, with declared manual/imported/assisted/mixed/unknown origin and style/correction intent.
Catalog validates both commits belong to the named photo. Equal commits can explicitly mean no
adjustment was wanted. Autosave and export are not implicit approvals. The Catalog page resolves
Recipe integrity and revocations in one read snapshot; source and feature verification remain
separate admission requirements. Older event JSON is unchanged; older binaries do not necessarily
understand the new action.

The [personal learning design](../../docs/architecture/personal-learning.md) distinguishes this
operator/API foundation from future desktop capture, dataset manifests, training and model rollout.
