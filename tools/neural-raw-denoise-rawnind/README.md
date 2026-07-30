# RawNIND public RAW foundation audit

This development tool audits the public `rawdenoise-nind` model package
released for darktable 5.6. It does not install a production model or change
Shadow's render pipeline.

RawNIND is the first public candidate evaluated here whose Bayer model
matches the semantic boundary discussed for AI RAW denoise:

```text
normalized packed Bayer [R, G1, G2, B]
                 |
                 v
       RawNIND UtNet2 denoiser
                 |
                 v
denoised + demosaiced linear camera RGB
```

The output is not an 8-bit JPEG. It is still scene-linear floating-point
camera RGB. However, the network has already made the demosaic decision, so
it cannot replace Shadow's existing 4-channel RAW-to-RAW denoise node. It is
a candidate implementation for a separately materialized RAW foundation.

Responsibility map:

- `package_contract.py`: official model-package identity and extraction
- `inference.py`: one static model tile and RAW-to-packed preprocessing
- `tiling.py`: full-image geometry, trusted overlap, deterministic blending,
  working-memory admission, global gain, and seam diagnostics
- `stripe_lifecycle.py`: direct-reflect tile extraction, bounded rolling
  accumulation, two-pass global gain, cancellation, and atomic sink lifecycle
- `foundation_artifact.py`: single-file foundation container, cache identity,
  checksums, bounded row reads, no-overwrite publication, and partial recovery
- `provider_sidecar.py`: strict `--verify-model` / `--run` process protocol
  used by Shadow's fail-closed Rust provider
- `public_pair.py` / `quality.py`: public paired-RAW identity and quality probe
- `dataset_manifest.py`: pinned Dataverse inventory and official test-reserve
  selection without RAW download
- `dataset_download.py`: resumable external download, payload verification,
  and atomic no-overwrite publication
- `benchmark.py`: camera-diverse batch execution, aggregation, and admission
- `audit.py`: composition and admission report only

## Pinned public release

- model repository:
  <https://github.com/darktable-org/darktable-ai>
- release:
  <https://github.com/darktable-org/darktable-ai/releases/tag/release-5.6.0>
- repository revision:
  `5454d7aa6d89a67054fd4a83343b09e69acaf76a`
- release asset:
  `rawdenoise-nind.dtmodel`
- release asset SHA-256:
  `d71b5f1e727c85a359e6f74dca9e2016c9d8fc3e2f7ac3e9b347d80ceca969af`
- model/training source:
  <https://github.com/trougnouf/rawnind_jddc>
- pinned training-source revision:
  `4d455aa8ada69214eafa6a91ac0b2e011cf9dcb7`
- paper:
  <https://arxiv.org/abs/2501.08924>
- model and training-code license: GPL-3.0
- model-card training-data declaration: CC BY 4.0 / CC0 per image
- current RawNIND Dataverse dataset license: CC-BY-SA-4.0

The tool verifies the release-asset digest, the exact archive member set,
every member digest, and the model manifest before loading ONNX.

The model card and the current top-level Dataverse record use different
wording for training-data licensing. The audit preserves both declarations
instead of silently treating them as equivalent. Any downloaded evaluation
RAW follows the Dataverse record attached to that file.

## Model contract

The Bayer variant has a static input:

- name: `input`
- dtype: float32
- shape: `[1, 4, 512, 512]`
- channel order: `R, G1, G2, B`
- normalization: per-CFA-site `(sample - black) / (white - black)`
- white balance: none

Its output is:

- name: `output`
- dtype: float32
- shape: `[1, 3, 1024, 1024]`
- space: linear camera RGB
- learned scale: arbitrary, requiring scalar mean gain matching

The network is not noise-level conditioned. Unlike PMRID, it was trained on
real noisy/clean pairs from multiple cameras.

The UtNet2 architecture has a 200-packed-pixel theoretical receptive field;
a conservative exact-context halo is therefore 100 packed pixels. The
published darktable integration blends 512-pixel tiles with 32 packed pixels
of overlap, while its Python demo trims 64 packed pixels. Those are practical
overlaps, not proof of exact equivalence to whole-image inference.

## Reproduce

Keep packages, extracted models, caches, and reports outside the repository:

