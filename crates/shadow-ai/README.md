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
- An exact public RawNIND Bayer foundation route with verified installation,
  no-inference cache planning, bounded sidecar execution, portable
  `.shadowrawf` provenance, and atomic managed-store publication. The same
  planning contract can resolve a verified cache hit after application restart
  without consuming an execution lease or running inference.
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
  side-loading. Core ML `.mlpackage` source archives are identity; extracted
  packages, `.mlmodelc`, and device-specialized caches are rebuildable.
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
- A macOS SAM 2.1 Small Core ML process provider for side-loaded model packages.
  The one-shot route remains available for diagnostics; the product route owns
  a resident child, compiled models, and one rendered-image embedding so point
  refinements run only the prompt encoder and decoder. It validates the exact
  pinned nine-file artifact inventory, uses a bounded JSON-lines protocol,
  cooperatively cancels by terminating the child, and retries one complete
  request after a transport failure. Model download and redistribution remain
  outside Shadow.
- A fail-closed loopback client for infer-runtime's experimental YuNet and
  SFace routes. It accepts only path-free literal loopback-IP HTTP URLs,
  disables proxies and redirects, loads a regular owner-only credential file, validates
  the exact `input_pixels_no_exif_transform` coordinate contract, and rejects
  malformed model provenance or biometric responses. SFace vectors are fixed
  at 128 finite L2-normalized values in one exact embedding space; their Debug
  output is redacted and they have no serialization implementation.
- The same loopback client now exposes infer-runtime's experimental `SigLIP 2`
  image/text routes through a separate semantic owner. It requires normalized
  display orientation for JPEG/PNG inputs, sends explicit interactive or
  background priority, admits only finite normalized 768-dimensional cosine
  vectors, requires tokenizer provenance for text, and compares results only
  inside one exact versioned embedding space. The client does not own a photo
  index, retry checkpoint, or Catalog publication.
- [`infer_runtime/raw_foundation.rs`](src/providers/infer_runtime/raw_foundation.rs)
  owns the typed Shadow client for the frozen RawNIND execution split. A
  caller must complete Shadow's cache lookup first, then create an authenticated
  Job/ticket, pass one read-only Bayer staging handle and one empty writable
  output handle through the owner-only `SCM_RIGHTS` lease, and execute the
  one-shot lease against the exact daemon endpoint that issued it. Capability
  ids and the Unix socket path are redacted from Debug output; the client
  validates owner/mode/link/open flags, exact staging byte count, response
  digests, and the isolated ORT 1.27 Build pair. Shadow remains responsible for
  complete `.shadowrawf` verification, publication, cache identity, and stale
  result arbitration. The desktop bridge can select this route only through an
  explicit execution override; the legacy sidecar remains the default while
  the assembled Infer deployment is not active in the running daemon. The
  Windows named-pipe/HANDLE binding is not implemented, so the override fails
  closed there. The current Job API has no stable RAW percentage/tile progress;
  consumers may show only queued, running, and terminal states.
- [`infer_runtime/discovery.rs`](src/providers/infer_runtime/discovery.rs) owns
  Consumer endpoint selection independently from those typed routes. An
  explicit Shadow override wins, otherwise it validates the owner-only
  `infra.discovery.registration@20260812.1` persistent candidate registration,
  exact `infer-runtime.consumer@0.1.0-candidate.3` offer, generation, and
  canonical numeric-loopback endpoint. It rejects the removed lease field, the
  previous Discovery schema, and candidate.2-only offers; neither manifest
  presence nor modification time is treated as liveness. Explicit URLs and the
  fixed `http://127.0.0.1:8787` transport fallback use candidate.3 vocabulary as
  well. A connection failure re-runs discovery and accepts a changed generation
  or offer immediately. Remove the fixed fallback after every supported Infer
  Runtime publisher reliably registers a compatible candidate.3 offer.
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
hashes that exact input, and stages a rebuildable SAM proposal through the
resident Core ML provider. Foreground and background clicks can be refined
against the cached image embedding; applying the proposal promotes the packed
soft mask into the managed raster store before the Recipe references it.
Cancellation, stale-result rejection, reversible invert/opacity/feather
settings, and explicit apply/cancel boundaries remain application-owned.

The first people-analysis slice is operator-facing rather than a desktop
feature. `shadow-core` pages the Catalog's current Review visuals, verifies each
content-addressed JPEG, calls infer-runtime's YuNet detection and SFace
embedding endpoints, and rechecks the exact selected artifact before admitting
the result. It currently returns an in-memory anonymous grouping report through
`shadow-cli people-cluster`; embeddings, face observations, and clusters are
not persisted or synchronized. This deliberately postpones durable biometric
retention/deletion policy and user merge/split/name facts instead of silently
putting vectors into the ordinary Catalog.

## Deliberately not implemented

- No linked ONNX Runtime/Vision/CUDA/Metal/DirectML/Windows ML inference
  adapter. ONNX YuNet/SFace execution is delegated to the separately managed
  local infer-runtime; Core ML inference is isolated in the packaged SAM
  provider, and the Apple Vision module remains an availability-tested boundary only.
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
  owns shared loopback transport and face routes, while
  [`src/providers/infer_runtime/discovery.rs`](src/providers/infer_runtime/discovery.rs)
  owns Consumer endpoint discovery and migration fallback policy, and
  [`src/providers/infer_runtime/semantic.rs`](src/providers/infer_runtime/semantic.rs)
  owns `SigLIP 2` image/text request and response admission;
  [`src/providers/`](src/providers/) owns the remaining platform availability
  boundaries.
- [`../shadow-core/src/people_analysis.rs`](../shadow-core/src/people_analysis.rs)
  owns Catalog/cache selection, source-revision binding, stale-result rejection,
  and the bounded transient people-analysis workflow.
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
- [`src/providers/sam2_coreml_sidecar.rs`](src/providers/sam2_coreml_sidecar.rs)
  owns exact artifact admission and the one-shot provider adapter;
  [`src/providers/sam2_coreml_sidecar/resident.rs`](src/providers/sam2_coreml_sidecar/resident.rs)
  owns the recoverable resident session. The packaged native provider is
  documented in
  [`../../apps/desktop/providers/sam2-coreml/README.md`](../../apps/desktop/providers/sam2-coreml/README.md).
- Use the Mac-only
  [`SAM 2.1 Core ML probe`](../../tools/sam2-coreml-probe/README.md) only for
  lower-level conversion compatibility diagnosis.
- Keep application scheduling, persistence, Recipe integration, and UI ownership
  in their respective crates. This README should change only when the current
  crate boundary or implemented facts change.
