# shadow-image

`shadow-image` is Shadow's platform-neutral C++20 image kernel. Its first implemented provider uses LibRaw, while callers depend only on the types in `include/shadow/image/decoder.hpp`.

## Decoder boundary

```text
DecoderProvider
└─ DecodeSession
   ├─ AssetMetadata
   ├─ DecodeCapabilities + PendingCorrections
   ├─ PreviewDescriptor[] → PreviewPayload
   ├─ MosaicBuffer
   └─ PixelBuffer (reference RGB only)
```

The public header does not expose LibRaw objects, enums, pointers, or ownership rules. A provider owns its decoder implementation; returned buffers own their memory and remain valid after subsequent session calls.

Current contract rules:

- A file without an embedded preview is valid and can still expose mosaic/RGB capabilities.
- Preview IDs are provider IDs, not vector positions. `select_best_preview` chooses the largest decodable candidate.
- DNG opcode lists are surfaced as `PendingCorrections` until Shadow can prove they were applied.
- `render_reference_rgb` is a correctness/fallback path. It is not Shadow's final scene-linear color pipeline.
- A `DecodeSession` is thread-confined. Providers may be shared; parallel work should open independent sessions.
- The current owned `MosaicBuffer` intentionally copies LibRaw memory. A later opaque/tiled buffer can remove that copy without changing metadata semantics.

## Build and test

```sh
cargo xtask native-check
```

This configures CMake, builds the library and probe, and runs CTest contract tests. Real RAW samples stay in the ignored local reference area; public deterministic fixtures will be added separately.
