#!/usr/bin/env python3
"""Build the audited RawNIND sidecar as a self-contained provider directory."""

from __future__ import annotations

import argparse
import hashlib
from importlib import metadata
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


PROVIDER_NAME = "shadow-rawnind-foundation-provider"
CONTENTS_DIRECTORY = "_rawnind_runtime"
EXPECTED_PYTHON = (3, 14)
EXPECTED_DISTRIBUTIONS = {
    "altgraph": "0.17.5",
    "flatbuffers": "25.12.19",
    "macholib": "1.16.4",
    "mpmath": "1.3.0",
    "numpy": "2.5.1",
    "onnxruntime": "1.24.4",
    "packaging": "26.2",
    "protobuf": "7.35.1",
    "pyinstaller": "6.21.0",
    "pyinstaller-hooks-contrib": "2026.6",
    "rawpy": "0.27.0",
    "setuptools": "83.0.0",
    "sympy": "1.14.0",
}

PROVIDER_DIRECTORY = Path(__file__).resolve().parent
REPOSITORY_ROOT = PROVIDER_DIRECTORY.parents[3]
ENTRY_POINT = (
    REPOSITORY_ROOT
    / "tools"
    / "neural-raw-denoise-rawnind"
    / "provider_sidecar.py"
)
MODEL_MANIFEST = PROVIDER_DIRECTORY / "model-manifest.json"
DESKTOP_MANIFEST_NAME = "shadow-rawnind-foundation-model-manifest.json"


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Freeze the pinned RawNIND Python/NumPy/rawpy/ONNX Runtime route "
            "into a relocatable onedir provider. The output root must be new "
            "and outside the Shadow source tree."
        )
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        required=True,
        help="new external directory for build intermediates, bundle, and receipt",
    )
    parser.add_argument(
        "--model-package",
        type=Path,
        help="optional pinned rawdenoise-nind.dtmodel used for post-build verification",
    )
    parser.add_argument(
        "--model-graph",
        type=Path,
        help="optional extracted model_bayer.onnx used for post-build verification",
    )
    return parser.parse_args()


def _validate_environment() -> dict[str, str]:
    if sys.version_info[:2] != EXPECTED_PYTHON:
        raise RuntimeError(
            "RawNIND provider packaging requires CPython "
            f"{EXPECTED_PYTHON[0]}.{EXPECTED_PYTHON[1]}.x, got "
            f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}"
        )
    actual = {
        distribution: metadata.version(distribution)
        for distribution in EXPECTED_DISTRIBUTIONS
    }
    changed = {
        distribution: (expected, actual[distribution])
        for distribution, expected in EXPECTED_DISTRIBUTIONS.items()
        if actual[distribution] != expected
    }
    if changed:
        details = ", ".join(
            f"{name}: expected {versions[0]}, got {versions[1]}"
            for name, versions in sorted(changed.items())
        )
        raise RuntimeError(f"RawNIND provider packaging environment changed: {details}")
    if not ENTRY_POINT.is_file() or not MODEL_MANIFEST.is_file():
        raise RuntimeError("RawNIND provider source or model manifest is missing")
    return actual


def _validate_output_root(requested: Path) -> Path:
    output_root = requested.expanduser().resolve(strict=False)
    if output_root == REPOSITORY_ROOT or output_root.is_relative_to(REPOSITORY_ROOT):
        raise RuntimeError("RawNIND provider output must remain outside the source tree")
    if output_root.exists():
        raise RuntimeError(f"RawNIND provider output already exists: {output_root}")
    output_root.mkdir(parents=True)
    return output_root