```bash
python3 -m venv /private/tmp/shadow-rawnind-venv
/private/tmp/shadow-rawnind-venv/bin/pip install \
  -r tools/neural-raw-denoise-rawnind/requirements.lock

curl -L \
  -o /private/tmp/rawdenoise-nind.dtmodel \
  https://github.com/darktable-org/darktable-ai/releases/download/release-5.6.0/rawdenoise-nind.dtmodel

TMPDIR=/private/tmp/shadow-rawnind-coreml-cache \
/private/tmp/shadow-rawnind-venv/bin/python \
  tools/neural-raw-denoise-rawnind/audit.py \
  --package /private/tmp/rawdenoise-nind.dtmodel \
  --output-dir /private/tmp/shadow-rawnind-audit \
  --sample-raw local-reference/sample-assets/dng/iphone-8-16bit.dng \
  --quality-noisy /private/tmp/RawNIND/Bayer_7D-1_ISO12800.cr2 \
  --quality-ground-truth /private/tmp/RawNIND/Bayer_7D-1_GT_ISO100.cr2 \
  --quality-preview /private/tmp/shadow-rawnind-audit/quality.ppm \
  --stripe-raw /private/tmp/RawNIND/Bayer_7D-1_ISO12800.cr2 \
  --foundation-artifact \
    /private/tmp/shadow-rawnind-audit/7d-1.shadowrawf
```

The report records backend parity, latency, context sensitivity, local RAW
tile admission, a bounded public paired-RAW quality probe when requested,
package identities, and the product decision. The public pair is verified
against its official Dataverse size and MD5 before RAW decoding, and the
report adds a local SHA-256 receipt. A successful runtime and one-pair quality
audit still reports `product_eligible: false`; a camera-diverse benchmark and
a production foundation-output contract are separate gates.

The optional display preview is one bounded crop in the fixed order
`ground truth | noisy bilinear baseline | RawNIND`. It applies only a shared
clip and 1/2.2 display power; it is diagnostic, not a color-managed export.

## Known-sensor benchmark manifest

Do not use the upstream all-files download command for this benchmark.
RawNIND v1.0 contains 2,845 files totaling 120,172,531,162 bytes. The local
manifest builder verifies a saved official Dataverse metadata response and
the official MD5-pinned `dataset.yaml`, then selects the paper's ten
known-sensor test scenes: six camera models from Canon, Panasonic, and Sony.

The first pilot contains one representative high-noise/ground-truth pair per
scene: 20 RAW files totaling 691,917,871 bytes. It emits exact file IDs,
persistent IDs, sizes, MD5 values, URLs, and a canonical manifest SHA-256
without downloading a RAW payload:

```bash
/private/tmp/shadow-rawnind-venv/bin/python \
  tools/neural-raw-denoise-rawnind/dataset_manifest.py \
  --dataverse-metadata \
    /private/tmp/shadow-rawnind-dataverse-metadata.json \
  --dataset-descriptor /private/tmp/shadow-rawnind-dataset.yaml \
  --output /private/tmp/shadow-rawnind-known-sensor-manifest.json
```

After those exact files have been downloaded into one external directory, run:

```bash
/private/tmp/shadow-rawnind-venv/bin/python \
  tools/neural-raw-denoise-rawnind/dataset_download.py \
  --manifest /private/tmp/shadow-rawnind-known-sensor-manifest.json \
  --destination /private/tmp/shadow-rawnind-known-sensor \
  --receipt /private/tmp/shadow-rawnind-known-sensor-download.json \
  --workers 16 \
  --segment-mib 1

TMPDIR=/private/tmp/shadow-rawnind-coreml-cache \
/private/tmp/shadow-rawnind-venv/bin/python \
  tools/neural-raw-denoise-rawnind/benchmark.py \
  --manifest /private/tmp/shadow-rawnind-known-sensor-manifest.json \
  --model /private/tmp/shadow-rawnind-model/model_bayer.onnx \
  --raw-root /private/tmp/shadow-rawnind-known-sensor \
  --provider coreml \
  --output /private/tmp/shadow-rawnind-known-sensor-report.json
```

The v1 admission requires every pair to improve MSE, every PSNR delta to be at
least 0.5 dB, and the median delta to be at least 3 dB. It deliberately retains
the existing deterministic center-region metric for the first cross-camera
gate. Multi-region flat-field and texture-retention metrics remain a separately
reported refinement rather than being implied by this result.

The pinned known-sensor pilot passed on CoreML:

- 10/10 scenes improved MSE across all six camera models and all four RAW
  containers (`CR2`, `ARW`, `DNG`, and `CRW`);
