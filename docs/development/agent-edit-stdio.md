# Single-photo edit tools over process pipes

Launch the desktop executable directly with connected stdin/stdout pipes:

```text
Shadow --agent-stdio --isolate /absolute/path/photo.jpg
```

This opt-in POSIX entry runs the ordinary independent editor and its existing EditController.
It accepts exactly one explicit source supported by the native photo router, including JPEG, bounded
opaque sRGB8 PNG and available RAW providers. PNG input rejects alpha, ICC/HDR overrides and compressed
metadata; see the [external stage contract](external-edit-stage.md) for its precise bounds. It does not attach to an already running Library, listen
on a socket, expose arbitrary Recipe JSON, execute models, or introduce a second draft owner.
Windows rejects the flag. Use a subprocess with both streams set to PIPE; diagnostics go to stderr.

The independent editor has a **temporary Catalog**. Commits and Undo are real within this session;
export a new file before shutdown. Restart creates a different session and rejects all old tokens.
A flattened PNG is an output artifact, not an editable-original or portable Recipe round trip.

## Material editing stage

`--pipeline-edit --request request.json --result result.json --agent-stdio` accepts the additive
[`shadow.external-edit/1` envelope](external-edit-stage.md). Human and AI use the same editor.
Discovery adds `externalStage` and `complete`; `complete` takes only the exact `expected` snapshot,
waits through the existing save/export lifecycle, and writes a terminal result file. Its immediate
`accepted` response is not a completion receipt. A neutral Recipe may be returned through this path.
`shutdown` cancels this bound stage and writes `cancelled`, whereas pipe EOF only detaches the client.
The normal `--isolate` shutdown/export contracts below are unchanged. Source binding for this profile
uses a verified private SHA-256 snapshot; it does not replace the editor's existing Recipe CAS.

## Wire contract

Each request and response is one UTF-8 JSON object followed by a newline. Requests have exactly
`schema`, `id`, `op`, and `params`. Schema is `shadow.edit-tools/1`; IDs are nonempty bounded strings.
Unknown fields, wrong types, non-finite/out-of-range adjustments, and requests over 64 KiB are rejected.
IDs correlate responses and do not provide durable replay or cross-restart idempotency. Reusing one
of the last 256 IDs is rejected. Responses can arrive out of order when cancelling a pending preview.

```json
{"schema":"shadow.edit-tools/1","id":"1","op":"discover","params":{}}
{"schema":"shadow.edit-tools/1","id":"2","op":"snapshot","params":{}}
```

Success returns `ok: true` and `result`; failure returns `ok: false` and `error.code/message`.
`discover` is the current supported operation/format/cancellation contract. While the selected photo
opens, snapshot returns not_ready; unsupported or corrupt input returns source_unavailable with the
admission diagnostic. An asynchronous history-open failure uses the same code and owner status. The client can still discover capabilities and shut down after that failure.
`snapshot` returns:

- `identity`: sessionId, snapshotId, photoId, representationId, baseCommitId, workingCommitId,
  activeVariantId, and the existing draftRevision encoded as a decimal string. Empty commit strings
  mean no commit; null does not. The opaque snapshot token also binds the full server-side stack,
  source size/mtime, photo generation, and owner invalidation epoch.
- `nodes`: IDs, labels, exposureStops, contrastFactor, saturationFactor, and availableEdits for each
  local enabled parameter with an existing render operation. The legacy editable flag still refers
  to exposure. Shared or disabled nodes expose no available edits.
- dirty, autosavePending, autosaveFailed, gestureActive, busy, canUndo, and sourceFingerprint.

Snapshot copies the current owner state. It does not finish a gesture, autosave, close the photo,
project a portable Recipe, or add history. Repeated snapshots reuse the same identity and reviewed
proposal while all existing owner/source/generation/draft/Variant guards still match. An actual
change captures a new identity and discards the old proposal. Observation never rebases an old
proposal onto a new human draft. Source fingerprint remains the existing size/mtime contract,
**not a content digest**. Discovery reports this lifecycle in proposalLifetime.

