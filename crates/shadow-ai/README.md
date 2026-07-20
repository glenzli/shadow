# shadow-ai

`shadow-ai` is Shadow's model-independent AI foundation. It intentionally does
not load a model, download weights, inspect hardware, edit a photo, or access the
Catalog. Runtime scheduling and persistence remain application-layer
responsibilities; the first such adapter now lives in `shadow-core` and
`shadow-catalog` without coupling this crate to either one.

## What is implemented now

- Stable task/capability, input artifact, observation, provenance, confidence,
  privacy, and proposal-review contracts.
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

All model-derived data remains rebuildable. Human decisions, feedback events, and
accepted edit versions remain durable application facts. A feedback candidate may
also carry the exact encoded visual artifact and the normalized decoded-frame
receipt that were presented when the decision was made. This provenance is an
identity contract, not proof that two differently authored proxies are comparable.

## Current application integration

The import/decode path now sends the preferred cached JPEG visual to a dedicated
single-worker actor with a bounded queue. It verifies the content-addressed blob,
decodes a maximum-512-edge display-luma plane through `shadow-bridge`, runs the
deterministic observer, and asks Catalog schema v8 to commit only against the
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
- No model downloader, remote API call, Python runtime, or direct Catalog access
  from this crate.
- No cross-photo quality rank derived from the current display-proxy metrics.
- No fixed hardware-name assumptions for M1 Pro or RTX 4070 Ti.

## Decisions and evidence needed next

These do not block the contracts above, but should be decided with real fixtures
and the two target machines before an implementation is called usable:

1. **Feature baseline:** exact frozen image extractor, revision, ONNX export,
   preprocessing/color contract, vector dimension, throughput, and weight/data
   license. DINOv2-small is a candidate, not an implemented fact.
2. **Runtime packaging:** exact ONNX Runtime version/build, C API adapter boundary,
   Core ML and CUDA provider options, CPU thread policy, binary size, and update
   strategy. Each provider needs parity and fallback tests.
3. **First quality models:** face/eye detector and landmarks, supported photo
   domains, privacy defaults, false-reject behavior, and whether their weights may
   be redistributed or must be side-loaded.
4. **Empirical thresholds:** defect gate, confidence tiers, duplicate collapse,
   cold-start evidence ramp, and active-learning frequency. Current policy values
   are caller data; production defaults must come from held-out shoots.
5. **Resource calibration:** measured peak RSS/VRAM, execution time, battery
   behavior, unload latency, and safe concurrency on M1 Pro 32 GB and the Windows
   64 GB/RTX 4070 Ti machine.
6. **Quality gates:** shoot-disjoint datasets and acceptance metrics for grouping,
   false reject, top-k recall, NDCG, pairwise agreement, mask IoU/boundary quality,
   recipe acceptance/undo, and model-versus-rule improvement. A score without
   this evidence stays an observation.
7. **Worker protocol:** exact process isolation, shared-buffer handle format,
   cancellation/health/restart semantics, sandboxing, and model-file validation.
8. **Model distribution and licenses:** registry/signature format, upstream
   acceptance flow, China-reachable mirror/side-load UX, notices, and per-release
   audit of code, weights, and training-data terms.
9. **Recipe/mask integration:** typed edit-graph payloads, parameter safety bounds,
   coordinate spaces, immutable AI branches, and frozen generative patch
   provenance once the edit/recipe crates stabilize.
10. **Remote providers:** whether any BYOK API is worth supporting, supported
    regions, crop-only upload policy, cost estimate/ceiling, deletion/privacy
    guarantees, and deterministic local fallback. Remote use remains off by
    default.

The next AI slice should first freeze a comparable analysis-artifact contract and
benchmark it on real shoots, then benchmark one pinned frozen embedding model and
runtime behind an isolated worker. The exact extractor, weights, preprocessing,
licenses, and ONNX Runtime/Core ML/CUDA packaging remain decisions, not implied
dependencies. Only after the comparability, quality, and resource gates pass
should group ranking or personal preference affect UI recommendations.