- median PSNR delta: `+4.702 dB`;
- worst PSNR delta: `+1.582 dB` on `MuseeL-bluebirds-A7C`;
- median MSE ratio: `33.9%` of the noisy bilinear baseline;
- worst MSE ratio: `69.5%`;
- every bounded alignment was within three sensor pixels.

The worst-scoring diagnostic crop showed clear noise reduction without gross
blocking, checkerboard artifacts, or color discontinuity. Fine feather texture
was smoother than the ground truth, so this result admits the cross-camera
foundation candidate but does not close the multi-region texture-retention
gate. The benchmark report therefore keeps `product_eligible: false`.

## Current bounded result

On Apple arm64 with ONNX Runtime 1.24.4, the pinned Canon 7D-1 public pair
produced this center-crop result after the official GBRG-to-RGGB physical
crop and an independently verified zero-pixel alignment:

- evaluation area: 624 x 624 sensor pixels after a conservative 200-pixel halo
- noisy bilinear baseline: 33.486 dB PSNR
- RawNIND output: 46.073 dB PSNR
- delta: +12.587 dB; output MSE is 5.51% of the noisy baseline
- CoreML/CPU maximum absolute difference: 5.96e-7
- CoreML warm 512-packed-pixel tile p50: roughly 160-180 ms across two runs

This is strong evidence that the public model and its preprocessing are
useful, not evidence that it beats Shadow's current denoiser. The baseline is
the same noisy Bayer tile with canonical bilinear demosaic. One chart-like
scene also cannot admit full-image tiling, camera generalization, or texture
retention. Those remain explicit gates.

## Full-image policy

The full-image audit does not use the published 32- or 64-packed-pixel
practical overlap as proof of exact context. It reserves the model's
100-packed-pixel conservative halo and uses 112 packed pixels per side.
That leaves a 24-packed-pixel trusted crossfade between adjacent predictions
and a 288-pixel step, which preserves the U-Net's 16-pixel pooling phase.

Tiles are inferred at their original scale, blended with separable weights
that sum to one, and gain-matched once after stitching. Per-tile gain is
recorded only as a stability diagnostic. `tiling.py` retains the full-frame
implementation as a small-image mathematical reference.

The bounded lifecycle never creates a full reflected input or a full output
accumulator. It extracts each 512 x 512 packed tile directly from the borrowed
input with NumPy-compatible reflect indices, and retains only output rows that
a later tile row can still affect. After a stripe becomes final, it is
normalized and released to an ordered sink.

One global gain requires the raw stitched mean before publish. The first
implementation therefore runs two deterministic inference passes: pass one
computes the normalized full-image mean and pass two applies the resulting
gain while emitting stripes. This trades roughly 2x inference time for a
simple bounded-memory and no-temporary-payload contract. The sink is opened
before work starts, accepts only contiguous ordered stripes, commits only
after every row, and aborts on cancellation or failure.

The current full Canon 7D run covers a 1731 x 2601 packed image
(3462 x 5202 sensor output) with a 7 x 10 grid:

- 70 CoreML tiles in 15.21 seconds
- tile p50 197.78 ms, p95 222.52 ms
- 8,983,116 trusted overlapping RGB samples
- overlap RMSE `3.51e-9`, or `2.57e-8` of output RMS
- overlap maximum absolute difference `1.03e-7`
- blend coverage maximum error `1.19e-7`
- tile-local diagnostic gain span 2.74%; one global gain is applied

That earlier run remains the full-frame mathematical and seam reference.

For the same Canon dimensions, the stripe plan holds at most 824 sensor rows
and emits at most 576 rows at a time. Its estimated additional working set is
152,469,248 bytes (145.4 MiB), excluding the borrowed 72,037,296-byte packed
input and any sink-owned storage. The previous full-frame reference estimate
is 483,282,624 bytes (460.9 MiB).

The bounded Canon run completed 140 CoreML tile inferences in 27.82 seconds:

- pass one: 13.48 seconds, tile p50 180.90 ms, p95 192.74 ms
- pass two: 14.33 seconds, tile p50 183.20 ms, p95 207.04 ms
- seven committed stripes; 216,111,888 logical output bytes
- pass-one and pass-two raw means are exactly equal
- coverage and trusted-overlap metrics exactly match the full-frame receipt
- ordered stripe digest:
  `ea19a23ccd7bc3c81c3878d5027ab500d184914b2d070aa3e186488622e8a557`

