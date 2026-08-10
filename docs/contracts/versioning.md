# Contract versioning

Every independently evolving Shadow-owned schema, wire protocol, cache format, render contract,
and provider ABI advances with a local `YYYYMMDD.N` revision. The serialized integer form is
`YYYYMMDDNN`.

## Ownership

- Each contract owner advances only when its own compatibility surface changes.
- Same-day sequence numbers are assigned in source order for that owner.
- Unrelated owners do not share a global compatibility number.
- External standards and dependencies retain their upstream versions.
- An untouched legacy `v1` identity converts when that contract next changes; it does not receive a
  mechanical dated revision merely for consistency.

## Compatibility

During development, an incompatible revision may reset derived or rebuildable data and may require
both protocol peers to update together. Original photos are never modified, and user-authored state
must be migrated or explicitly backed up rather than silently discarded.

Compatible protocol additions use explicit capabilities. Numeric ordering alone never proves that
two peers are compatible. Before a stable release, every persistent owner must define readable and
writable ranges plus a migration policy.

The canonical revision value, supported range, migration behavior, and compatibility tests belong
with the schema or wire owner. This document defines the repository-wide policy; it is not a registry
of current revisions.
