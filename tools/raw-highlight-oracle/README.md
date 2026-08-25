# RAW highlight oracle lab

This directory owns an offline, provenance-aware harness for comparing RAW decoding and clipped
highlight reconstruction. It is a development oracle, not a production decoder and not an
interactive rendering path.

The lab exists to answer two different questions without conflating them:

1. Given Shadow's exact provider-neutral `RawFrame`, how does Shadow's current CFA reconstruction
   differ from a pinned reference algorithm?
2. Given the same proprietary RAW container, what result does an independent decoder and complete
   reconstruction pipeline produce?

The first question isolates reconstruction. The second is useful product evidence, but it combines
container interpretation, black/white calibration, white balance, demosaic, colour conversion, and
highlight reconstruction. A visual difference in the second class cannot be attributed to one
stage without further evidence.

## Pipeline and ownership

```text
named source RAW (read-only; never copied)
        |
        +-- Shadow provider route -> RawFrame
        |       +-- untouched CFA evidence
        |       +-- active uint16 CFA -> research DNG -> LibRaw byte-exact re-import
        |       `-- Shadow current vs pinned Darktable-opposed CFA reference
        |
        +-- LibRaw/dcraw_emu  H0 clip / H2 blend / H3 rebuild
        +-- RawTherapee       disabled / Coloropp / Color propagation
        +-- Darktable         isolated default history / caller-controlled XMP
        `-- vkdt              raw denoise -> [CFA hilite] -> demosaic -> linear PFM
                                |
                                `-- immutable run manifest + hashed artifacts + logs
                                                |
                                                +-- region-aware linear objective analysis
                                                `-- local no-copy fixture/candidate matrix
```

`oracle_lab.py` owns orchestration, isolation, provenance, and the comparison boundary. It does not
contain or choose Shadow's production algorithm. The production owner remains
[`shadow-image`](../../cpp/shadow-image/README.md), including the same-`RawFrame` diagnostic emitted
by `shadow-raw-probe --highlight-cfa-diagnostic`.

`normalized_mosaic.py` owns the strict RawFrame staging reader. `cfa_topology.py` owns factual
physical-white, response-shoulder, per-channel, and shared-three-colour CFA masks.
`linear_image.py` owns bounded PFM/TIFF ingestion. `objective_metrics.py` owns Phase 3 registration,
normalization, mask projection, metrics, and derived images. `fixture_matrix.py` owns Phase 4 local source
admission, SHA audit, region export, candidate declarations, and complete per-case evaluation. The
split keeps format parsing and fixture policy out of the adapter orchestrator.

[`REFERENCE_NOTES.md`](REFERENCE_NOTES.md) records the inspected algorithm families and failure
boundaries. [`ROADMAP.md`](ROADMAP.md) defines the normalized-mosaic and objective-metric phases.

This tool has zero dependencies from the desktop, Recipe, preview/detail/export execution, cache
identity, GPU session, or cancellation paths. Adding an oracle must not change those contracts.

## Run contract

Each invocation creates one new directory under a caller-supplied output root outside the Shadow
checkout. Existing run directories are never reused or overwritten. The source RAW is opened in
place and represented in `manifest.json` only by basename, byte length, and SHA-256; it is never
copied into the run. Commands, parsed evidence, stdout, and stderr redact the source and run paths.

Every adapter records:

- the executable's resolved path, size, and SHA-256;
- a redacted argument vector, working directory, and explicit environment overrides;
- the stages and comparison class it claims, plus hashes of every profile or graph input;
- exit status, elapsed time, stdout/stderr logs, and hashes of produced artifacts;
- the pinned upstream source revisions from `upstreams.lock.json`.

Unavailable optional tools are recorded instead of silently substituted. `--strict` fails if any
selected adapter is unavailable or fails. Repeat `--require ADAPTER` to make only named adapters
mandatory.