Synthetic odd-dimension tests prove that direct reflection is element-identical
to the full-padding reference and that final stripe output is bit-identical to
the full-frame result. They also prove that cancellation aborts the
unpublished sink and that incomplete row sequences cannot commit. The current
digest-only sink remains available for quick lifecycle audits. The durable
reference sink below materializes the same sequence; proving camera
generalization remains a separate gate.

## Foundation artifact

The first durable reference format uses the `.shadowrawf` extension and this
single-file layout:

```text
magic + canonical JSON header
ordered CHW float32 stripe payloads
canonical JSON manifest
fixed footer: manifest offset + length + SHA-256
```

The v2 header binds the source RAW SHA-256 and size, the exact pinned RawPy
RAW-to-packed-Bayer pixel-contract SHA-256, the RAW normalization receipt,
pinned model/package identity, tiling and global-gain algorithm revision,
ONNX Runtime version, active execution providers, platform, and output pixel
interpretation. These fields form a pre-inference cache key. The v2 reader
deliberately rejects the pre-contract v1 magic and schemas instead of risking
reuse after a decoder-semantic change.
The manifest adds every stripe offset, geometry and digest, the complete
ordered-sequence digest, the actual global-gain receipt, and a final artifact
identity.

The verifier distrusts the writer: it checks exact schemas, bounded offsets,
complete row coverage, cache and artifact identities, every stripe SHA-256,
the ordered sequence digest, finite float32 pixels, and the complete file
SHA-256. A verified reader keeps the same file descriptor open and can load a
bounded row range across stripe boundaries without materializing the full
216 MB image.

Publication writes a uniquely named sibling partial, fsyncs and independently
verifies it, then creates the final name with a hard link. Hard linking is an
atomic no-overwrite operation; an existing cache entry is never replaced.
Cancellation removes the owned partial. A complete partial left by a process
crash can be verified and linked into place later, while a truncated or
tampered partial cannot be recovered.

The reference payload is deliberately lossless little-endian float32. No
compression or float16 conversion is admitted yet, because this artifact is
the correctness baseline against which those storage policies must be
measured. `shadow-cache` now owns an independent Rust verifier, bounded row
reader, content-addressed foundation store, race-safe no-overwrite
publication, and crash recovery for this exact format.

## Provider protocol

The executable reference sidecar speaks the exact bounded stdout protocol
consumed by `shadow-ai`. Model preflight verifies the release package, Bayer
graph, checked-in model manifest, ONNX graph I/O, ONNX Runtime 1.24.4, and
CPU-only execution:

```bash
/private/tmp/shadow-rawnind-venv/bin/python \
  tools/neural-raw-denoise-rawnind/provider_sidecar.py \
  --model-package /private/tmp/rawdenoise-nind.dtmodel \
  --model-graph /private/tmp/shadow-rawnind-model/model_bayer.onnx \
  --manifest apps/desktop/providers/rawnind-foundation/model-manifest.json \
  --verify-model
```

One materialization accepts an exact source RAW and one absent, externally
allocated `.shadowrawf` partial. The sidecar never creates or publishes a
cache entry; the application independently verifies the completed bytes and
then publishes that same partial through `FoundationArtifactStore`.

Before allocating or running inference, the application uses `--plan` with
the same source and fixed pixel contract. Planning verifies and loads the
pinned model/runtime, decodes the RAW, and computes the complete v2 contract
and cache key, but executes zero model tiles:

```bash
/private/tmp/shadow-rawnind-venv/bin/python \
  tools/neural-raw-denoise-rawnind/provider_sidecar.py \
  --model-package /private/tmp/rawdenoise-nind.dtmodel \
  --model-graph /private/tmp/shadow-rawnind-model/model_bayer.onnx \
  --manifest apps/desktop/providers/rawnind-foundation/model-manifest.json \
  --input-raw /private/tmp/RawNIND/Bayer_7D-1_ISO12800.cr2 \
  --source-pixel-contract-sha256 \
    e1998069001c14d01251cc3d6e2bc2aa66b807f3f17d246e7ee7270528302f7f \
  --plan
```

The public Canon `1D3_6400.CR2` plan completed in 0.52 seconds and returned
the exact cache key later sealed by `--run`. A verified cache hit therefore
avoids the roughly 24-second CPU inference and 122 MB rewrite.

