# Editable Shadow documents

`.shdw` is the independent editor's self-contained document. Open an original or an
existing document with `Shadow --isolate /absolute/file`; a lone `.shdw` argument
also selects this entry. The independent window's file picker and drop target
accept documents, opening each document in a separate independent process.

**Save document**, **Save as**, and **Save and close** preserve the sources,
complete editable state, named versions, gesture history, and required local
resources. Raster export remains available separately. Closing discards changes
since the last explicit document save; session autosave alone is not a document
save. A saved document does not require its former original paths or temporary
session directory.

## Ownership and persistence

[`editable_document`](../../crates/shadow-desktop-bridge/src/editable_document.rs)
owns the ZIP contract, verification, publication, source relocation, and durable
history. Qt's [`pipeline_document.cpp`](../../apps/desktop/src/pipeline_document.cpp)
owns asynchronous save admission, persistence-before-export, and return to the
editor. [`EditableDocumentHistory`](../../apps/desktop/src/editable_document_history.hpp)
owns paged timeline browsing and same-photo restoration.

Ordinary independent input admission first copies the selected source into the
private session and verifies its bytes. Existing stdio and external-image stage
admission retain their established source-binding contracts. No source image is
modified. The normal Library, credentials, global preferences, models, provider
executables, and in-flight jobs are not document payloads.

Saving finishes the current gesture, waits for working-state persistence and the
serial history writer, and renders the frozen photo Recipes with the existing
durable export queue. Only then does a worker snapshot the Catalog, collect the
resources, write the ZIP, verify every member, and publish it. An unsuccessful
save preserves the previous document and reopens the live editing session.

Saving an opened document compares its previously observed SHA-256 at publication;
a changed document requires Save As. Per-user kernel locks serialize Shadow's
publication attempts. New destinations use exclusive publication; existing
documents use same-directory staging and atomic replacement. Files written by
uncoordinated external programs are detected by the digest check, which is not a
filesystem-wide compare-and-swap promise against arbitrary external writers.

## ZIP envelope, version 1

`manifest.json` identifies `format: "dev.shadow.document"`, `version: 1`, a stable
`document_id`, a new `revision` per save, `sources`, and `files`. Each file entry
contains a portable relative `path`, its uncompressed `bytes`, and `sha256`.
Each source declares `photo_id`, `representation_id`, `title`, original `path`,
current `render`, public `recipe` and `history` JSON paths, its Catalog location
identity, and the source timestamp used by fingerprint checks.

| Member | Role |
| --- | --- |
| `sources/<representation-id>.<extension>` | Exact original bytes |
| `renders/<photo-id>.png` | Full-resolution sRGB RGB8 current render, with the existing export ICC tag |
| `recipes/<photo-id>.json` | Complete immutable Recipe commits and their refs, under `dev.shadow.document-recipes/1` |
| `histories/<photo-id>.json` | All completed semantic steps, parents, cursor, and redo refs, under `dev.shadow.document-history/1` |
| `catalog.sqlite`, `document-history/history.sqlite` | Shadow's private persistence representation for exact restoration |
| `derived-rasters/objects/`, `cache/blobs/`, `cache/ai/raw-foundations/` | Managed resources, including adopted masks, completion patches, and available RAW denoise artifacts |
| `lut-store/`, `optics-profiles/` | Local editing resources |

The private Catalog is a consistent SQLite online backup, not a copied live WAL
file. Opening first verifies the complete member set and digests into an empty
private directory, validates the Catalog schema/integrity, binds every source
location to its extracted file, restores its source fingerprint timestamp, and
cancels nonterminal old output jobs. The current development version requires
the current Catalog schema; unsupported documents fail explicitly and stay intact.
Future schema migration must be implemented at this boundary.

The reader rejects unsupported versions, unknown envelope fields, missing or
extra members, duplicate/case-alias names, traversal paths, symlinks, encryption,
length/digest mismatches, and mismatched Catalog/source identities. Version 1
allows at most 256 sources, 100,000 members, 32 MiB of manifest JSON, and 64 GiB of
uncompressed payload. These are admission limits, not recommended document sizes.

## History and render equivalence

One completed slider/curve gesture produces one semantic step; repeated transient
values do not enter the document journal. The in-memory undo window remains
bounded at 256 entries, while the independent document journal retains older
steps and abandoned branches on disk. The **History** dialog loads older steps
in pages and can restore a branch to continue editing. A document also preserves
its current undo/redo cursor across saving and reopening.

This changes persistence, not an image algorithm. Gesture rendering reuses the
same source, CFA/Foundation state, warm sessions, and GPU resources. The journal
records value snapshots on a serial worker after a semantic action completes;
it adds no source decode, inference, image readback, or GPU upload. Saving adds
the existing full-resolution export materialization at its explicit consumer
boundary. Restoring history feeds the complete Recipe back through the existing
generation/cancellation-aware preview scheduler; preview, detail, and export
retain their established quality differences and Recipe meaning.

Originals and accepted results travel with the document. Re-editing still needs
a compatible Shadow renderer and the relevant installed source decoder; the ZIP
does not distribute private providers or model runtimes. Existing `.shadowrecipe`
cross-photo grading exchange remains a separate contract.

## External document stage

Call `Shadow --pipeline-edit --request /absolute/request.json --result
/absolute/new-result.json` with this exact request shape:

```json
{
  "schema": "shadow.document-edit/1",
  "requestId": "caller-session",
  "input": "/absolute/original.png",
  "inputSha256": "64 lowercase hexadecimal characters",
  "output": "/absolute/new-document.shdw"
}
```

Input may be a supported photo or `.shdw`. Output and result must be distinct new
files. The verified private input is bound to `inputSha256`. The human-visible
**Return editable document** button performs the same save use case as the
interactive editor. This request does not accept stdio tools or an operations
list; existing image-stage and stdio schemas retain their own contracts.

The result schema is `shadow.document-result/1`. It echoes `requestId` and
`inputSha256`, with `outcome` and `errors`. Only `completed` includes `artifact`
with `path`, `sha256`, decimal-string `byteLength`, and `format: "shdw"`. Exit codes
are 0 completed, 2 cancelled, and 1 failed. Invalid launch requests can fail before
creating a result; a missing terminal receipt is incomplete. Callers verify the
returned digest and decide independently whether to accept a new artifact.

Consumers can store the whole document, read the manifest's current render, and
send the file back to Shadow for further editing. Native editing by another
application requires implementing the relevant Recipe semantics.

`shadow-desktop-editable-document` exercises the packaged window's return button,
source-independent reopen, render equality, undo/redo, abandoned-branch restore,
corruption/traversal rejection, cancellation, digest conflicts, and Library
isolation. Archive/history invariants live beside the Rust owner.
