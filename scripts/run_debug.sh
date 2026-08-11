#!/bin/sh

set -eu
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
exec cargo run --manifest-path "$repository_root/Cargo.toml" -p xtask -- desktop-run-debug "$@"