The adapter orchestrator and fixture catalog use only the Python standard library. Objective
analysis additionally requires NumPy. TIFF analysis records and invokes `tiffcp` to normalize
compression and planar layout before reading only the requested crop. External tools are never
built, installed, downloaded, or updated automatically by these scripts.

## Local CLI setup

The normalized bridge and LibRaw ablations use Homebrew's `libraw` tools. RawTherapee's macOS cask
exposes a CLI launcher, while the oracle resolves its sibling `rawtherapee-cli-bin` so a launcher
signature failure cannot be mistaken for an algorithm failure. Each run redirects RawTherapee's
settings and cache through `RT_SETTINGS` and `RT_CACHE`. Darktable ships its CLI inside the
application bundle, so it can be passed by absolute path without adding a global symlink:

```sh
brew install libraw
brew install --cask rawtherapee

raw-identify -v -w /absolute/path/to/input.raw
unprocessed_raw -q -T /absolute/path/to/input.dng
RT_SETTINGS=/private/tmp/rt-settings RT_CACHE=/private/tmp/rt-cache \
  /Applications/RawTherapee.app/Contents/MacOS/rawtherapee-cli-bin \
  -q -o /private/tmp/result.tif -p /absolute/profile.pp3 -t -b16 -Y -c input.raw

python3 tools/raw-highlight-oracle/oracle_lab.py inventory \
  --darktable-cli /Applications/darktable.app/Contents/MacOS/darktable-cli
```

`unprocessed_raw`, rather than `dcraw_emu`, owns the independent pre-demosaic round-trip. The
Homebrew LibRaw `dcraw_emu` does not expose classic dcraw's `-D` document option. `vkdt` has no
locally discovered Homebrew formula. On macOS its pinned CLI needs the Vulkan loader, MoltenVK,
and shader compiler:

```sh
brew install vulkan-loader molten-vk glslang vulkan-tools
git clone https://github.com/hanatos/vkdt ~/probe/vkdt
git -C ~/probe/vkdt checkout b95b3a0ae88959589dc11d07c9040aead5acd1f8
```

Build a task-private copy with Rawler input (`VKDT_USE_RAWINPUT=2`) and pass its absolute
`vkdt-cli`. The oracle also accepts the exact MoltenVK ICD JSON through `--vkdt-icd`; both the CLI
and ICD are hashed. Codex's filesystem sandbox does not expose Metal, so a sandboxed run can return
`VK_ERROR_INCOMPATIBLE_DRIVER` even though `vulkaninfo --summary` succeeds on the host. A real vkdt
run therefore requires host GPU execution. This is an execution-environment fact, not an algorithm
fallback.

## Inventory and execution

Inspect the local tool inventory:

```sh
python3 tools/raw-highlight-oracle/oracle_lab.py inventory
```

Plan a run without executing a decoder:

```sh
python3 tools/raw-highlight-oracle/oracle_lab.py run \
  --input /absolute/path/to/photo.raw \
  --output-root /private/tmp/shadow-raw-oracles \
  --run-name example-plan \
  --dry-run
```

Run the strict same-decoded-CFA comparison with a task-private Shadow build:

```sh
python3 tools/raw-highlight-oracle/oracle_lab.py run \
  --input /absolute/path/to/photo.raw \
  --output-root /private/tmp/shadow-raw-oracles \
  --run-name shadow-cfa-example \
  --oracle shadow-rawframe \
  --oracle shadow-cfa-opposed \
  --require shadow-cfa-opposed \
  --shadow-probe /absolute/task-build/cpp/shadow-image/shadow-raw-probe
```

Combine one or more successful strict manifests into an immutable alignment receipt:

```sh
python3 tools/raw-highlight-oracle/strict_alignment.py \
  --fixture lamp=/absolute/lamp-strict-cfa/manifest.json \
  --fixture sony=/absolute/sony-strict-cfa/manifest.json \
  --output-root /private/tmp/shadow-raw-strict-alignment \
  --run-name lamp-sony-a
```