def _run_pyinstaller(output_root: Path) -> Path:
    environment = os.environ.copy()
    environment["PYINSTALLER_CONFIG_DIR"] = str(output_root / "config")
    command = [
        sys.executable,
        "-m",
        "PyInstaller",
        "--onedir",
        "--name",
        PROVIDER_NAME,
        "--contents-directory",
        CONTENTS_DIRECTORY,
        "--clean",
        "--noupx",
        "--log-level",
        "WARN",
        "--distpath",
        str(output_root / "dist"),
        "--workpath",
        str(output_root / "work"),
        "--specpath",
        str(output_root / "spec"),
        "--hidden-import",
        "rawpy",
        str(ENTRY_POINT),
    ]
    subprocess.run(
        command,
        cwd=REPOSITORY_ROOT,
        env=environment,
        check=True,
    )
    bundle = output_root / "dist" / PROVIDER_NAME
    executable = bundle / PROVIDER_NAME
    runtime = bundle / CONTENTS_DIRECTORY
    if not executable.is_file() or not runtime.is_dir():
        raise RuntimeError("PyInstaller did not produce the expected RawNIND onedir layout")
    shutil.copy2(MODEL_MANIFEST, bundle / DESKTOP_MANIFEST_NAME)
    help_probe = subprocess.run(
        [executable, "--help"],
        check=False,
        capture_output=True,
        text=True,
        timeout=60,
    )
    if help_probe.returncode != 0 or "--verify-model" not in help_probe.stdout:
        raise RuntimeError("frozen RawNIND provider failed its command-line probe")
    return bundle


def _verify_model(
    executable: Path,
    bundle_manifest: Path,
    model_package: Path | None,
    model_graph: Path | None,
) -> str | None:
    if (model_package is None) != (model_graph is None):
        raise RuntimeError("--model-package and --model-graph must be supplied together")
    if model_package is None or model_graph is None:
        return None
    command = [
        executable,
        "--model-package",
        str(model_package.resolve(strict=True)),
        "--model-graph",
        str(model_graph.resolve(strict=True)),
        "--manifest",
        str(bundle_manifest),
        "--verify-model",
    ]
    verification = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=180,
    )
    receipt = verification.stdout.strip()
    if (
        verification.returncode != 0
        or not receipt.startswith("shadow-rawnind-foundation-model-v1 ")
        or verification.stderr
    ):
        diagnostic = verification.stderr.strip() or receipt
        raise RuntimeError(f"frozen RawNIND model verification failed: {diagnostic}")
    return receipt


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _bundle_inventory(bundle: Path) -> list[dict[str, object]]:
    inventory: list[dict[str, object]] = []
    for path in sorted(bundle.rglob("*")):
        relative_path = path.relative_to(bundle).as_posix()
        if path.is_symlink():
            inventory.append(
                {
                    "path": relative_path,
                    "kind": "symlink",
                    "target": os.readlink(path),
                }
            )
        elif path.is_file():
            inventory.append(
                {
                    "path": relative_path,
                    "kind": "file",
                    "byte_len": path.stat().st_size,
                    "sha256": _sha256(path),
                }
            )
    return inventory


def _write_receipt(
    output_root: Path,
    bundle: Path,
    distributions: dict[str, str],
    model_verification: str | None,
) -> Path:
    receipt = {
        "schema": "shadow-rawnind-frozen-provider-build-v1",
        "provider_name": PROVIDER_NAME,
        "bundle_directory": bundle.name,
        "entry_point": str(ENTRY_POINT.relative_to(REPOSITORY_ROOT)),
        "python": platform.python_version(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "distributions": dict(sorted(distributions.items())),
        "model_verification": model_verification,
        "inventory": _bundle_inventory(bundle),
    }
    receipt_path = output_root / "build-receipt.json"
    receipt_path.write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return receipt_path


def main() -> int:
    arguments = _arguments()
    distributions = _validate_environment()
    output_root = _validate_output_root(arguments.output_root)
    bundle = _run_pyinstaller(output_root)
    model_verification = _verify_model(
        bundle / PROVIDER_NAME,
        bundle / DESKTOP_MANIFEST_NAME,
        arguments.model_package,
        arguments.model_graph,
    )
    receipt_path = _write_receipt(
        output_root,
        bundle,
        distributions,
        model_verification,
    )
    print(bundle)
    print(receipt_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
