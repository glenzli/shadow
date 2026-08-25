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

Status: implemented. RawTherapee, isolated Darktable, and pinned vkdt normalized-DNG child
execution are available. The vkdt route has a strict hilite on/off graph ablation and has completed
host-GPU runs for Nikon HE*, the lamp edge, and Sony sun fixtures.

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

Accepted boundaries:

- run RawTherapee, Darktable, and vkdt from the generated DNG under pinned profiles/graphs;
- keep the implemented fail-closed parent manifest and normalized-DNG artifact identity receipt;
- distinguish byte-preserving import, metadata projection, and complete reconstruction output;
- record the exact vkdt executable, graph, MoltenVK ICD, and non-flipped PFM row convention.

## Phase 3 — versioned objective comparison

Status: implemented in `linear_image.py`, `cfa_topology.py`, and `objective_metrics.py`.

The separate analysis owner consumes explicitly declared linear or sRGB outputs and emits:

- registered linear crops and an explicit exposure/white normalization receipt;
- per-channel and luminance difference maps;
- clipped-core, boundary-band, and reliable-exterior masks;
- false-colour exterior area;
- boundary hue/chroma discontinuity;
- luminance slope/curvature across the boundary;
- runtime, artifact size, and peak-memory evidence.

Metrics must be reported by region and must never hide a worse boundary behind one whole-image
average. Visual contact sheets are secondary evidence derived from the registered linear outputs.

Same-pipeline ablations use identity normalization. Cross-pipeline exploratory comparisons may use
declared reliable-exterior RGB normalization, but their differences remain complete-pipeline
evidence. PFM orientation is part of the receipt because pinned vkdt emits top-down, non-flipped
PFM rows rather than the standard bottom-up convention.

The analysis can now replace the manual core with a factual mask derived from byte-exact active CFA
values and four-site physical whites. It preserves response-shoulder and shared-three-colour masks
as separate facts, verifies staging and topology payload identities, applies RawFrame orientation,
and fails when image dimensions would require implicit resampling. A decoder that removes a known
sensor border may declare one exact integer topology-to-reference crop; the receipt records the
source rectangle, output dimensions, and `resampling=false`. Manual core rectangles remain only as
an explicit compatibility fallback.

## Phase 4 — fixture admission and production candidates

Status: oracle admission/evaluation infrastructure implemented; no production candidate admitted.

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

The external local catalog admits three named RAW files in place and six cases covering all required
classes. Source size/SHA auditing, immutable region export, and complete per-case topology analyses
have completed. CFA projection proved that the original Nikon "unclipped cloud" control contains
617,792 physical-white samples, and the topology-required gate correctly rejected it. Matrix
`20260825.5` retains the visually reviewed replacement containing zero physical-white samples and
declares six candidates: vkdt hilite, both RawTherapee modes, Darktable opposed, LibRaw H2 blend,
and LibRaw H3 rebuild. Its source audit and all topology-required candidate evaluations pass; the
immutable catalog identity is `fixture-matrix-20260825e.json` at SHA-256
`fe86bb9151615c6e746bcefc42b4b3ea06e6738071cccfc427c8a070ccdc6a42`.
All six candidate-evaluation receipts are retained in the external local catalog's
`evaluations/20260825.5/` directory and bind that exact identity.

Each candidate explicitly owns only external oracle output, uses full-frame context, and makes no
Shadow preview/detail/export equivalence claim. Relative to each engine's disabled baseline,
RawTherapee Coloropp writes broadly across the Nikon smooth-sky control (about 43.78%), while Color
propagation is materially more selective (about 0.65%). Both modes still write roughly 4--5% in the
lamp cases and 5.06% around the Sony sun disc. These are write-footprint diagnostics, not a quality
ranking. The controlled Darktable opposed pass is selective on the lamp and Nikon cases but changes
about 7.66% of the Sony sun-disc reliable exterior. LibRaw H2/H3 also modify 5.10% of the repaired
unclipped control and broad majorities of several bright cases, so they remain negative evidence.
The next production-facing study should compare Shadow's strict same-`RawFrame` opposed result
against the selective Darktable cases, then isolate which clip-mask, white-balance, or replacement-
ownership differences explain the remaining Sony behavior. The receipts still show substantial
offline working memory, so the correct outcome remains research evidence rather than automatic
production admission.

The controlled Darktable runs also expose one interchange follow-up: RawSpeed reports that the
research DNG lacks a standard camera `Make`. This does not invalidate the same-input clip/opposed
delta, but it blocks treating Darktable's full colour rendering as a camera-profile reference until
the normalized-DNG metadata owner adds and independently verifies standard camera identity.

## Phase 5 — strict production/reference attribution

Status: repeatable alignment and threshold-domain ablation receipts implemented; no production
threshold change admitted.

`strict_alignment.py` consumes successful `shadow-cfa-opposed` manifests and fails closed unless
the adapter used the `same-decoded-cfa-reference` comparison class. The probe now reports, per CFA
channel:

- effective WB gain and Darktable-adaptation clip value;
- physical-white clipped and actually changed reference photosites;
- Shadow response-shoulder candidates and actually raised photosites;
- compiled/reference chrominance offsets;
- shared-downstream linear and strongest-boundary differences.

The verified three-fixture receipt shows that response-limit candidate admission is broader than
the physical-white reference by 4.42% (lamp), 14.13% (Nikon sky), and 12.18% (Sony), while actual
write totals differ by only 2.08%, 1.26%, and 2.64%. This isolates the next experiment: hold WB,
opposed estimation, area sampling, and camera matrix fixed, then ablate only response-limit versus
physical-white candidate admission. The result must be evaluated on all six admitted regions before
any default-path change; the current calibrated response-limit behavior remains production truth.

The controlled ablation is now complete. `shadow-threshold-ablation` holds `RawFrame`, WB, compiled
opposed chrominance, area sampling, and camera matrix fixed and changes only the terminal admission
domain. Physical-white candidates exactly match the pinned reference counts and form a strict
subset of response-limit candidates on all three sources. Against the pinned same-decode linear
reference, the physical-white branch is closer on every non-zero metric across the six regions,
while the zero-physical-white control remains unchanged. This resolves the threshold attribution
question, but it does not itself authorize a default change.

## Phase 6 — production parity experiment

Status: ready, not started.

The next step is a narrow production experiment that changes the default admission domain together
with CPU/Metal parity, algorithm/cache identity, preview/detail/export contracts, and representative
interactive timing. It must preserve the one-sided photosite write owner and may not import the
offline full-frame artifact path. Acceptance requires the existing six-region oracle comparison,
source-fresh CPU/Metal tests, and proof that warm slider edits reuse the same prepared source with no
new host/device transfer or full-frame pass.