Copy the entire returned identity into `expected` below; never reconstruct it from a filename or
replace only its working head to force a stale request through.

| Operation | Exact params | Result / limit |
| --- | --- | --- |
| render_current | expected, outputPath | Current saved stack as a new JPEG, `expected` and `current`; no proposal, draft/ref/Undo mutation. Works on a neutral image. |
| preview | expected, operation **or** operations, outputPath | New JPEG artifact, proposalId and changes with previous/current values; preview max edge 1024. No draft/ref/Undo mutation. |
| apply | expected, proposalId | Exact photo/representation/Variant/commit/recipe/digest receipt, one Undo step. Requires the reviewed current proposal and a clean, idle human draft. |
| export | expected, outputPath | New full-resolution sRGB RGB8 PNG pinned to that committed Recipe. A neutral or historical draft must first be applied. |
| cancel | requestId | `preview` and `render_current`, accepted until artifact publication begins. Running apply/export return cancel_unsupported; finished requests return not_running. |
| shutdown | empty object | Requires a clean idle owner, reserves it, drains the final response, then exits. |

`render_current` is optional: check `discover.operations` before using it. Older producers do not
provide this operation; report that limitation instead of inventing a tiny adjustment to obtain pixels.
Like `preview`, it requires the exact current snapshot and a clean idle owner. It renders the captured
stack through the existing exact renderer, including human changes that the three editable fields do
not expose. It returns exactly `artifact`, `expected` and `current`; it has no applicable `proposalId`.
Observation success, failure or cancellation preserves a reviewed proposal while that proposal's
original snapshot remains current. Human/source changes still invalidate it. If `current` is false,
the artifact belongs to the returned old `expected` identity; obtain a fresh snapshot before acting.

For example, after copying the complete identity from `snapshot`, send a new correlated request:

```json
{"schema":"shadow.edit-tools/1","id":"observe-1","op":"render_current","params":{"expected":{"...":"copy the complete snapshot identity"},"outputPath":"/absolute/path/current-saved.jpg"}}
```

Use a separate new JPEG path. In an external material stage, neither a current image nor a proposal
preview may use the reserved output/result path, even if the result filename happens to end in `.jpg`.
Observation does not return or accept the material: `complete` and the unchanged completed result
remain the final stage handoff.

A singular `operation` remains compatible with earlier clients. `operations` accepts 1–16 typed
adjustments as one atomic proposal. Supply exactly one form; duplicate node/parameter pairs and
unknown fields are rejected. Supported absolute values match the existing editor controls:

| Type | Value field | Range | Neutral |
| --- | --- | --- | --- |
| set_exposure | stops | [-16,16] | 0 |
| set_contrast | factor | [0,8] | 1 |
| set_saturation | factor | [0,8] | 1 |

These are **absolute values on the addressed node**, not relative changes. For a node already at
`exposureStops: 0.75`, setting `stops: 1` replaces 0.75 with 1; it does not produce 1.75. Read current
values and `availableEdits` from the snapshot before constructing a proposal. Multiple Grade nodes,
mask strength and subsequent operations still determine the final image.

Exposure multiplies scene-linear RGB by `2^stops` at that operation. Contrast adjusts Oklab lightness
around the existing Recipe pivot; its factor is not a per-channel RGB slope. Saturation scales
Oklab chroma while retaining lightness; zero removes chroma at that node, and one is neutral. These
controls preserve existing Foundation, geometry, masks and other unaddressed settings. They do not
imply that the final image has no clipping, gamut loss or noise amplification.

For example, preview a combined proposal using the same exact snapshot identity and a fresh path:

```json
{"expected":{"...":"copy the complete snapshot identity"},"operations":[{"type":"set_exposure","nodeId":"captured-node-id","stops":0.75},{"type":"set_contrast","nodeId":"captured-node-id","factor":1.15},{"type":"set_saturation","nodeId":"captured-node-id","factor":0.8}],"outputPath":"/absolute/path/proposal.jpg"}
```

