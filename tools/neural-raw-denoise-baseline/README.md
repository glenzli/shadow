# Neural RAW denoise synthetic baseline

This development tool closes the first executable checkpoint loop for
`shadow-image`'s optional neural RAW stage. It procedurally generates normalized
R/Gr/Gb/B planes, applies calibrated per-plane Poisson-Gaussian noise, trains a
small noise-conditioned residual CNN, converts it to Core ML, compiles a
`.mlmodelc`, computes Shadow's exact tree identity, and measures held-out
synthetic quality and Core ML cold/warm prediction latency.

It is intentionally **not a product model**:

- no real camera or RAW corpus participates in training or evaluation;
- the checkpoint is not evidence of cross-camera quality or generalization;
- it remains side-loaded and must not be bundled, auto-downloaded, exposed in
  UI, or selected by default;
- Shadow's conventional RAW denoise remains the real fallback.

The owner is separate from
`cpp/shadow-image/src/raw/neural_raw_denoise/`: this directory owns checkpoint
generation and synthetic benchmark policy, while the native owner keeps model
admission, packing, tiling, execution, fallback, and provenance.

## Reproduce

Use Python 3.12 and install the pinned direct dependencies in a disposable
environment outside the repository:

```sh
/opt/homebrew/bin/python3.12 -m venv /private/tmp/shadow-neural-raw-venv
/private/tmp/shadow-neural-raw-venv/bin/pip install \
  -r tools/neural-raw-denoise-baseline/requirements.lock
/private/tmp/shadow-neural-raw-venv/bin/python \
  tools/neural-raw-denoise-baseline/baseline.py \
  --output-dir /private/tmp/shadow-neural-raw-checkpoint
```

The output directory must be absolute, empty, and outside the repository. It
receives the Torch state checkpoint, source `.mlpackage`, compiled `.mlmodelc`,
and `report.json`. The report records a canonical learned-state digest separately
from the device-compiled Core ML tree digest: repeat training must preserve the
former, while the latter identifies the exact admitted local runtime cache. The
report is fail-closed: the command exits `2` unless the Core ML result improves
held-out synthetic PSNR, retains edge quality, improves flat-region quality, and
changes measurably when only the calibrated noise tensor changes. It must also
agree with the Torch reference under Shadow-equivalent tiling.

Run the focused tool contracts with:

```sh
cd tools/neural-raw-denoise-baseline
/private/tmp/shadow-neural-raw-venv/bin/python -m unittest -v test_baseline.py
```

Then use the four `SHADOW_TEST_NEURAL_RAW_DENOISE_*` values printed by the tool
to run `shadow-image-neural-raw-denoise-contract`. That second gate proves the
compiled checkpoint is executable through Shadow's native Core ML adapter; the
Python report alone is insufficient.

## Promotion boundary

A future checkpoint may be considered for product work only after a separate,
rights-cleared real-camera program adds held-out cameras, exact artifact and
dataset terms, sensor- and post-color metrics, texture/fine-detail failure
sets, seam/memory/cancellation pressure, and full-resolution latency. Passing
this synthetic baseline does not satisfy any of those gates.
