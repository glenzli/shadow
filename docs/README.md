# Shadow developer documentation

This directory is the maintained entry point for developers and reviewers. It routes a task to the
narrowest durable owner without duplicating the implementation that already lives in source and
tests.

## Start here

| Need | Entry |
| --- | --- |
| Find the subsystem that owns a change | [Architecture and repository map](architecture/README.md) |
| Build, test, format, or launch Shadow | [Development guide](development/README.md) |
| Change a persistent or wire contract | [Contract versioning](contracts/versioning.md) |
| Run the local Library Server | [Library Server operations](operations/library-server.md) |
| Inspect repository-wide engineering constraints | [Project Skeleton](../SKELETON.md) |
| Review a change | [Review Skeleton](../REVIEW_SKELETON.md) |
| Coordinate work in the shared checkout | [Agent instructions](../AGENTS.md) |

## Navigation model

Documentation is intentionally layered:

1. The root [Project Skeleton](../SKELETON.md) supplies durable boundaries and routes to a concern.
2. The root [README](../README.md) introduces the product and the shortest working entry points.
3. This page routes developers to architecture, development, contract, and operations documents.
4. The [repository map](architecture/README.md) routes a change to one subsystem.
5. A component README or entry module routes the change to its semantic owner.
6. Source, schemas, build registration, and adjacent tests define current behavior.

Do not turn this directory into an exhaustive mirror of the codebase. A cross-cutting policy or
operator workflow belongs here only when it has an independent audience and lifecycle. Feature
status, call-by-call mechanics, and implementation inventories stay with their source owners.
