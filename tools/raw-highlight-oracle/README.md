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
        |       `-- Shadow current vs pinned Darktable-opposed CFA reference
        |
        +-- LibRaw/dcraw_emu  H0 clip / H2 blend / H3 rebuild
        +-- RawTherapee       disabled / Coloropp / Color propagation
        +-- Darktable         caller-controlled XMP only
        `-- vkdt              raw denoise -> CFA hilite -> demosaic -> linear PFM
                                |
                                `-- immutable run manifest + hashed artifacts + logs
```

`oracle_lab.py` owns orchestration, isolation, provenance, and the comparison boundary. It does not
contain or choose Shadow's production algorithm. The production owner remains
[`shadow-image`](../../cpp/shadow-image/README.md), including the same-`RawFrame` diagnostic emitted
by `shadow-raw-probe --highlight-cfa-diagnostic`.

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

The orchestrator uses only the Python standard library. External tools are never built, installed,
downloaded, or updated automatically.

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

Darktable deliberately has no implicit default profile. Select `darktable-xmp` only with an XMP
that was created and reviewed for the experiment:

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
| `shadow-cfa-opposed` | Same Shadow `RawFrame` | Strict Shadow-current versus pinned Darktable-opposed reconstruction |
| `libraw-identify` | Independent container metadata | LibRaw interpretation and camera capability evidence |
| `libraw-h0-clip` | Independent complete pipeline | Clipped baseline |
| `libraw-h2-blend` | Independent complete pipeline | Channel blend ablation |
| `libraw-h3-rebuild` | Independent complete pipeline | dcraw-compatible reconstruction ablation; diagnostic, not a default recommendation |
| `rawtherapee-disabled` | Independent complete pipeline | RawTherapee no-recovery baseline |
| `rawtherapee-coloropp` | Independent complete pipeline | RawTherapee opposed-colour reconstruction |
| `rawtherapee-color-propagation` | Independent complete pipeline | RawTherapee colour-propagation reconstruction |
| `darktable-xmp` | Independent controlled-XMP pipeline | Darktable result under an explicit experiment profile |
| `vkdt-hilite` | Independent GPU pipeline | Raw-mosaic multiscale inpainting before demosaic |

The PP3, controlled XMP, and vkdt graph files are experiment inputs and are hashed in the run
manifest. Their source lineage is recorded in comments and `upstreams.lock.json`.

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

## What is deliberately not implemented yet

There is not yet a common normalized mosaic interchange that feeds every reconstruction engine the
same black-subtracted CFA and metadata. Proprietary-container runs therefore remain whole-pipeline
oracles. The next isolating step is a documented normalized-mosaic-to-DNG bridge with exact active
area, CFA phase, per-colour black/white levels, as-shot neutral, camera matrix, orientation, and
lossless sample storage. It must be verified against round-trip sample equality before any result
is called a same-decode comparison.

Likewise, the harness currently records artifact hashes and pipeline evidence rather than imposing
one image-quality score. Objective comparisons should be added as separate, versioned analysis
owners: linear-domain alignment, clipped-topology masks, boundary hue error, false-colour area,
luminance continuity, and runtime/memory measurements. Capture One may remain a manually exported
visual reference, but it is not a reproducible executable oracle.

## Validation

Run the owner tests without decoding a photo:

```sh
python3 -m unittest discover -s tools/raw-highlight-oracle -p 'test_*.py'
```

The tests use temporary fake executables and verify isolation, immutable run creation, source-path
redaction, source non-copying, adapter status, and strict failure. A real-RAW smoke is additional
local evidence and must keep its outputs outside the source tree.
