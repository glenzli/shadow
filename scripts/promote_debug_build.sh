#!/bin/sh

set -eu

usage() {
    echo "usage: $0 /absolute/path/to/Shadow.app validation-label" >&2
    exit 64
}

candidate_app=${1:-}
validation_label=${2:-}
if [ -z "$candidate_app" ] || [ -z "$validation_label" ]; then
    usage
fi
if [ "${candidate_app#/}" = "$candidate_app" ]; then
    echo "promote debug build: candidate app path must be absolute" >&2
    exit 64
fi

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
repository_parent=$(dirname -- "$repository_root")
local_build_root=${SHADOW_LOCAL_BUILD_ROOT:-"$repository_parent/.shadow-local-build"}
release_root="$local_build_root/releases/debug"
current_link="$local_build_root/current-debug"
promotion_lock="$local_build_root/.promote-debug-build.lock"

shadow_executable="$candidate_app/Contents/MacOS/Shadow"
decode_helper="$candidate_app/Contents/MacOS/shadow-image-decode-helper"
rawnind_provider_root="$candidate_app/Contents/Helpers/RawNIND"
rawnind_provider="$rawnind_provider_root/shadow-rawnind-foundation-provider"
rawnind_runtime="$rawnind_provider_root/_rawnind_runtime"
rawnind_manifest="$rawnind_provider_root/shadow-rawnind-foundation-model-manifest.json"
user_home=${HOME:-}
if [ -z "$user_home" ]; then
    echo "promote debug build: HOME is empty; cannot resolve the local model directory" >&2
    exit 65
fi
rawnind_model_root="$user_home/Library/Application Support/Shadow/Shadow/models/rawnind-public-bayer-release-5.6.0"
rawnind_package=${SHADOW_RAWNIND_PACKAGE_PATH:-"$rawnind_model_root/rawdenoise-nind.dtmodel"}
rawnind_graph=${SHADOW_RAWNIND_BAYER_GRAPH_PATH:-"$rawnind_model_root/model_bayer.onnx"}
if [ ! -d "$candidate_app" ] || [ ! -x "$shadow_executable" ]; then
    echo "promote debug build: candidate does not contain an executable Shadow app" >&2
    exit 65
fi
if [ ! -x "$decode_helper" ]; then
    echo "promote debug build: candidate does not contain the isolated RAW decode helper" >&2
    exit 65
fi
if [ ! -x "$rawnind_provider" ]; then
    echo "promote debug build: candidate does not contain the AI RAW Denoise provider" >&2
    echo "Expected: $rawnind_provider" >&2
    exit 65
fi
if [ ! -d "$rawnind_runtime" ] || [ ! -r "$rawnind_runtime/base_library.zip" ]; then
    echo "promote debug build: candidate contains an incomplete AI RAW Denoise runtime" >&2
    echo "Expected: $rawnind_runtime" >&2
    exit 65
fi
if [ ! -r "$rawnind_manifest" ]; then
    echo "promote debug build: candidate does not contain the AI RAW Denoise manifest" >&2
    echo "Expected: $rawnind_manifest" >&2
    exit 65
fi
if [ ! -r "$rawnind_package" ] || [ ! -r "$rawnind_graph" ]; then
    echo "promote debug build: local AI RAW Denoise model files are incomplete" >&2
    echo "Package: $rawnind_package" >&2
    echo "Graph: $rawnind_graph" >&2
    exit 65
fi
if ! "$rawnind_provider" \
    --model-package "$rawnind_package" \
    --model-graph "$rawnind_graph" \
    --manifest "$rawnind_manifest" \
    --verify-model >/dev/null; then
    echo "promote debug build: candidate AI RAW Denoise provider/model verification failed" >&2
    exit 65
fi
if [ -f "$candidate_app/Contents/Info.plist" ] && command -v plutil >/dev/null 2>&1; then
    plutil -lint "$candidate_app/Contents/Info.plist" >/dev/null
fi

mkdir -p "$release_root"
if [ -e "$current_link" ] && [ ! -L "$current_link" ]; then
    echo "promote debug build: canonical entry exists but is not a symlink: $current_link" >&2
    exit 73
fi
if ! mkdir "$promotion_lock" 2>/dev/null; then
    echo "promote debug build: another steward is already promoting the canonical build" >&2
    exit 73
fi

