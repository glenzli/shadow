# shadow-ai

`shadow-ai` is Shadow's model-independent AI foundation. It intentionally does
not load a model, download weights, inspect hardware, edit a photo, or access the
Catalog. Runtime scheduling and persistence remain application-layer
responsibilities; the first such adapter now lives in `shadow-core` and
`shadow-catalog` without coupling this crate to either one.

Use this file for the crate's current facts and navigation. The staged,
evidence-gated route from these contracts to real culling, masks, restoration,
denoise, and super-resolution providers lives in
[`AI_CAPABILITY_PLAN.md`](AI_CAPABILITY_PLAN.md). That plan describes candidates
and gates, not implemented inference.

## What is implemented now

- Stable task/capability, input artifact, observation, provenance, confidence,
  privacy, and proposal-review contracts.
- Typed subject-mask and denoise requests plus validated generated-artifact
  outputs. A soft mask records both its stored raster extent and the image
  coordinate extent it maps to. A denoise output records its exact source pixel
  contract, image domain, layout, sample format, tiling halo, and whether it is
  full resolution.
- A storage-class boundary for generated pixels: workers emit rebuildable
  proposals, while the application must promote an accepted mask or denoised
  raster to managed derived storage before a Recipe can depend on it. Generated
  pixels are never intended to live as SQLite blobs or ordinary evictable
  thumbnail cache entries.
- A strict model manifest covering exact artifact revision/hash, tensor I/O,
  preprocessing, execution targets, RAM/VRAM, code/weight/data license notes,
  redistribution, gating, and side-loading.
- Deterministic resource admission against a supplied Mac/Windows hardware
  snapshot. It respects RAM reserve, device-memory headroom, session limits,
  CPU threads, battery state, remote policy, and biometric privacy.
- Explainable group-relative selection scoring. Severe defects are a safety gate;
  technical quality, general prior, personal preference, and uniqueness remain
  separate signals. Unique or manually protected photos are never hidden by the
  gate. The API can propose or fold duplicates, but cannot delete originals or
  write Reject decisions.
- Append-only feedback events that record the candidates the user actually saw.
  Explicit pairwise choices can become incremental examples; absence of a click,
  export, dwell time, and other ambiguous behavior are not silently turned into
  negatives.
- Deterministic CPU observations over a bounded, normalized display-luma plane:
  a fixed histogram, exact nearest-rank percentiles, near-black/near-white
  fractions, and two explicitly defined sharpness proxies. These measurements
  describe the supplied display proxy only; they are not RAW exposure readings,
  aesthetic scores, or automatic Pick/Reject decisions.
  The application does not currently compare sharpness across photos. Matching
  proxy scale and exact preprocessing revision is a minimum precondition, not
  proof of comparability; upstream resizing or sharpening changes their meaning.
- A small deterministic Bradley-Terry/logistic linear preference head over frozen
  feature vectors. It is a real, serializable CPU update path, but it is not a
  substitute for the still-unselected image feature extractor.

Unaccepted model-derived data remains rebuildable. Human decisions, feedback
events, and accepted edit versions are durable application facts today. The
generated-artifact contract additionally requires any future accepted generated
dependency to be promoted to managed derived storage before a Recipe can refer
to it; that storage and promotion path is not implemented yet. The current
application stores manual Pick/Reject/rating transitions in a separate immutable
Catalog ledger, but does not expose that ledger as AI training data or grant
models write access to it. A feedback candidate may also carry the exact encoded
visual artifact and the normalized decoded-frame receipt that were presented
when the decision was made. This provenance is an identity contract, not proof
that two differently authored proxies are comparable.

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

## Deliberately not implemented

- No ONNX Runtime/Core ML/CUDA/Metal/DirectML/Windows ML adapter.
- No DINO, CLIP, face/eye, SAM, depth, inpaint, diffusion, VLM, or LLM model.
- No fabricated quality score, embedding, mask, recipe, or generated patch.
- No persistent mask raster store or managed derived-raster store yet. The
  generated-artifact contracts define the promotion boundary but do not pretend
  the storage or renderer exists.
- No model downloader, remote API call, Python runtime, or direct Catalog access
  from this crate.
- No cross-photo quality rank derived from the current display-proxy metrics.
- No fixed hardware-name assumptions for M1 Pro or RTX 4070 Ti.

## Next navigation

- Follow the source modules from [`src/lib.rs`](src/lib.rs) when changing a
  current contract.
- Use [`AI_CAPABILITY_PLAN.md`](AI_CAPABILITY_PLAN.md) when selecting or
  integrating a runtime, model package, culling feature, mask generator,
  restoration provider, denoiser, or super-resolution route.
- Keep application scheduling, persistence, Recipe integration, and UI ownership
  in their respective crates. This README should change only when the current
  crate boundary or implemented facts change.
