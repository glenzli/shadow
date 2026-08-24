# RAW oracle roadmap

This roadmap keeps research instrumentation separate from production admission. A phase may produce
useful oracle evidence without authorizing its algorithm in Shadow's interactive source path.

## Phase 1 — executable decode and reconstruction matrix

Status: implemented in `oracle_lab.py`.

Acceptance:

- one immutable run directory outside the source checkout;
- no copied RAW payload and no source/run path leakage in command logs;
- executable, profile, graph, source, and artifact SHA-256 identities;
- a strict same-`RawFrame` Shadow/Darktable-opposed comparison;
- isolated independent LibRaw, RawTherapee, controlled-XMP Darktable, and vkdt adapters;
- unavailable decoders recorded without fallback substitution;
- owner tests using no real photo payload.

Local availability is allowed to be partial. Missing upstream binaries do not weaken the manifest;
they remain explicit `unavailable` adapter results until separately built or installed.

## Phase 2 — normalized mosaic interchange

Goal: make more reconstruction engines consume equivalent decoded sensor evidence.

Proposed boundary:

```text
Shadow RawFrame
  -> canonical active Bayer mosaic + exact metadata manifest
  -> lossless research DNG
  -> independent decoder import
  -> pre-demosaic sample/metadata round-trip verification
  -> reconstruction oracle
```

Required metadata:

- active-area origin/extent and stored dimensions;
- exact Bayer phase and orientation;
- uint16 sample byte order with no tone or gamma transform;
- per-colour black and physical-white levels;
- as-shot neutral and camera-to-XYZ calibration;
- source/provider identity and pending opcode declarations.

The bridge is not accepted merely because a DNG opens. It must prove byte-identical active samples,
identical CFA positions, and numerically equivalent calibration after re-import. Engines that alter
samples or auto-apply a profile stay classified as independent pipelines.

## Phase 3 — versioned objective comparison

Add a separate analysis owner that consumes linear outputs and emits:

- registered linear crops and an explicit exposure/white normalization receipt;
- per-channel and luminance difference maps;
- clipped-core, boundary-band, and reliable-exterior masks;
- false-colour exterior area;
- boundary hue/chroma discontinuity;
- luminance slope/curvature across the boundary;
- runtime, artifact size, and peak-memory evidence.

Metrics must be reported by region and must never hide a worse boundary behind one whole-image
average. Visual contact sheets are secondary evidence derived from the registered linear outputs.

## Phase 4 — fixture admission and production candidates

Curate a small, locally licensed matrix covering:

- Nikon HE/HE* large smooth clipping;
- the lamp/fixture edge with a dark neighbouring object;
- Sony sun disc and gradient;
- specular highlights;
- saturated single-colour emitters;
- ordinary unclipped controls.

For each candidate algorithm, first state:

- exact write ownership and context-gathering radius;
- reusable upstream state and recomputation frontier;
- CPU/GPU residency and transfers;
- cache identity impact;
- cancellation behavior;
- preview/detail/export equivalence.

Only then may the production owner evaluate a change. An oracle that requires a slow multiscale
pass may still be valuable as a one-time RAW source preparation, but it must not migrate into every
slider event or create a preview/detail semantic fork.
