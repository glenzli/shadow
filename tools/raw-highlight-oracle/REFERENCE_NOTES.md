# Highlight reconstruction reference notes

These notes record the source-level ideas inspected for the oracle lab. They are design evidence,
not a substitute for the pinned source and not a claim that independent pipelines are numerically
equivalent to Shadow.

All revisions below are locked in [`upstreams.lock.json`](upstreams.lock.json).

## Darktable opposed reconstruction

Source entry: `darktable/src/iop/hlreconstruct/opposed.c` at
`943d74a50e5baeecee26005cf20309e32f487949`.

The opposed method works before ordinary display grading and reasons over CFA colour evidence. For
each asking location, it forms a local three-colour superpixel and estimates a clipped colour from
the other two colours in cube-root space. It separately gathers chrominance close to clipped data.
Its damaged-site mask may be dilated to gather candidate context, but candidate gathering is not a
license to replace every gathered neighbour.

The source itself names important failure modes: incorrect physical white, large differences
between the assumed and appropriate white-balance coefficients, complicated illumination whose
gradients are not related, and the limits of the opposed estimate. Those are precisely why the
oracle must preserve black/white calibration and white-balance provenance rather than compare only
the final display image.

Shadow's existing `raw_highlight_reference_pipeline` is the strict reference adaptation. It starts
from Shadow's exact `RawFrame`; no external container decoder is involved. Its display PPM is for
inspection, while its linear PFM and CFA-domain CSVs are the diagnostic truth.

The strict alignment receipt now makes the remaining threshold distinction explicit. The pinned
adaptation evaluates `0.987` in the physical-white-normalized domain, while Shadow production
evaluates the same numeric shoulder in the calibrated linear-response domain. On the verified
fixtures, the latter limits are `15311/16383` for both Nikon inputs and `15360/16383` for Sony.
That expands production candidate counts by about 4.42--14.13%, although actual one-sided write
counts remain within about 1.26--2.64% of the reference totals. Per-channel WB gains and
chrominance-offset deltas remain in the receipt so a future experiment can separate mask admission
from opposed-colour estimation. Counts alone do not prove that the two spatial masks overlap.

The strict threshold adapter now supplies that spatial proof. It evaluates both admission domains
through the same Shadow sampler and records exact candidate/write intersections. Across lamp,
Nikon-sky, and Sony, `physical-white` is a strict subset of `linear-response-limit`; there are zero
physical-only candidates or writes. Its per-channel candidate counts equal the pinned Darktable
adaptation's clipped counts exactly. Against the pinned same-`RawFrame` linear output, the six fixed
regions show no changed pixels in the Nikon unclipped control and no metric where the physical-white
branch is farther away. The largest improvements are in luminance curvature at the clipped
boundary, not in broad colour mixing, which supports the hypothesis that the extra response-shoulder
writes create the remaining contour. It still does not prove that Darktable is perceptually ideal,
so the evidence stays offline until a production-path experiment updates all backend identities and
contracts together.

The external Darktable ablation uses two bundled version-4 highlight XMP records. Their 48-byte
parameter payloads differ only in the first little-endian integer: `0` for clip and `5` for inpaint
opposed. Darktable 5.6 completed both profiles on all three verified normalized DNGs without changing
active-area dimensions. Relative to its clip baseline, opposed stayed below the false-colour
threshold in the lamp and Nikon regions but changed about 7.66% of the reliable Sony sun-disc
exterior. That is strong same-engine evidence, but it is still a full Darktable pipeline rather than
the strict same-`RawFrame` reference above.

Darktable also logs a RawSpeed warning that the research DNG has no standard `Make` entry. Both
members of the controlled pair share that metadata limitation, so their delta remains usable as a
highlight-module ablation; the full rendered colour is not yet a camera-profile oracle. A future DNG
metadata revision must add independently verified standard camera identity without weakening the
byte-exact CFA interchange contract.

## RawTherapee

Source entries:

- `RawTherapee/rtengine/rawimagesource.cc`
- `RawTherapee/rtengine/hilite_recon.cc`
- `RawTherapee/doc/manpage/rawtherapee.1`

The `Coloropp` method is RawTherapee's adaptation of the Darktable opposed algorithm. It computes
white-balance-aware clip thresholds and replaces clipped channel evidence from the opposing
colours. The `Color` method enters RawTherapee's separate inpainting route. These are intentionally
separate adapters because a single on/off comparison would not reveal which family helped.

`rawtherapee-cli` begins with neutral processing values when `-d`, `-s`, and `-S` are absent, then
applies each `-p` profile in command-line order. The oracle profiles therefore contain only the
`HLRecovery` group. They do not import the user's default profile or an adjacent sidecar.

