# shadow-ai

`shadow-ai` is Shadow's model-independent AI foundation. It intentionally does
not load a model, download weights, inspect hardware, edit a photo, or write the
Catalog. Those responsibilities live behind later worker/provider and application
boundaries.

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
  Sharpness proxies are only comparable at the same proxy scale and exact
  preprocessing revision; resizing or sharpening changes their meaning.
- A small deterministic Bradley-Terry/logistic linear preference head over frozen
  feature vectors. It is a real, serializable CPU update path, but it is not a
  substitute for the still-unselected image feature extractor.

All model-derived data remains rebuildable. Human decisions, feedback events, and
accepted edit versions remain durable application facts.

## Deliberately not implemented

- No ONNX Runtime/Core ML/CUDA/Metal/DirectML/Windows ML adapter.
- No DINO, CLIP, face/eye, SAM, depth, inpaint, diffusion, VLM, or LLM model.
- No fabricated quality score, embedding, mask, recipe, or generated patch.
- No model downloader, remote API call, Python runtime, or Catalog schema write.
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

The intended next vertical slice is modest: connect these rule-based display-luma
observations to versioned proxy artifacts, persist their provenance, and benchmark
one pinned frozen embedding model behind a worker. Only after it passes the
quality/resource gates should personal preference affect UI recommendations.