The existing Recipe graph determines rendering order. Every operation is validated against its
captured node before rendering or replacing a valid proposal. An invalid replacement preserves the
previous candidate; it cannot partially edit the owner. A valid replacement also preserves the
reviewed candidate until its render and new-file publication succeed. A failed or cancelled
replacement leaves the old proposal applicable only while its original snapshot remains current.
Successful replacement retires the previous proposal ID. An entirely unchanged proposal returns
invalid_operation and creates no history. A successful combination applies in one CAS transaction
and one Undo step. `changes` describes only fields whose values actually change.

Output paths must be absolute, have an existing directory, and use `.jpg`/`.jpeg` for previews or
`.png` for exports. Existing files and dangling symlinks are rejected; the common exporter also
publishes exclusively if another writer creates the destination after admission. Originals remain
read-only. A preview can finish after a human change: its artifact is preserved, but `current` is
false and no applicable proposal is returned. Cancellation acknowledged before publication leaves
no artifact. Rejected cancellation after publication began does not retract a completed file.

The server retains one snapshot, at most one reviewed candidate, one in-flight replacement, and
one tool operation. These are bounded Recipe/control snapshots, not retained rendered pixel
history. A stale identity returns stale_snapshot; a current but unsaved/gesturing draft returns draft_pending. It never saves,
rebases, or discards that human draft on behalf of a read/preview/apply request. A manual edit after
preview makes its proposal stale; capture a fresh snapshot after that edit settles to preserve it
in the next proposal. During rendering, apply and export, the same window shows the corresponding
localized activity. Cancellation and asynchronous preview/apply/export failures leave a localized
terminal message in that same status surface; technical details remain in the client reply.
Observation, language changes and ordinary autosave/status refreshes retain the terminal message.
The next admitted operation replaces it with progress, and a new human draft/Variant/photo dismisses
it. A late completion remains bound to the task's captured photo, draft revision and Variant: its
client still receives the result, but its old feedback cannot mask a newer human draft. Success
restores the ordinary render/autosave status retained beneath it. Admission rejections
that never started an operation do not overwrite the window's status.
Tool commits share
the existing Catalog working-head + active-Variant CAS. A publication receipt comes directly from
the committed immutable record; subsequent movable-head refresh is not its source of truth.

Human editing may continue during a frozen export. The independent editor serializes tool/GUI
export consumers and session close, preventing a second runner from stealing the first runner's
receipt. Export publication stays successful if optional output metadata readback fails; the reply
then contains published=true, path, commitId, and metadataUnavailable=true. Output metadata and
SHA-256, when available, are explicitly post-publication readback observations. External writers
can subsequently change a path, so this is not an immutable storage capability.

Input EOF or output backpressure failure detaches the client and preserves the actual editor window
and human work. Send shutdown and keep draining stdout for an orderly process exit. A crashed or
forcibly terminated client has no durable idempotent retry guarantee. The output buffer is bounded
at 256 KiB; an unresponsive peer cannot block the GUI indefinitely. No credentials or extra OS
permissions are required by the tools.

## Human handoff and recovery

Keep the process and both pipes open while the person uses the same editor. After a human adjustment,
wait for `dirty`, `autosavePending`, `autosaveFailed`, `gestureActive` and `busy` to clear in a fresh
snapshot. If saving failed, let the person resolve or retry that save; observation never silently
saves or discards their draft. Construct the next proposal from the new node values and copy its
whole identity. Use `render_current` to inspect that saved state without modifying it. A proposal
reviewed before the handoff cannot be applied to the new human state.

For a request such as “keep my white balance and make the second node a little darker,” the client
reads a fresh snapshot, checks that node's `availableEdits`, renders the current image, then proposes
an absolute exposure value derived from the returned value. It reviews the separate proposal JPEG
before `apply`, and obtains a fresh snapshot for `export`. The existing full-stack capture preserves
the human white balance; the client must not rebuild a Recipe from the three visible parameters.
Apply creates one Undo entry; the window's ordinary Undo/Redo controls keep the preceding human edit.
Export pins the selected committed Recipe even if the person continues editing during export.

