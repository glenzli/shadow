$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
& cargo run --manifest-path (Join-Path $repositoryRoot "Cargo.toml") -p xtask -- desktop-build-promote @args
exit $LASTEXITCODE
