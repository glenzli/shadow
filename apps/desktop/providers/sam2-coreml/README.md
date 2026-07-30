# SAM 2.1 Core ML provider

This macOS-only executable is Shadow's local process boundary for a
side-loaded SAM 2.1 Small Core ML model set. Shadow invokes and packages the
provider, but model packages remain outside the application and source tree.
It never downloads, copies, or redistributes model files.

The first protocol version accepts one rendered JPEG, normalized foreground or
background prompt points, and writes a tightly packed 256×256 unsigned
grayscale soft mask. The byte values are sigmoid probabilities rather than a
thresholded binary selection.

The locally validated Apple package accepts multiple prompt points even though
Core ML's human-readable feature description prints the default one-point
shape. Shadow therefore treats an actual two-point foreground/background
prediction as the compatibility gate instead of relying on that description.

```sh
shadow-sam2-coreml-provider \
  --model-dir "/path/to/coreml-sam2.1-small" \
  --manifest "/path/to/model-manifest.json" \
  --input-jpeg "/tmp/shadow-preview.jpg" \
  --output-mask "/tmp/shadow-mask.gray8" \
  --point 0.5 0.5 foreground \
  --point 0.75 0.5 background
```

The one-shot form remains a diagnostic and compatibility surface. On success,
stdout contains one bounded receipt:

```text
shadow-sam2-coreml-mask-v1 width=256 height=256 score=0.987500 points=2
```

Diagnostics are written to stderr. The caller owns the input and output
payload paths.

## Resident product protocol

Shadow normally launches the provider once per admitted desktop runtime:

```sh
shadow-sam2-coreml-provider \
  --model-dir "/path/to/coreml-sam2.1-small" \
  --manifest "/path/to/model-manifest.json" \
  --serve
```

`--serve` uses a bounded JSON-lines protocol on stdin/stdout. Protocol v1
supports `load_image`, `predict`, and `shutdown`. `load_image` binds a rendered
JPEG to its caller-supplied content identity and computes the image embedding
only when that identity changes. Repeated foreground/background prompt
refinements call only the prompt encoder and mask decoder. Shadow serializes
requests, terminates the child on cancellation, and retries one complete
load-and-predict request after a transport failure.

Stdout is reserved for protocol frames. Core ML framework diagnostics and all
provider diagnostics are redirected to stderr so they cannot corrupt the
session. A command is limited to 64 KiB and a prediction accepts 1–16 points,
including at least one foreground point.

The first local real-model acceptance on the development Mac measured about
304 ms to encode the display JPEG and 203–229 ms for subsequent point
predictions. Those numbers are diagnostic evidence for that machine and image,
not a performance guarantee.

Before admission, Shadow runs the same binary in verification mode:

```sh
shadow-sam2-coreml-provider \
  --model-dir "/path/to/coreml-sam2.1-small" \
  --manifest "/path/to/model-manifest.json" \
  --verify-model
```

Both verification and inference hash all nine executable `.mlpackage` members
against the pinned manifest. A mixed or modified encoder/prompt/decoder set is
rejected before Core ML compilation.

Build output must remain outside the shared source tree:

```sh
cmake -S apps/desktop/providers/sam2-coreml \
  -B /tmp/shadow-sam2-coreml-provider-build \
  -G Ninja
cmake --build /tmp/shadow-sam2-coreml-provider-build
```

For an opt-in end-to-end Rust/desktop acceptance, set the local provider,
model, manifest, and one display-JPEG fixture:

```sh
SHADOW_SAM2_COREML_PROVIDER_PATH=/tmp/shadow-sam2-coreml-provider-build/shadow-sam2-coreml-provider \
SHADOW_SAM2_COREML_MODEL_DIR="/path/to/coreml-sam2.1-small" \
SHADOW_SAM2_COREML_MANIFEST_PATH="$PWD/apps/desktop/providers/sam2-coreml/model-manifest.json" \
SHADOW_SAM2_COREML_ACCEPTANCE_INPUT_JPEG="/path/to/display-preview.jpg" \
cargo test -p shadow-desktop-bridge \
  configured_real_route_stages_distinct_point_refinements -- --nocapture
```

## Source ownership

- `main.mm` owns command-line parsing and exact model-manifest verification.
- `sam2_coreml_engine.*` owns compiled Core ML models and the one-image
  embedding cache.
- `resident_server.*` owns the bounded JSON-lines transport.