The receipt compares per-CFA-channel candidate and write counts, effective white-balance gains,
opposed chrominance offsets, and the shared-downstream linear output. Count agreement is explicitly
not a spatial mask-intersection proof. The analyzer is a separate owner because execution and
cross-run attribution have different schemas and failure policies; `oracle_lab.py` remains the
adapter/run orchestrator.

Run the locally installed LibRaw ablation:

```sh
python3 tools/raw-highlight-oracle/oracle_lab.py run \
  --input /absolute/path/to/photo.raw \
  --output-root /private/tmp/shadow-raw-oracles \
  --run-name libraw-example \
  --oracle libraw-identify \
  --oracle libraw-h0-clip \
  --oracle libraw-h2-blend \
  --oracle libraw-h3-rebuild
```

Create and independently verify the normalized research DNG:

```sh
python3 tools/raw-highlight-oracle/oracle_lab.py run \
  --input /absolute/path/to/photo.raw \
  --output-root /private/tmp/shadow-raw-oracles \
  --run-name normalized-dng-example \
  --oracle shadow-normalized-dng \
  --require shadow-normalized-dng \
  --shadow-decode-helper \
    /absolute/task-build/cpp/shadow-image/shadow-image-decode-helper
```

The adapter calls Shadow's existing `raw-frame-staging` boundary, writes an uncompressed active-
area DNG, checks its own strip and private descriptor, asks `raw-identify` to import it, and asks
LibRaw `unprocessed_raw` to export the CFA again. Success requires the final uint16 sample SHA-256
to equal the original staging SHA-256.

Run child reconstruction oracles from a verified normalized-DNG parent. The parent manifest is
hashed and its exact `normalized.dng` artifact identity must match the input before the child run is
created:

```sh
python3 tools/raw-highlight-oracle/oracle_lab.py run \
  --input /private/tmp/parent/adapters/shadow-normalized-dng/normalized.dng \
  --normalized-parent-manifest /private/tmp/parent/manifest.json \
  --output-root /private/tmp/shadow-raw-oracles \
  --run-name normalized-children \
  --oracle rawtherapee-coloropp \
  --oracle rawtherapee-color-propagation \
  --oracle darktable-default \
  --strict
```

Run the strict within-vkdt highlight ablation from that same parent:

```sh
python3 tools/raw-highlight-oracle/oracle_lab.py run \
  --input /private/tmp/parent/adapters/shadow-normalized-dng/normalized.dng \
  --normalized-parent-manifest /private/tmp/parent/manifest.json \
  --output-root /private/tmp/shadow-raw-oracles \
  --run-name vkdt-ablation \
  --oracle vkdt-hilite-disabled \
  --oracle vkdt-hilite \
  --require vkdt-hilite-disabled \
  --require vkdt-hilite \
  --vkdt-cli /absolute/task-build/bin/vkdt-cli \
  --vkdt-icd /opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json
```

The two graphs share input, raw denoise, demosaic, colour, and output. The disabled graph removes
only the raw-mosaic `hilite` node, making it a stronger algorithm ablation than subtracting two
unrelated complete pipelines.

`darktable-default` uses an empty in-memory library, disables custom presets, and therefore records
the executable-defined default history without reading an adjacent sidecar. The bundled controlled
ablation keeps the normalized DNG, isolated configuration, export settings, and version-4 highlight
parameters fixed; its two XMP files differ only in the first parameter, `mode=clip` versus
`mode=inpaint opposed`:

```sh
python3 tools/raw-highlight-oracle/oracle_lab.py run \
  --input /absolute/path/to/verified-normalized.dng \
  --normalized-parent-manifest /absolute/path/to/parent/manifest.json \
  --output-root /private/tmp/shadow-raw-oracles \
  --run-name darktable-highlight-ablation \
  --oracle darktable-highlights-clip \
  --oracle darktable-highlights-opposed \
  --darktable-cli /absolute/path/to/darktable-cli \
  --require darktable-highlights-clip \
  --require darktable-highlights-opposed
```

