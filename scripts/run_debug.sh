#!/bin/sh

set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
repository_parent=$(dirname -- "$repository_root")
local_build_root=${SHADOW_LOCAL_BUILD_ROOT:-"$repository_parent/.shadow-local-build"}
canonical_app="$local_build_root/current-debug/Shadow.app"
shadow_executable="$canonical_app/Contents/MacOS/Shadow"
decode_helper="$canonical_app/Contents/MacOS/shadow-image-decode-helper"
geonames_root="$canonical_app/Contents/Resources/GeoNames"
geonames_index="$geonames_root/shadow-geonames-cities-v1.tsv"
geonames_notice="$geonames_root/NOTICE.txt"
rawnind_provider_root="$canonical_app/Contents/Helpers/RawNIND"
rawnind_provider=${SHADOW_RAWNIND_PROVIDER_PATH:-"$rawnind_provider_root/shadow-rawnind-foundation-provider"}
rawnind_manifest=${SHADOW_RAWNIND_MANIFEST_PATH:-"$rawnind_provider_root/shadow-rawnind-foundation-model-manifest.json"}
rawnind_runtime="$(dirname -- "$rawnind_provider")/_rawnind_runtime"
user_home=${HOME:-}
if [ -z "$user_home" ]; then
    echo "Shadow cannot resolve its local model directory because HOME is empty." >&2
    exit 69
fi
rawnind_model_root="$user_home/Library/Application Support/Shadow/Shadow/models/rawnind-public-bayer-release-5.6.0"
rawnind_package=${SHADOW_RAWNIND_PACKAGE_PATH:-"$rawnind_model_root/rawdenoise-nind.dtmodel"}
rawnind_graph=${SHADOW_RAWNIND_BAYER_GRAPH_PATH:-"$rawnind_model_root/model_bayer.onnx"}

verify_rawnind_command_surface() {
    provider=$1
    provider_help=$("$provider" --help 2>&1) || return 1
    for required_option in \
        --model-package \
        --model-graph \
        --manifest \
        --input-raw \
        --input-raw-frame \
        --output-foundation \
        --source-pixel-contract-sha256 \
        --verify-model \
        --plan \
        --run
    do
        case "$provider_help" in
            *"$required_option"*) ;;
            *)
                echo "Shadow AI RAW Denoise provider is incompatible: missing $required_option." >&2
                return 1
                ;;
        esac
    done
}

if [ ! -x "$shadow_executable" ]; then
    echo "Shadow has no promoted canonical debug build." >&2
    echo "Expected: $canonical_app" >&2
    echo "A release steward must validate and run scripts/promote_debug_build.sh first." >&2
    exit 69
fi
if [ ! -x "$decode_helper" ]; then
    echo "Shadow canonical debug build is incomplete: RAW decode helper is missing." >&2
    echo "Expected: $decode_helper" >&2
    exit 69
fi
if [ ! -r "$geonames_index" ] || [ ! -r "$geonames_notice" ]; then
    echo "Shadow canonical debug build has no complete offline city data." >&2
    echo "Expected: $geonames_index" >&2
    echo "Rebuild with -DSHADOW_GEONAMES_CITY_INDEX_PATH=/absolute/index.tsv" >&2
    echo "and promote that complete Shadow.app again." >&2
    exit 69
fi
if [ ! -x "$rawnind_provider" ]; then
    echo "Shadow canonical debug build has no AI RAW Denoise provider." >&2
    echo "Expected: $rawnind_provider" >&2
    echo "Rebuild with -DSHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR=/absolute/provider/directory" >&2
    echo "and promote that complete Shadow.app again." >&2
    exit 69
fi
if [ ! -d "$rawnind_runtime" ] || [ ! -r "$rawnind_runtime/base_library.zip" ]; then
    echo "Shadow AI RAW Denoise provider runtime is incomplete." >&2
    echo "Expected: $rawnind_runtime" >&2
    exit 69
fi
if [ ! -r "$rawnind_manifest" ]; then
    echo "Shadow AI RAW Denoise model manifest is missing." >&2
    echo "Expected: $rawnind_manifest" >&2
    exit 69
fi
if [ ! -r "$rawnind_package" ]; then
    echo "Shadow AI RAW Denoise model package is missing." >&2
    echo "Expected: $rawnind_package" >&2
    exit 69
fi
if [ ! -r "$rawnind_graph" ]; then
    echo "Shadow AI RAW Denoise model graph is missing." >&2
    echo "Expected: $rawnind_graph" >&2
    exit 69
fi
if ! verify_rawnind_command_surface "$rawnind_provider"; then
    echo "Rebuild and promote the RawNIND provider from the current Shadow source." >&2
    exit 69
fi
if ! "$rawnind_provider" \
    --model-package "$rawnind_package" \
    --model-graph "$rawnind_graph" \
    --manifest "$rawnind_manifest" \
    --verify-model >/dev/null; then
    echo "Shadow AI RAW Denoise provider/model verification failed." >&2
    echo "Provider: $rawnind_provider" >&2
    echo "Model package: $rawnind_package" >&2
    echo "Model graph: $rawnind_graph" >&2
    exit 69
fi

if [ "${1:-}" = "--check" ]; then
    echo "canonical debug app: $canonical_app"
    echo "offline city index: $geonames_index"
    echo "AI RAW Denoise provider: $rawnind_provider"
    echo "AI RAW Denoise model package: $rawnind_package"
    echo "AI RAW Denoise model graph: $rawnind_graph"
    exit 0
fi

exec "$shadow_executable" "$@"
