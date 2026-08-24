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

Status: transport and independent LibRaw sample round-trip implemented; RawTherapee and isolated
Darktable normalized-DNG child execution is available, while the vkdt child matrix remains in
progress.

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

The bridge is not accepted merely because a DNG opens. The current adapter proves byte-identical
active samples through LibRaw `unprocessed_raw`, identical CFA positions, exact Shadow metadata in
`DNGPrivateData`, and records the standard-tag calibration projection. Nikon HE* and Sony real-RAW
smokes both passed the byte-exact sample proof. Engines that alter samples or auto-apply a profile
stay classified as independent pipelines.

Standard DNG cannot express four separate physical white levels for a one-sample CFA IFD. The
interoperable `WhiteLevel` is therefore the exact common value when all sites agree, otherwise the
conservative minimum. The exact four-site values remain in the private descriptor and the run must
not call the standard projection calibration-equivalent in the unequal case.

Remaining acceptance:

- run RawTherapee, Darktable, and vkdt from the generated DNG under pinned profiles/graphs;
- keep the implemented fail-closed parent manifest and normalized-DNG artifact identity receipt;
- distinguish byte-preserving import, metadata projection, and complete reconstruction output;
- add the remaining vkdt normalized child once its pinned CLI build is available.

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