For a different named experiment, select `darktable-xmp` only with an XMP that was created and
reviewed for that experiment:

```sh
python3 tools/raw-highlight-oracle/oracle_lab.py run \
  --input /absolute/path/to/photo.raw \
  --output-root /private/tmp/shadow-raw-oracles \
  --run-name darktable-controlled \
  --oracle darktable-xmp \
  --darktable-cli /absolute/path/to/darktable-cli \
  --darktable-xmp /absolute/path/to/controlled.xmp \
  --require darktable-xmp
```

Darktable config, cache, temporary files, and library database are isolated inside that adapter's
run directory. RawTherapee settings and cache are isolated likewise. Adjacent user sidecars and
application defaults are therefore not silently admitted as oracle inputs.

## Adapter catalogue

| Adapter | Boundary | Intended evidence |
| --- | --- | --- |
| `shadow-rawframe` | Shadow decode to provider-neutral CFA | Decoder descriptor and untouched sensor plane |
| `shadow-normalized-dng` | Same Shadow active CFA through research DNG and LibRaw | Byte-exact independent sample re-import plus exact private metadata receipt |
| `shadow-cfa-opposed` | Same Shadow `RawFrame` | Strict Shadow-current versus pinned Darktable-opposed reconstruction |
| `libraw-identify` | Independent container metadata | LibRaw interpretation and camera capability evidence |
| `libraw-h0-clip` | Independent complete pipeline | Clipped baseline |
| `libraw-h2-blend` | Independent complete pipeline | Channel blend ablation |
| `libraw-h3-rebuild` | Independent complete pipeline | dcraw-compatible reconstruction ablation; diagnostic, not a default recommendation |
| `rawtherapee-disabled` | Independent complete pipeline | RawTherapee no-recovery baseline |
| `rawtherapee-coloropp` | Independent complete pipeline | RawTherapee opposed-colour reconstruction |
| `rawtherapee-color-propagation` | Independent complete pipeline | RawTherapee colour-propagation reconstruction |
| `darktable-highlights-clip` | Same Darktable normalized-DNG pipeline | Controlled clipped baseline |
| `darktable-highlights-opposed` | Same Darktable normalized-DNG pipeline | Controlled inpaint-opposed ablation |
| `darktable-default` | Independent isolated default pipeline | Executable-defined history with no sidecar or custom presets |
| `darktable-xmp` | Independent controlled-XMP pipeline | Darktable result under an explicit experiment profile |
| `vkdt-hilite-disabled` | Same vkdt normalized-DNG pipeline | Baseline with only the raw-mosaic hilite node removed |
| `vkdt-hilite` | Same vkdt normalized-DNG pipeline | Raw-mosaic multiscale inpainting before demosaic |

The PP3, bundled or caller-supplied XMP, and vkdt graph files are experiment inputs and are hashed
in the run manifest. Their source lineage is recorded here, in `REFERENCE_NOTES.md`, and in
`upstreams.lock.json`.

## Source references and licensing

Pinned references live outside the Shadow checkout, conventionally under `~/probe`, and must be
treated as read-only research inputs. The current revisions and the exact files inspected are in
`upstreams.lock.json`. Typical reference clones are:

```sh
git clone https://github.com/darktable-org/darktable ~/probe/darktable
git clone https://github.com/RawTherapee/RawTherapee ~/probe/RawTherapee
git clone https://github.com/hanatos/vkdt ~/probe/vkdt
```

Do not replace the lock with a floating branch. When moving a reference, record the exact commit,
reinspect the relevant files and their per-file license, update the lock, and rerun the owner tests.
Shadow is GPL-3.0, but compatible project-level licensing does not remove attribution, per-file
exception, or source-provenance obligations.

## Phase 3 objective comparison

