# Library Server operations

Shadow Server is a separate application for sharing selected photo roots with authenticated Shadow
clients. It is bundled with the canonical debug build and has a lifecycle independent of the photo
editor.

## Graphical server

Build and promote the canonical debug applications first, then launch the server controller:

```sh
./scripts/build_and_promote_debug.sh
./scripts/run_library_server_debug.sh
```

The launcher returns after starting the controller. Output is appended to
`.shadow-local-build/logs/shadow-server-controller-debug.log`; pass `--foreground` to keep it
attached to the terminal. Use `./scripts/run_library_server_debug.sh --check` to inspect the
resolved controller without launching it.

The controller owns shared roots, access permission, startup policy, provider state, rescans,
cache maintenance, and listener lifetime. The main Shadow application does not start a competing
listener. Removing a shared root removes it from later manifests without deleting the source photo
or reusable server cache.

Access credentials remain in user-private local storage. They must not be printed, committed, or
passed through ordinary logs.

## Headless development server

For a direct development server, provide an explicit source root:

```sh
./scripts/run_library_server_debug.sh --headless /absolute/path/to/photos
```

Headless state lives outside the repository under `.shadow-local-library-server`. Append `--check`
to inspect resolved paths without starting the listener.

## Diagnostic CLI

Low-level `library-serve`, `library-sync`, and `library-materialize` commands remain available for
focused diagnostics. Their current argument contracts are maintained in the
[`shadow-cli` guide](../../apps/shadow-cli/README.md), while authenticated manifest and
materialization semantics are owned by
[`shadow-library-sharing`](../../crates/shadow-library-sharing/README.md).
