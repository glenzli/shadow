---
name: shadow-interactive-rendering
description: Preserve Shadow's minimal-path responsive editing contract. Use whenever changing an interactive image adjustment, RAW development or rebinding, GPU/CPU render or fallback path, warm preview session/cache, node executor, presentation surface, or preview scheduling and cancellation.
---

# Shadow Interactive Rendering

## Overview

Use this skill before changing any path that turns an editor gesture into pixels. It keeps the
change source-faithful and responsive: the correct upstream image state is reused, only the
affected suffix is recalculated, and GPU work stays continuous where the backend supports it.

## Required reading

Read [`docs/development/interactive-editing-pipeline.md`](../../../docs/development/interactive-editing-pipeline.md)
before planning or editing. Then read the narrowest current owner:

- native RAW/rebinding: [`cpp/shadow-image/README.md`](../../../cpp/shadow-image/README.md) and
  the selected `src/raw/` or `src/proxy/` owner;
- bridge cache/render transaction: [`crates/shadow-desktop-bridge/README.md`](../../../crates/shadow-desktop-bridge/README.md)
  and the selected `src/edit_preview/` owner;
- desktop scheduling/presentation: [`apps/desktop/README.md`](../../../apps/desktop/README.md)
  and the selected controller/platform owner.

## Workflow

1. Classify the parameter: source-domain RAW, geometry/optics, node-grade/display, or a genuine
   source-plan/quality/foundation change.
2. State the minimal path before coding: reusable upstream inputs, exact invalidation frontier,
   CPU↔GPU transfers, cache/receipt identity changes, cancellation points, and the permitted
   preview-versus-detail/export difference.
3. Preserve the real algorithm. In particular, RAW white balance must keep its exact CFA/DCP path;
   never substitute post-demosaic RGB tinting merely to improve responsiveness.
4. Keep gesture work asynchronous and latest-value-wins. Interactive renders publish transient
   pixels only; analysis and durable preview publication remain settled/commit work.
5. Reuse resident device sources and side resources when valid. A host materialization followed by
   an upload is a named boundary to justify or remove, never an invisible implementation detail.
6. Validate at the narrow owner and real consumer boundary. Apply the repository validation budget:
   do not rerun broad builds or unrelated suites when the changed contract has focused evidence.

## Performance evidence and cache discipline

- Distinguish a cold path from a steady-state gesture. The first visible frame may prepare a bounded
  source or warm session; repeated changes to the same photo and semantic source must not pay that
  preparation cost again. Measure and report both separately when responsiveness is under review.
- Keep semantic source identity separate from presentation policy. A preview edge, JPEG quality, or
  viewport shape identifies a rendered result, but does not by itself invalidate reusable
  sensor-domain state. Coordinate-space tools such as the RAW white-balance picker may reuse a
  compatible retained source across presentation sizes; rendered-frame reuse still needs its exact
  output identity.
- When latency is unclear, add a temporary, opt-in timing trace with one gesture/revision identity
  and bounded phase names: cache admission, source rebind, device adoption or host materialization,
  recipe execution, encoding, and publish. Do not log image bytes, paths beyond existing diagnostics,
  or per-pixel data. Remove or keep the probe only when it remains a useful maintained diagnostic.
- Optimize the measured expensive phase, not the symptom. Do not hide a slow CFA/DCP, geometry, or
  GPU-transfer boundary by reducing quality, changing colour semantics, using provider RGB, or
  treating a failed rebind as a successful preview.
- A warm-source cache match must name both sides of its contract: fields that must be identical for
  semantic correctness, and narrowly proved fields that are deliberately rebindable. Do not make
  dimensions, UI policy, or a previous result cache key accidental gates on a reusable source.

## Non-negotiable checks

- A parameter may not reopen the source, rerun denoise, reload a Foundation artifact, or rebuild an
  unrelated source stage unless its declared input domain actually changed.
- A GPU path is only continuous when source, resources, and output remain on the device until an
  explicit consumer boundary. Do not call it GPU-resident after an avoidable readback/upload pair.
- Preview reuse requires matching source and receipt semantics. Fail explicitly instead of quietly
  switching provider, color, or geometry policy.
- An old task must never publish over a newer revision. Memory residency must be bounded and owned.
- Any remaining full-path rebuild or host/device seam is performance debt: record the reason and
  the condition needed to remove it.
- Before declaring an edit responsive, confirm the measured steady-state path, not only a successful
  first render: no source reopen, no repeated denoise/Foundation work, no avoidable CPU↔GPU round
  trip, and no analysis or durable-cache publication on the interactive event.
