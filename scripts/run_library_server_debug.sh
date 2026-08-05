#!/bin/sh

set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
repository_parent=$(dirname -- "$repository_root")
local_build_root=${SHADOW_LOCAL_BUILD_ROOT:-"$repository_parent/.shadow-local-build"}
canonical_app="$local_build_root/current-debug/Shadow.app"
server_app="$canonical_app/Contents/Applications/Shadow Server.app"
server_executable="$server_app/Contents/MacOS/Shadow Server"
debug_log_root=${SHADOW_DEBUG_LOG_ROOT:-"$local_build_root/logs"}
server_controller_log="$debug_log_root/shadow-server-controller-debug.log"

usage() {
    cat <<'EOF'
usage:
  ./scripts/run_library_server_debug.sh
  ./scripts/run_library_server_debug.sh --foreground
  ./scripts/run_library_server_debug.sh --check
  ./scripts/run_library_server_debug.sh --headless <shared-folder> [options]

default control mode:
  Launches the standalone Shadow Server controller from the canonical debug
  bundle. It configures, starts, stops, and rescans this Mac's Library server
  without opening the photo editor. The controller starts in the background;
  use --foreground to keep it attached to the terminal.

headless options:
  --bind <address:port>   Listener address (default: 0.0.0.0:37641)
  --name <display-name>   Name shown to Shadow clients
  --provider-host <path>  Use an explicit RAW Provider Host helper
  --public-only           Disable the private Provider Host fallback
  --check                 Print the resolved headless configuration without starting
  -h, --help              Show this help

examples:
  ./scripts/run_library_server_debug.sh
  ./scripts/run_library_server_debug.sh --headless /Volumes/Photos/RAW
  ./scripts/run_library_server_debug.sh --headless /Volumes/Photos/RAW \
      --bind 0.0.0.0:38641 --name "Studio Mac"

Headless mode creates an access token and rebuildable server data under the sibling
.shadow-local-library-server directory. Press Ctrl-C to stop a headless server.
EOF
}

if [ "${1:-}" != "--headless" ]; then
    case "${1:-}" in
        "")
            if [ ! -x "$server_executable" ]; then
                echo "Shadow Server is not present in the canonical debug build." >&2
                echo "Expected: $server_app" >&2
                exit 69
            fi
            mkdir -p "$debug_log_root"
            nohup "$server_executable" >>"$server_controller_log" 2>&1 </dev/null &
            server_controller_pid=$!
            echo "Shadow Server controller started in the background (pid $server_controller_pid)."
            echo "log: $server_controller_log"
            ;;
        --foreground)
            [ "$#" -eq 1 ] || { echo "--foreground does not accept control-mode arguments" >&2; exit 64; }
            if [ ! -x "$server_executable" ]; then
                echo "Shadow Server is not present in the canonical debug build." >&2
                echo "Expected: $server_app" >&2
                exit 69
            fi
            exec "$server_executable"
            ;;
        --check)
            [ "$#" -eq 1 ] || { echo "--check does not accept extra control-mode arguments" >&2; exit 64; }
            "$repository_root/scripts/run_debug.sh" --check
            if [ ! -x "$server_executable" ]; then
                echo "Shadow Server is not present in the canonical debug build." >&2
                echo "Expected: $server_app" >&2
                exit 69
            fi
            echo "standalone server controller: $server_app"
            exit 0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "control mode takes no folder; use --headless before a shared folder" >&2
            usage >&2
            exit 64
            ;;
    esac
    exit 0
fi
shift

