# RawNIND frozen desktop provider

This directory owns the distribution boundary for Shadow's audited RawNIND
RAW Foundation sidecar. The public model weights remain outside the application
bundle, but the executable provider must not depend on a developer or user
Python installation.

`build_provider.py` freezes the checked-in sidecar together with CPython,
NumPy, rawpy, and ONNX Runtime into one relocatable PyInstaller `onedir`
bundle. It rejects a changed Python minor version or dependency set, refuses
to write inside the repository or over an existing output, verifies the full
desktop command surface (including isolated `--input-raw-frame` planning),
optionally verifies the pinned public model, and emits a complete SHA-256
inventory in `build-receipt.json`.

The `onedir` layout is intentional. Shadow invokes verification, planning, and
materialization as separate processes. A one-file executable would extract the
95 MiB runtime on every invocation and can leave its temporary extraction
directory behind after a hard cancellation.

For desktop planning/materialization, Shadow's isolated decode helper first
projects the selected decoder's provider-neutral `RawFrame` into a short-lived
Bayer staging manifest. The frozen provider consumes that manifest through
`--input-raw-frame`, while the original RAW path remains the source provenance
identity. This lets the bundled model handle proprietary containers such as
Nikon HE without linking or rediscovering the private decoder. rawpy stays
pinned for direct public-dataset/audit invocations that omit the staging input.

## Build

Create the environment and all output outside the shared source tree:

```sh
python3.14 -m venv /private/tmp/shadow-rawnind-provider-venv
/private/tmp/shadow-rawnind-provider-venv/bin/python -m pip install \
  -r tools/neural-raw-denoise-rawnind/requirements.lock \
  -r apps/desktop/providers/rawnind-foundation/requirements-build.lock

/private/tmp/shadow-rawnind-provider-venv/bin/python \
  apps/desktop/providers/rawnind-foundation/build_provider.py \
  --output-root /private/tmp/shadow-rawnind-provider-build \
  --model-package /path/to/rawdenoise-nind.dtmodel \
  --model-graph /path/to/rawdenoise-nind/model_bayer.onnx
```

The optional model arguments do not copy weights into the provider. They make
the new frozen executable verify the exact package, graph, manifest, and ONNX
Runtime receipt before the build is accepted.

The deployable directory is:

```text
dist/shadow-rawnind-foundation-provider/
├── shadow-rawnind-foundation-provider
├── shadow-rawnind-foundation-model-manifest.json
└── _rawnind_runtime/
```

## Desktop bundle

Pass the deployable directory to the desktop configuration:

```sh
cmake -S . -B /private/tmp/shadow-desktop-release \
  -DSHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR=\
/private/tmp/shadow-rawnind-provider-build/dist/shadow-rawnind-foundation-provider
cmake --build /private/tmp/shadow-desktop-release --target shadow-desktop
```

CMake validates the onedir layout and copies the executable, its private
runtime directory, and the canonical manifest together. On macOS the intact
directory lives under `Shadow.app/Contents/Helpers/RawNIND/`; placing a
PyInstaller executable directly under `Contents/MacOS` would make its
bootloader resolve the main application's `Contents/Frameworks` instead of its
own runtime. Flat non-bundle installations keep the same three entries beside
the Shadow executable. Model weights remain under the versioned
application-data location documented by the desktop runtime:

```text
models/rawnind-public-bayer-release-5.6.0/
├── rawdenoise-nind.dtmodel
└── model_bayer.onnx
```

The final release signer must sign the completed application after the nested
provider and dylibs have been copied. A build without
`SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR` remains valid, but the AI RAW Denoise
surface reports the provider as unavailable instead of falling back to another
pixel path.

That optionality applies to ordinary application builds. Shadow's canonical
developer entry, `scripts/run_debug.sh`, intentionally represents the complete
debug product and therefore requires this provider, its private runtime, and
the side-loaded pinned model. `scripts/promote_debug_build.sh` verifies the
candidate and copied provider/model command surface before advancing
`current-debug`; `scripts/run_debug.sh --check` repeats that compatibility gate
and reports the resolved canonical paths without opening the application.

## Ownership

- `build_provider.py` owns the pinned, no-overwrite frozen-build contract and
  its external receipt.
- `requirements-build.lock` owns build-only dependency pins. Runtime pins
  remain in
  [`../../../../tools/neural-raw-denoise-rawnind/requirements.lock`](../../../../tools/neural-raw-denoise-rawnind/requirements.lock).
- `model-manifest.json` owns the exact public model artifact set.
- `install_runtime.cmake` owns the guarded, symlink-preserving copy of the
  PyInstaller private runtime into the application bundle.
- [`../../CMakeLists.txt`](../../CMakeLists.txt) owns only the short desktop
  bundle-copy seam.
