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

RawTherapee still performs its own container decode, calibration, demosaic, and colour conversion.
Until a normalized interchange route exists, its output is a complete-pipeline oracle rather than
a same-CFA algorithm comparison.

## vkdt

Source entry: `vkdt/src/pipe/modules/hilite/` at
`b95b3a0ae88959589dc11d07c9040aead5acd1f8`.

vkdt's `hilite` module consumes a raw mosaiced image normalized to `[0,1]` after raw denoise. It
builds a multiscale pyramid whose reductions ignore clipped samples and renormalize from remaining
valid evidence. Assembly upsamples the coarse estimate, matches it to available unclipped channels,
and fills the clipped sites. The `soft` parameter changes scale weighting; `desat` trades retained
source colour against rejection of false chromatic-aberration colour.

This family is structurally different from a fixed-radius colour feather. It can reconstruct broad
clipped regions while keeping replacement ownership at damaged sites, but it is also more expensive
and its result depends on scale/desaturation policy. The bundled graph exposes the default module
between raw denoise and demosaic and exports linear PFM without a display tone mapper.

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

LibRaw may identify camera metadata while still failing to unpack the RAW payload. The Nikon Z9
HE/HE* sample exercises exactly that boundary: `raw-identify` is decoder evidence, while H0/H2/H3
are unavailable reconstructions. Shadow's private provider can still supply the provider-neutral
`RawFrame`, allowing the strict same-CFA reference to run.

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