RawTherapee still performs its own DNG import, calibration, demosaic, and colour conversion. The
normalized-DNG route now removes proprietary-container decoding as a variable and verifies the
parent sensor samples before the child run. Its output is nevertheless a complete reconstruction
pipeline unless two RawTherapee profiles differ only at the highlight-recovery setting.

## vkdt

Source entry: `vkdt/src/pipe/modules/hilite/` at
`b95b3a0ae88959589dc11d07c9040aead5acd1f8`.

vkdt's `hilite` module consumes a raw mosaiced image normalized to `[0,1]` after raw denoise. It
builds a multiscale pyramid whose reductions ignore clipped samples and renormalize from remaining
valid evidence. Assembly upsamples the coarse estimate, matches it to available unclipped channels,
and fills the clipped sites. The `soft` parameter changes scale weighting; `desat` trades retained
source colour against rejection of false chromatic-aberration colour.

This family is structurally different from a fixed-radius colour feather. It can reconstruct broad
clipped regions, but it is also more expensive and its result depends on scale/desaturation policy.
The bundled on/off graphs keep raw input, denoise, demosaic, colour, and output fixed; the disabled
graph removes only `hilite`. That makes their difference attributable to the module within vkdt,
without claiming equivalence to Shadow's production stages.

The output module writes a deliberately non-flipped PFM. In the Phase 3 reader this must be declared
as `top-down`; applying the standard bottom-up PFM convention silently mirrors a plausible image and
invalidates region evidence. Real Nikon HE*, lamp, and Sony runs also showed that the multiscale
module can alter broad bright areas outside a manually labelled clipped core. Replacement ownership
therefore remains a measured property, not an assumption taken from the algorithm description.

On macOS the pinned CLI uses Vulkan through MoltenVK and requires host Metal access. A valid loader
and ICD can still fail inside a filesystem sandbox that does not expose Metal. The executable,
graph, and ICD identities belong in the oracle receipt; host execution is not permission to skip
those records.

vkdt is BSD-2-Clause by default and has per-file exceptions, including GPL-3.0 files. Inspect the
exact files before adapting code; never infer a file's license only from the repository headline.

## LibRaw / dcraw-compatible modes

The installed `dcraw_emu` exposes three useful ablations:

- `-H 0`: clip highlights;
- `-H 2`: blend clipped channels;
- `-H 3`: rebuild highlights using the dcraw-compatible reconstruction path.

The lab exports camera-white-balanced, linear 16-bit TIFF with no automatic brightening. These
modes are fast whole-pipeline baselines. `H3` is not Shadow's intended default: it may generate
plausible colour by spreading context, so apparent smoothness alone is not evidence of correct
ownership or retained light energy.

LibRaw may identify camera metadata while still failing to unpack the proprietary RAW payload. The
Nikon Z9 HE/HE* sample exercises exactly that boundary: direct-container `raw-identify` is decoder
evidence, while direct H0/H2/H3 reconstruction remains unavailable. Shadow's private provider can
instead supply the provider-neutral `RawFrame`; the verified normalized DNG then makes H0/H2/H3
runnable without claiming that LibRaw decoded the original HE* container.

The normalized-DNG H2/H3 ablation is deliberately retained as negative evidence. Both modes changed
about 5.10% of the repaired zero-physical-white control, 32--56% of the lamp reliable exteriors, and
roughly 96--100% of the Nikon smooth-sky and Sony sun-disc exteriors relative to H0. Their apparent
smoothness therefore cannot justify adopting either reconstruction boundary in Shadow.

## What to compare

A useful comparison should preserve more than a screenshot:

1. raw active-area geometry and CFA phase;
2. per-colour black and physical-white levels;
3. measured and reconstructed clipped-site topology;
4. boundary hue/chroma error without expanding replacement ownership;
5. luminance continuity across the physical clipping boundary;
6. retained energy in one-colour emitters and sunset gradients;
7. false-colour area outside factual damage;
8. elapsed time, peak working memory, and whether the method can remain a one-time source pass.

The first three decide whether two results are even comparable. The next four describe recovery
quality. The last item decides whether a successful oracle is compatible with Shadow's responsive
editing contract.

Phase 3 now projects the physical-white mask from Shadow's byte-exact active CFA into the registered
linear crop. It separately records response-shoulder sites and the native 3x3 shared terminal core,
because neither is interchangeable with the per-site factual damage mask. Human rectangles retain
only scene-crop and compatibility-fallback roles. This projection also caught a fixture error: the
first Nikon "unclipped cloud" control actually contains hundreds of thousands of physical-white
sites and cannot serve as an unclipped control.