| Response | Recovery |
| --- | --- |
| `not_ready`, `busy` | Wait with a bounded timeout, obtain a fresh snapshot, and retry with a new request ID. |
| `draft_pending` | Finish the human gesture/save and check `autosaveFailed`; do not force a commit or replace only the head token. |
| `stale_snapshot` | Re-read the current state and review a new proposal that preserves the human changes. |
| `invalid_operation` | Correct the node, range or output path; an unchanged proposal does not require a commit. |
| `preview_failed`, `cancelled` | For either image operation, inspect the failure and current identity; a previously reviewed proposal remains usable only if that same snapshot is still current. Use a new output path for a replacement. |
| `export_failed` | Inspect existing output and the diagnostic, then reconcile the result before retrying to a new path; never delete another writer's file. |
| `source_unavailable` | Check the admitted input and supported representation. Repeated polling does not repair a rejected format. |

The isolated session is temporary. A saved Recipe is durable only inside that session's Catalog;
export before ending it. Across process restarts, old snapshot/proposal/Recipe IDs are not resume
handles. The external material stage additionally verifies the input digest and provides its terminal
result; the calling application still owns Candidate import and explicit Accept.

## Reading preview and output quality

Tool previews are lossy sRGB JPEGs, at most 1024 pixels on the long edge, encoded at quality 95.
Exports are full-resolution sRGB RGB8 PNGs from the pinned Recipe, including spatial edits. The two
use the same authored adjustments but differ in resolution and encoding; JPEG bytes or individual
pixels are not a full-resolution quality proof. Review the exported image for fine detail and delivery
requirements. Eight-bit output does not preserve a RAW source's bit depth, sensor data or editable
Recipe, and an sRGB label is a color encoding description rather than a claim of scene-wide gamut.
PNG output currently carries the standard sRGB ICC tag; another bounded PNG editing stage requires
the explicit materialization described in the [external stage contract](external-edit-stage.md).

## Rendering and validation boundary

Exposure, contrast and saturation change the addressed Grade nodes and their downstream rendering.
One combined candidate is compiled and rendered once; array entries do not schedule separate previews. The complete captured
stack and immutable base preserve upstream Foundation, source development, masks, and private
Recipe meaning. Preview reuses the existing transient exact renderer and warm-source identities;
no durable preview blob is published. JPEG is an explicit host-byte consumer: GPU results, where
used, materialize at the existing bounded preview boundary and encode once for the external file.
There is no new host readback/upload loop. Full-resolution export uses the same exact Recipe
compiler through the established durable queue. Resolution/encoding differ; authored semantics do
not. Cancellation uses the existing render token plus a bounded publication-stage transition.

Owner/UI arbitration and the actual external pipe are separate tests:

```sh
ctest --test-dir "$SHADOW_BUILD_DIR" -R 'shadow-desktop-edit-tool-' --output-on-failure
```

The integration test launches the real stdio entry with synthetic JPEG pixels and no injected test
harness. It observes neutral and committed images, checks that observation preserves a reviewed
proposal and Undo state, and compares the reviewed JPEG with the same Recipe after apply. The external
stage test separately asserts all four exact terminal JSON object shapes for Shape compatibility and
rejects observation/preview attempts to occupy a JPEG-named terminal result, including composed and
decomposed Unicode aliases on macOS. The owner/UI test separately drives the same controller, holds
workers to test close/apply races, creates deterministic new-file publication collisions, and verifies
terminal English/Chinese feedback, failed/cancelled replacement preservation and export retry after a
human edit. Real window pointer events exercise packaged Undo/Redo controls for the complete composed
proposal and confirm that enabled-only history changes dismiss completed tool failures. The synthetic
handoff retains one reviewed AI proposal across current-image failure, accepted cancellation and retry,
then applies that original proposal, exports its committed version, and exercises Undo/Redo. A pending
observation also yields to a new human draft; the stale identity is rejected before observing the newly
saved state. A same-size gradient compares JPEG and PNG with a bounded lossy tolerance; this is a
renderer regression, not a general image-quality score. It runs offscreen by default; its script takes
the application and shadow-edit-tool-fixture executables, and accepts --visible and --evidence-root
for an explicit visible-window acceptance.
No new quiet-machine latency baseline is claimed by these correctness tests.