incoming_release=
next_link=
cleanup() {
    if [ -n "$next_link" ] && [ -L "$next_link" ]; then
        rm "$next_link"
    fi
    case "$incoming_release" in
        "$release_root"/.incoming-*)
            if [ -d "$incoming_release" ]; then
                rm -rf "$incoming_release"
            fi
            ;;
    esac
    rmdir "$promotion_lock" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

revision=$(git -C "$repository_root" rev-parse --short=12 HEAD)
promoted_at=$(date -u '+%Y%m%dT%H%M%SZ')
release_id="$revision-$promoted_at"
release_directory="$release_root/$release_id"
incoming_release="$release_root/.incoming-$release_id-$$"
if [ -e "$release_directory" ]; then
    echo "promote debug build: release already exists: $release_directory" >&2
    exit 73
fi

mkdir "$incoming_release"
ditto "$candidate_app" "$incoming_release/Shadow.app"

shadow_digest=$(shasum -a 256 "$shadow_executable" | awk '{print $1}')
helper_digest=$(shasum -a 256 "$decode_helper" | awk '{print $1}')
rawnind_provider_digest=$(shasum -a 256 "$rawnind_provider" | awk '{print $1}')
rawnind_manifest_digest=$(shasum -a 256 "$rawnind_manifest" | awk '{print $1}')
copied_shadow_digest=$(
    shasum -a 256 "$incoming_release/Shadow.app/Contents/MacOS/Shadow" | awk '{print $1}'
)
copied_helper_digest=$(
    shasum -a 256 \
        "$incoming_release/Shadow.app/Contents/MacOS/shadow-image-decode-helper" |
        awk '{print $1}'
)
copied_rawnind_provider="$incoming_release/Shadow.app/Contents/Helpers/RawNIND/shadow-rawnind-foundation-provider"
copied_rawnind_manifest="$incoming_release/Shadow.app/Contents/Helpers/RawNIND/shadow-rawnind-foundation-model-manifest.json"
copied_rawnind_provider_digest=$(shasum -a 256 "$copied_rawnind_provider" | awk '{print $1}')
copied_rawnind_manifest_digest=$(shasum -a 256 "$copied_rawnind_manifest" | awk '{print $1}')
if [ "$shadow_digest" != "$copied_shadow_digest" ] ||
    [ "$helper_digest" != "$copied_helper_digest" ] ||
    [ "$rawnind_provider_digest" != "$copied_rawnind_provider_digest" ] ||
    [ "$rawnind_manifest_digest" != "$copied_rawnind_manifest_digest" ]; then
    echo "promote debug build: copied application digest verification failed" >&2
    exit 74
fi
if ! "$copied_rawnind_provider" \
    --model-package "$rawnind_package" \
    --model-graph "$rawnind_graph" \
    --manifest "$copied_rawnind_manifest" \
    --verify-model >/dev/null; then
    echo "promote debug build: copied AI RAW Denoise provider/model verification failed" >&2
    exit 74
fi
worktree_digest=$(
    git -C "$repository_root" status --porcelain=v1 --untracked-files=all |
        shasum -a 256 |
        awk '{print $1}'
)
{
    echo "schema=shadow-canonical-debug-build-v1"
    echo "revision=$revision"
    echo "promoted_at_utc=$promoted_at"
    echo "validation=$validation_label"
    echo "source_app=$candidate_app"
    echo "worktree_status_sha256=$worktree_digest"
    echo "shadow_executable_sha256=$shadow_digest"
    echo "decode_helper_sha256=$helper_digest"
    echo "rawnind_provider_sha256=$rawnind_provider_digest"
    echo "rawnind_manifest_sha256=$rawnind_manifest_digest"
    echo "rawnind_model_verified=true"
} >"$incoming_release/build-manifest.txt"

mv "$incoming_release" "$release_directory"
incoming_release=

next_link="$local_build_root/.current-debug-$release_id-$$"
ln -s "releases/debug/$release_id" "$next_link"
mv -fh "$next_link" "$current_link"
next_link=

canonical_app="$current_link/Shadow.app"
if [ ! -x "$canonical_app/Contents/MacOS/Shadow" ] ||
    [ ! -x "$canonical_app/Contents/MacOS/shadow-image-decode-helper" ] ||
    [ ! -x "$canonical_app/Contents/Helpers/RawNIND/shadow-rawnind-foundation-provider" ] ||
    [ ! -d "$canonical_app/Contents/Helpers/RawNIND/_rawnind_runtime" ]; then
    echo "promote debug build: canonical link verification failed" >&2
    exit 74
fi

echo "canonical debug app: $canonical_app"
echo "run: $repository_root/scripts/run_debug.sh"
