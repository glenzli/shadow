# SAM 2.1 Core ML probe

This Mac-only probe validates one side-loaded Apple SAM 2.1 Small Core ML
artifact set before Shadow links it into the interactive editor. It compiles
and loads the image encoder, prompt encoder, and mask decoder, prints their
actual runtime contracts, then performs three one-point predictions against a
synthetic image while retaining the image embedding.

The probe does not download models, write inside the source tree, or register a
provider. Model packages and probe build output remain local payloads outside
the repository.

The currently validated artifact set is
[`apple/coreml-sam2.1-small`](https://huggingface.co/apple/coreml-sam2.1-small)
at Hugging Face revision `883f5787eb0be35ce6965907a8bc1f5320a5a02e`.
The model card declares Apache-2.0 and identifies the packages as a float16
conversion for the upstream
[`sam2-studio`](https://github.com/huggingface/sam2-studio) application.

Build and run with external directories:

```sh
cmake -S tools/sam2-coreml-probe \
  -B /tmp/shadow-sam2-coreml-probe-build \
  -G Ninja
cmake --build /tmp/shadow-sam2-coreml-probe-build
/tmp/shadow-sam2-coreml-probe-build/shadow-sam2-coreml-probe \
  "$HOME/Library/Application Support/Shadow/Models/apple/coreml-sam2.1-small" \
  /tmp/shadow-sam2-mask.pgm
```

The output is an 8-bit grayscale PGM made by applying a sigmoid to the best
256×256 mask logits. It is only compatibility evidence, not Shadow's durable
soft-mask encoding.

On the first run, Core ML may specialize its execution graph. Compare later
process runs and the second/third prompt timings before assessing interactive
latency.
