#!/bin/sh
# Fast, dependency-free transport preflight for the local-shared-workspace
# topology. Keep this shell entry point so the guard itself never causes Cargo
# to recreate target/ before it can inspect the source root.
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
blocked=0

for relative_path in target build local-reference/sample-assets local-reference/experiment-output; do
    candidate="$repository_root/$relative_path"
    if [ -L "$candidate" ]; then
        configured_target=$(readlink "$candidate")
        case "$configured_target" in
            /*) ;;
            *)
                printf '%s\n' "local workspace guard: guarded payload link $relative_path must use an absolute external target" >&2
                blocked=1
                continue
                ;;
        esac
        if [ ! -d "$configured_target" ]; then
            printf '%s\n' "local workspace guard: guarded payload link $relative_path must resolve to an existing external directory" >&2
            blocked=1
            continue
        fi
        canonical_target=$(CDPATH= cd -- "$configured_target" && pwd -P)
        case "$canonical_target" in
            "$repository_root"|"$repository_root"/*)
                printf '%s\n' "local workspace guard: guarded payload link $relative_path must point outside $repository_root" >&2
                blocked=1
                ;;
            *)
                printf '%s\n' "local workspace guard: accepting external local payload link $relative_path -> $canonical_target"
                ;;
        esac
        continue
    fi
    if [ -e "$candidate" ]; then
        size_kib=$(du -sk "$candidate" 2>/dev/null | awk 'NR == 1 { print $1 }')
        if [ "${size_kib:-0}" -gt 0 ]; then
            printf '%s\n' "local workspace guard: blocked payload $relative_path (${size_kib} KiB)" >&2
            blocked=1
        fi
    fi
done

validate_external_path() {
    variable_name=$1
    configured_path=$2
    if [ -n "$configured_path" ]; then
        case "$configured_path" in
            /*) ;;
            *)
                printf '%s\n' "local workspace guard: $variable_name must be absolute and outside the repository" >&2
                blocked=1
                return
                ;;
        esac
        case "$configured_path" in
            "$repository_root"|"$repository_root"/*)
                printf '%s\n' "local workspace guard: $variable_name must point outside $repository_root" >&2
                blocked=1
                ;;
        esac
    fi
}

validate_external_path CARGO_TARGET_DIR "${CARGO_TARGET_DIR-}"
validate_external_path SHADOW_BUILD_DIR "${SHADOW_BUILD_DIR-}"

if [ "$blocked" -ne 0 ]; then
    printf '%s\n' "Use an external task-private CARGO_TARGET_DIR and SHADOW_BUILD_DIR. Do not start another build until the payload is moved or its owner releases it." >&2
    exit 1
fi

printf '%s\n' "local workspace guard: passed; no guarded generated payload is present in $repository_root"