Objective analysis consumes declared RGB TIFF or PFM inputs and never assumes that a container's
integer samples are already linear. Every input declares `linear` or `srgb`; sRGB is decoded with a
recorded IEC 61966-2-1 piecewise transform. PFM row orientation is also explicit. Standard PFM is
`bottom-up`, while pinned vkdt's `o-pfm` source documents and emits a non-flipped `top-down`
dialect. Omitting `--reference-pfm-orientation top-down` or the corresponding candidate option for
vkdt produces a vertically mirrored but otherwise plausible analysis and is invalid evidence.

Generate a byte-exact CFA topology once for each normalized `RawFrame` staging payload. The output
is a one-byte-per-photosite bitfield and a hashed manifest; it remains outside the repository and
does not copy the source RAW. `physical-white` is the factual damage mask. The separately recorded
`linear-response-terminal` mask identifies response shoulder samples and must not be presented as
already clipped. The `shared-physical-white` bit reproduces Shadow's native 3x3 all-observed-colour
terminal-core predicate without granting an algorithm permission to modify that whole support.

```sh
python3 tools/raw-highlight-oracle/cfa_topology.py \
  --staging-manifest /private/tmp/parent/adapters/shadow-normalized-dng/source.shadowrawi \
  --parent-manifest /private/tmp/parent/manifest.json \
  --output-root /private/tmp/shadow-raw-cfa-topology \
  --run-name sony-a1
```

Use identity normalization for a same-pipeline ablation. `exterior-rgb` is only for an explicitly
declared complete-pipeline comparison, where per-channel reliable-exterior p99.5 scales are part of
the receipt. It must not be used to hide a candidate's broad write footprint.

```sh
python3 tools/raw-highlight-oracle/objective_metrics.py \
  --reference /private/tmp/vkdt-off/result.pfm \
  --reference-transfer linear \
  --reference-pfm-orientation top-down \
  --candidate oracle-vkdt-hilite=/private/tmp/vkdt-on/result.pfm \
  --candidate-transfer oracle-vkdt-hilite=linear \
  --candidate-pfm-orientation oracle-vkdt-hilite=top-down \
  --candidate-manifest oracle-vkdt-hilite=/private/tmp/vkdt-on/manifest.json \
  --region-spec /private/tmp/regions/sony-sun-disc-gradient.json \
  --topology-manifest /private/tmp/shadow-raw-cfa-topology/sony-a1/topology.json \
  --topology-mask physical-white \
  --topology-image-space display \
  --output-root /private/tmp/shadow-raw-objective \
  --run-name sony-vkdt-ablation \
  --normalization identity \
  --max-shift 0
```

One immutable analysis emits registered linear crops, RGB and luminance difference PFMs,
clipped-core/boundary/reliable-exterior/false-colour PGMs, per-region numerical summaries, source
and conversion identities, adapter runtime when a matching run manifest is supplied, analysis
runtime, artifact sizes, and analysis peak RSS. When topology is supplied, the human region defines
only the scene crop and optional reliable exterior; the factual CFA mask replaces its manual core.
Exact reference dimensions are required, so an accidental resample fails closed. Whole-image
averages are deliberately absent.

Some oracle pipelines remove a deterministic sensor border. Declare that geometry explicitly with
`--topology-reference-crop-xywh X,Y,WIDTH,HEIGHT`. The receipt records an exact integer crop from
the oriented topology plane to the reference dimensions and asserts `resampling=false`; dimensions
that still disagree continue to fail closed. For example, the pinned RawTherapee normalized-DNG
route emits Sony output at `8632x5752` from an `8640x5760` active plane, represented as
`--topology-reference-crop-xywh 4,4,8632,5752`.

## Phase 4 local fixture and candidate matrix

The repository contains only a schema and a path-placeholder example. A real definition and
catalog stay outside the checkout. `build` hashes each named local RAW in place and records
`copied_into_catalog=false`; `audit` detects missing or changed sources and requires all six scene
classes. `export-regions` materializes immutable Phase 3 region JSON files. `evaluate` refuses
partial case coverage and preserves each case instead of collapsing it into one quality score.
`--require-cfa-topology` additionally rejects a case that fell back to a hand-labelled core.