```bash
/private/tmp/shadow-rawnind-venv/bin/python \
  tools/neural-raw-denoise-rawnind/provider_sidecar.py \
  --model-package /private/tmp/rawdenoise-nind.dtmodel \
  --model-graph /private/tmp/shadow-rawnind-model/model_bayer.onnx \
  --manifest apps/desktop/providers/rawnind-foundation/model-manifest.json \
  --input-raw /private/tmp/RawNIND/Bayer_7D-1_ISO12800.cr2 \
  --output-foundation /private/tmp/.7d-1.shadowrawf \
  --source-pixel-contract-sha256 \
    e1998069001c14d01251cc3d6e2bc2aa66b807f3f17d246e7ee7270528302f7f \
  --run
```

Only the final machine-readable receipt is written to stdout. Errors go to
stderr and a failed or catchably cancelled run removes the application-owned
partial. The Rust provider treats process success as insufficient: it verifies
the complete artifact and cross-checks source, model, runtime, cache,
artifact, extent, and file identities before returning a generated foundation
descriptor.

This Python executable remains the audited reference and end-to-end acceptance
source. The final desktop distribution freezes this exact route with its
version-pinned runtime instead of depending on an arbitrary developer Python
environment; that packaging boundary and its external build receipt are owned
by
[`apps/desktop/providers/rawnind-foundation/`](../../apps/desktop/providers/rawnind-foundation/README.md).

The executable protocol has completed a real CPU acceptance run on the
public darktable Canon `1D3_6400.CR2` sample:

- output extent: 3908 x 2600 linear camera-RGB pixels
- artifact format: `.shadowrawf` v2; 121,933,266 file bytes
- cache key:
  `3a93ad80243a50dacd9e9da418ae4be88adb28b99b897123913ed08c7e84dc5d`
- artifact identity:
  `b2eefdeeeda2e6628972efa841e4e128eb3e0cdd12aa3f11581ed6d4c25808df`
- complete file SHA-256:
  `ed6aa1de1771abc6a5f3857299e3e8ae36ae6943e7ee47598fabceca0d1c9b48`
- ONNX Runtime: 1.24.4, CPU execution only

The earlier full Canon 7D artifact passed the combined quality, stripe, and
artifact gate before the source pixel contract was introduced. Its numbers
remain useful tiling evidence, but it is a historical v1 payload and the v2
reader intentionally rejects it:

- file bytes: 216,115,795; pixel payload: 216,111,888
- cache key:
  `297f6d6fec3a721a9779d7d1c852b669748da4ace2b8c68c6a054437216770a4`
- artifact identity:
  `bf3fe1f5f33e1591218d02caab4780a907302297d81f1f078cf955af0427a463`
- complete file SHA-256:
  `2fb0c92fc6338fd60ebe6612e2048ae977060c8823f6ecaeaac8ff9697d2685a`
- two-pass generation, sealing, and writer verification: 30.20 seconds
- complete combined v3 audit: 36.70 seconds
- independent full verification: 0.37 seconds
- verified 36-row read crossing a stripe boundary: 0.49 ms

Operational inspection stays external to the repository:

```bash
/private/tmp/shadow-rawnind-venv/bin/python \
  tools/neural-raw-denoise-rawnind/artifact_cli.py verify \
  /private/tmp/shadow-rawnind-provider-public-1d3-v2/.1d3-6400.shadowrawf

/private/tmp/shadow-rawnind-venv/bin/python \
  tools/neural-raw-denoise-rawnind/artifact_cli.py read-probe \
  /private/tmp/shadow-rawnind-provider-public-1d3-v2/.1d3-6400.shadowrawf \
  --y-start 540 --rows 36

/private/tmp/shadow-rawnind-venv/bin/python \
  tools/neural-raw-denoise-rawnind/artifact_cli.py recover \
  /private/tmp/.1d3-6400.shadowrawf.partial-crash \
  /private/tmp/1d3-6400.shadowrawf
```

## Tests

```bash
PYTHONDONTWRITEBYTECODE=1 \
SHADOW_TEST_RAWNIND_PACKAGE=/private/tmp/rawdenoise-nind.dtmodel \
/private/tmp/shadow-rawnind-venv/bin/python -m unittest -v
```

`SHADOW_TEST_RAWNIND_PACKAGE` is optional. When set, the tests include the
external 57.7 MB release asset without copying it into the worktree.