server_debug_root=${SHADOW_LIBRARY_SERVER_DEBUG_ROOT:-"$repository_parent/.shadow-local-library-server"}
cargo_target_root=${SHADOW_LIBRARY_SERVER_CARGO_TARGET_DIR:-"$repository_parent/.shadow-local-target/library-server-debug"}
canonical_helper="$local_build_root/current-debug/Shadow.app/Contents/MacOS/shadow-image-decode-helper"
bind_address=${SHADOW_LIBRARY_SERVER_BIND:-"0.0.0.0:37641"}
display_name=${SHADOW_LIBRARY_SERVER_NAME:-"$(hostname -s 2>/dev/null || hostname) · Shadow Library Debug"}
provider_host=${SHADOW_DECODE_HELPER_PATH:-}
provider_host_required=0
public_only=0
check_only=0
shared_folder=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --bind)
            [ "$#" -ge 2 ] || { echo "--bind requires a value" >&2; exit 64; }
            bind_address=$2
            shift 2
            ;;
        --name)
            [ "$#" -ge 2 ] || { echo "--name requires a value" >&2; exit 64; }
            display_name=$2
            shift 2
            ;;
        --provider-host)
            [ "$#" -ge 2 ] || { echo "--provider-host requires a path" >&2; exit 64; }
            provider_host=$2
            provider_host_required=1
            shift 2
            ;;
        --public-only)
            public_only=1
            shift
            ;;
        --check)
            check_only=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            echo "unknown headless option: $1" >&2
            usage >&2
            exit 64
            ;;
        *)
            if [ -n "$shared_folder" ]; then
                echo "only one shared folder may be supplied" >&2
                usage >&2
                exit 64
            fi
            shared_folder=$1
            shift
            ;;
    esac
done

if [ -n "${SHADOW_DECODE_HELPER_PATH:-}" ]; then
    provider_host_required=1
fi

if [ -z "$shared_folder" ]; then
    echo "a shared folder is required in headless mode" >&2
    usage >&2
    exit 64
fi
if [ ! -d "$shared_folder" ]; then
    echo "shared folder is unavailable: $shared_folder" >&2
    exit 66
fi
shared_folder=$(CDPATH= cd -- "$shared_folder" && pwd)

case "$bind_address" in
    *:*) ;;
    *)
        echo "bind address must include a port: $bind_address" >&2
        exit 64
        ;;
esac

if [ "$public_only" -eq 1 ]; then
    provider_host=
elif [ -z "$provider_host" ] && [ -x "$canonical_helper" ]; then
    provider_host=$canonical_helper
fi
if [ -n "$provider_host" ]; then
    provider_inventory=
    if [ -x "$provider_host" ]; then
        provider_inventory=$("$provider_host" provider-inventory 2>/dev/null || true)
    fi
    case "$provider_inventory" in
        "shadow-provider-host-v1 provider-inventory "*) ;;
        *)
            if [ "$provider_host_required" -eq 1 ]; then
                echo "RAW Provider Host is missing the current provider-inventory contract: $provider_host" >&2
                exit 69
            fi
            echo "warning: canonical RAW Provider Host is incompatible; using public LibRaw only" >&2
            provider_host=
            ;;
    esac
fi

catalog_path="$server_debug_root/catalog.sqlite"
preview_cache="$server_debug_root/preview-cache"
server_state="$server_debug_root/state"
token_file=${SHADOW_LIBRARY_SERVER_TOKEN_FILE:-"$server_debug_root/access-token"}

echo "Shadow Library headless server debug"
echo "  shared folder: $shared_folder"
echo "  listener: $bind_address"
echo "  display name: $display_name"
echo "  server data: $server_debug_root"
echo "  access token: $token_file"
if [ -n "$provider_host" ]; then
    echo "  RAW provider: $provider_host"
else
    echo "  RAW provider: public LibRaw only"
fi

if [ "$check_only" -eq 1 ]; then
    exit 0
fi

sh "$repository_root/scripts/local_shared_workspace_guard.sh"
umask 077
token_parent=$(dirname -- "$token_file")
mkdir -p "$server_debug_root" "$cargo_target_root" "$token_parent"
if [ ! -s "$token_file" ]; then
    if ! command -v openssl >/dev/null 2>&1; then
        echo "openssl is required to create the Library access token" >&2
        exit 69
    fi
    openssl rand -hex 32 >"$token_file"
    echo "Created a new access token. Keep this file private."
fi
chmod 600 "$token_file"

export CARGO_TARGET_DIR="$cargo_target_root"
if [ -n "$provider_host" ]; then
    export SHADOW_DECODE_HELPER_PATH="$provider_host"
else
    unset SHADOW_DECODE_HELPER_PATH 2>/dev/null || true
fi

cd "$repository_root"
exec cargo run --package shadow-cli -- \
    library-serve \
    "$catalog_path" \
    "$preview_cache" \
    "$shared_folder" \
    "$server_state" \
    "$bind_address" \
    "$token_file" \
    "$display_name"