```sh
python3 tools/raw-highlight-oracle/fixture_matrix.py build \
  --definition /private/tmp/local-fixture-definition.json \
  --output /absolute/local/payloads/fixture-matrix.json \
  --matrix-version 20260825.2

python3 tools/raw-highlight-oracle/fixture_matrix.py audit \
  --catalog /absolute/local/payloads/fixture-matrix.json

python3 tools/raw-highlight-oracle/fixture_matrix.py export-regions \
  --catalog /absolute/local/payloads/fixture-matrix.json \
  --output-directory /private/tmp/raw-highlight-regions
```

Candidate declarations are mandatory before evaluation: write ownership, context radius,
reusable upstream state, recomputation frontier, residency/transfers, cache impact, cancellation,
and preview/detail/export equivalence. An oracle declaration explicitly makes no production
equivalence claim.

For a sensor-factual evaluation, pass all six analysis receipts with
`--require-cfa-topology`. A genuine `ordinary-unclipped-control` must then contain zero
physical-white samples; every clipped class must contain at least one. A mislabeled fixture fails
before an evaluation artifact is written.

The local matrix uses three named RAW sources and six cases. CFA projection invalidated the original
`nikon-he-unclipped-cloud` control because that crop contains 617,792 samples at physical white, and
the topology-required Phase 4 gate correctly rejected it. Matrix `20260825.4` retains the visually
reviewed `nikon-he-bright-unclipped-control`, a varied settlement/barrier crop containing zero
physical-white samples. Matrix `20260825.5` declares six independently runnable candidates: vkdt
hilite, both RawTherapee modes, Darktable opposed, LibRaw H2 blend, and LibRaw H3 rebuild. Source
audit, immutable region export, and complete topology-required evaluations for all six candidates
pass. The immutable catalog is `fixture-matrix-20260825e.json`, SHA-256
`fe86bb9151615c6e746bcefc42b4b3ea06e6738071cccfc427c8a070ccdc6a42`.
The six candidate-evaluation receipts are retained beside the local catalog under
`evaluations/20260825.5/`; every receipt binds that exact matrix SHA and preserves all six cases.

These percentages measure changed pixels against each engine's own disabled baseline. They describe
write footprint, not perceptual quality and not cross-engine colour equivalence. On that bounded
measurement, RawTherapee Coloropp changes about 43.78% of the reliable exterior in the Nikon smooth
sky case, while Color propagation changes about 0.65% and vkdt about 0.0029%. Both RawTherapee modes
change roughly 4--5% in the lamp cases and 5.06% in the Sony sun-disc case, versus at most 0.096%
and 1.23% respectively for vkdt. The repaired control remains quiet in all three comparisons.

The controlled Darktable opposed pass remains below the false-colour threshold in both lamp cases,
the Nikon smooth-sky case, and the repaired control, but changes about 7.66% of the reliable Sony
sun-disc exterior relative to Darktable clip. LibRaw H2 and H3 are much broader: each changes about
5.10% of the repaired zero-physical-white control, 32--56% of the lamp exteriors, and roughly
96--100% of the Nikon smooth-sky and Sony sun-disc exteriors. This makes LibRaw useful negative and
boundary evidence, not a plausible direct production candidate.

The receipts still expose substantial offline working memory: analysis peak RSS ranges from
hundreds of megabytes to more than one gigabyte for selected crops because this owner retains and
writes several float RGB artifacts. That cost is recorded honestly and does not enter Shadow's
preview path. All six candidates remain oracle evidence rather than drop-in production admission.

