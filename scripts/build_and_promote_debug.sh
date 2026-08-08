#!/bin/sh

# Build the complete, user-launchable debug product without building every desktop contract
# executable. Full `cargo xtask desktop-build` remains the all-target integration command; this
# script is the intentionally narrower canonical-debug release path.
set -eu

usage() {
    cat <<'EOF'
usage:
  ./scripts/build_and_promote_debug.sh [validation-label]
  ./scripts/build_and_promote_debug.sh --check

Builds only Shadow.app and its bundled Shadow Server, verifies their offscreen startup paths,
then atomically advances current-debug through promote_debug_build.sh.

Inputs are deliberately explicit because they are large local assets, not source-tree payloads:
  SHADOW_GEONAMES_CITY_INDEX_PATH              Prepared city index (optional if cached).
  SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR       Frozen RawNIND onedir provider (optional if cached).
  SHADOW_CANONICAL_DEBUG_ASSET_ROOT            Cache root containing geonames/ and rawnind/.
  SHADOW_CANONICAL_DEBUG_BUILD_DIR             External CMake directory for this candidate.
  SHADOW_CANONICAL_DEBUG_CARGO_TARGET_DIR      External Cargo target directory for the i18n gate.

When neither explicit path nor asset cache is available, an already promoted current-debug bundle
is used only as a read-only bootstrap source. A first build must provide the two explicit inputs.
EOF
}

fail() {
    echo "canonical debug build: $*" >&2
    exit 69
}

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
repository_parent=$(dirname -- "$repository_root")
local_build_root=${SHADOW_LOCAL_BUILD_ROOT:-"$repository_parent/.shadow-local-build"}
asset_root=${SHADOW_CANONICAL_DEBUG_ASSET_ROOT:-"$repository_parent/.shadow-local-assets/canonical-debug"}
build_directory=${SHADOW_CANONICAL_DEBUG_BUILD_DIR:-"$local_build_root/canonical-debug-build"}
cargo_target_directory=${SHADOW_CANONICAL_DEBUG_CARGO_TARGET_DIR:-"$repository_parent/.shadow-local-target/canonical-debug-xtask"}
current_app="$local_build_root/current-debug/Shadow.app"
city_index_name=shadow-geonames-cities-v1.tsv
provider_name=shadow-rawnind-foundation-provider

case "$build_directory" in
    /*) ;;
    *) fail "SHADOW_CANONICAL_DEBUG_BUILD_DIR must be an absolute path" ;;
esac
case "$cargo_target_directory" in
    /*) ;;
    *) fail "SHADOW_CANONICAL_DEBUG_CARGO_TARGET_DIR must be an absolute path" ;;
esac
case "$build_directory" in
    "$repository_root"|"$repository_root"/*) fail "candidate build directory must stay outside the source tree" ;;
esac
case "$cargo_target_directory" in
    "$repository_root"|"$repository_root"/*) fail "Cargo target directory must stay outside the source tree" ;;
esac

city_index=${SHADOW_GEONAMES_CITY_INDEX_PATH:-"$asset_root/geonames/$city_index_name"}
if [ ! -r "$city_index" ] && [ -r "$current_app/Contents/Resources/GeoNames/$city_index_name" ]; then
    city_index="$current_app/Contents/Resources/GeoNames/$city_index_name"
fi
provider_directory=${SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR:-"$asset_root/rawnind"}
if [ ! -x "$provider_directory/$provider_name" ] \
    && [ -x "$current_app/Contents/Helpers/RawNIND/$provider_name" ]; then
    provider_directory="$current_app/Contents/Helpers/RawNIND"
fi

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage
    exit 0
fi
if [ "${1:-}" = "--check" ]; then
    [ "$#" -eq 1 ] || fail "--check does not accept a validation label"
    [ -r "$city_index" ] || fail "no prepared GeoNames city index; set SHADOW_GEONAMES_CITY_INDEX_PATH"
    [ -x "$provider_directory/$provider_name" ] \
        || fail "no frozen RawNIND provider; set SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR"
    echo "candidate build directory: $build_directory"
    echo "GeoNames city index input: $city_index"
    echo "RawNIND provider input: $provider_directory"
    exit 0
fi
[ "$#" -le 1 ] || { usage >&2; exit 64; }
[ -r "$city_index" ] || fail "no prepared GeoNames city index; set SHADOW_GEONAMES_CITY_INDEX_PATH"
[ -x "$provider_directory/$provider_name" ] \
    || fail "no frozen RawNIND provider; set SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR"

validation_label=${1:-"canonical-debug-fast-$(git -C "$repository_root" rev-parse --short HEAD)"}
candidate_app="$build_directory/apps/desktop/Shadow.app"

CARGO_TARGET_DIR="$cargo_target_directory" SHADOW_BUILD_DIR="$build_directory" \
    sh "$repository_root/scripts/local_shared_workspace_guard.sh"
(
    cd "$repository_root"
    CARGO_TARGET_DIR="$cargo_target_directory" cargo xtask desktop-i18n-check
)
cmake --preset desktop-dev -B "$build_directory" \
    -DSHADOW_GEONAMES_CITY_INDEX_PATH="$city_index" \
    -DSHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR="$provider_directory"
cmake --build "$build_directory" --target shadow-desktop shadow-server-manager --parallel 6

ctest --test-dir "$build_directory" --output-on-failure \
    -R '^(shadow-desktop-qml-startup|shadow-server-manager-qml-startup)$'
"$repository_root/scripts/promote_debug_build.sh" "$candidate_app" "$validation_label"
"$repository_root/scripts/run_debug.sh" --check