The strict same-`RawFrame` three-fixture alignment adds a narrower result. Shadow's production
response-shoulder selector admits 4.42% more candidates than the physical-white Darktable
adaptation on the lamp fixture, 14.13% more on the Nikon smooth-sky fixture, and 12.18% more on the
Sony sun fixture. Actual one-sided writes are much closer: 2.08%, 1.26%, and 2.64% more,
respectively. The provider facts explain the mask difference: both Nikon inputs use linear-response
limits `15311` below physical white `16383`, while Sony uses `15360` below `16383`. This is evidence
for a controlled threshold-domain ablation, not authorization to discard Shadow's calibrated
response shoulder. The complete local receipt is
`/private/tmp/shadow-raw-strict-alignment/three-fixture-20260826c/alignment.json`; RAW and rendered
payloads remain outside the repository.

The follow-up `shadow-threshold-ablation` adapter now makes that experiment reproducible:

```bash
python3 tools/raw-highlight-oracle/oracle_lab.py run \
  --input /absolute/source.raw \
  --output-root /private/tmp/shadow-threshold-ablation \
  --run-name source-id \
  --oracle shadow-threshold-ablation \
  --require shadow-threshold-ablation \
  --shadow-probe /absolute/build/shadow-raw-probe
```

It writes linear/display response-limit and physical-white outputs plus their absolute difference.
Per-channel receipts record candidate/write intersection and union, response-only/physical-only
counts, IoU, and positive deltas. Both branches share decoded CFA, white balance, compiled opposed
chrominance, area sampling, and camera matrix; no product render or cache is read or mutated.

On the three verified sources, every physical-white candidate set is an exact subset of the
response-limit set, and its per-channel candidate counts exactly match the pinned Darktable
physical-white reference. Six fixed-region comparisons against that same-`RawFrame` reference leave
the repaired unclipped control unchanged and place the physical-white branch closer on every
reported non-zero boundary/core metric. The strongest separation is boundary continuity: lamp
curvature error falls by roughly 94--97%, Nikon-sky curvature by about 72%, and Sony-ray curvature
by about 77%. This is reference proximity, not a perceptual-quality proof or automatic production
admission. The verified local run roots are
`/private/tmp/shadow-threshold-ablation-runs-20260826-a/` and
`/private/tmp/shadow-threshold-darktable-objective-20260826-a/`.

## Exactness boundary and remaining work

The normalized-mosaic-to-DNG bridge is implemented. It preserves the active samples byte-for-byte,
rephased CFA, four-site black/white/linear-response values, as-shot neutral, orientation, provider
identity, matrices, and unapplied-opcode declarations in `DNGPrivateData`. Standard DNG tags carry
the interoperable projection. In particular, standard DNG `WhiteLevel` has one value for a one-
sample CFA IFD; when Shadow has unequal physical whites across CFA sites, the bridge uses the
conservative minimum and records `standard_white_projection=conservative-minimum`. Such a file is
transport evidence, not a claim that every external decoder received four independent white
levels.

RawTherapee, Darktable, and the pinned vkdt CLI can consume a generated DNG in a child run whose
parent artifact is verified and recorded. Nikon HE*, the lamp edge, and Sony sun fixtures have all
completed the vkdt normalized child on the host GPU. Each external reconstruction output remains a
complete pipeline unless a same-engine ablation (such as vkdt hilite on/off) holds the other stages
fixed.

The Phase 3/4 owners now produce versioned metrics and a topology-enforcing candidate-evaluation gate.
Factual clipping topology is derived from byte-exact CFA samples and four-site physical whites,
then projected through recorded RawFrame orientation and the exact analysis crop. Manual rectangles
remain a compatibility fallback and are rejected by topology-required evaluation. Capture One may
remain a manually exported visual reference, but it is not a reproducible executable oracle.

## Validation

Run the owner tests without decoding a photo:

```sh
python3 -m unittest discover -s tools/raw-highlight-oracle -p 'test_*.py'
```

The tests use temporary fake executables and verify isolation, immutable run creation, source-path
redaction, source non-copying, adapter status, and strict failure. A real-RAW smoke is additional
local evidence and must keep its outputs outside the source tree.
