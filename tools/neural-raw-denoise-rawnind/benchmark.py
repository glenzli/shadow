#!/usr/bin/env python3
"""Run and aggregate the pinned RawNIND known-sensor quality pilot."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import statistics
from typing import Any, Callable

import dataset_manifest
import public_pair
import quality


SCHEMA = "shadow-rawnind-known-sensor-benchmark-v1"
MIN_PAIR_PSNR_DELTA_DB = 0.5
MIN_MEDIAN_PSNR_DELTA_DB = 3.0


@dataclass(frozen=True)
class EvaluatedPair:
    scene: str
    camera_make: str
    camera_model: str
    noisy_iso: int
    quality: quality.PairQualityReceipt


def _public_raw(value: dict[str, Any]) -> public_pair.PublicRawFile:
    required = {
        "role",
        "dataverse_file_id",
        "persistent_id",
        "filename",
        "size_bytes",
        "md5",
    }
    if set(value) != required:
        raise ValueError("RawNIND manifest RAW identity schema mismatch")
    return public_pair.PublicRawFile(**value)


def evaluate_manifest(
    manifest: dict[str, Any],
    model_path: Path,
    raw_root: Path,
    requested_provider: str,
    preview_root: Path | None = None,
    evaluator: Callable[..., quality.PairQualityReceipt] = (
        quality.evaluate_public_pair
    ),
) -> list[EvaluatedPair]:
    dataset_manifest.validate_manifest(manifest)
    if preview_root is not None:
        preview_root.mkdir(parents=True, exist_ok=False)
    evaluated: list[EvaluatedPair] = []
    for pair in manifest["pairs"]:
        noisy_identity = _public_raw(pair["noisy"])
        ground_truth_identity = _public_raw(pair["ground_truth"])
        noisy_path = raw_root / noisy_identity.filename
        ground_truth_path = raw_root / ground_truth_identity.filename
        preview_path = (
            preview_root / f"{pair['scene']}.ppm"
            if preview_root is not None
            else None
        )
        receipt = evaluator(
            model_path,
            noisy_path,
            ground_truth_path,
            requested_provider,
            preview_path,
            expected_noisy=noisy_identity,
            expected_ground_truth=ground_truth_identity,
        )
        evaluated.append(
            EvaluatedPair(
                scene=pair["scene"],
                camera_make=pair["camera_make"],
                camera_model=pair["camera_model"],
                noisy_iso=pair["noisy_iso"],
                quality=receipt,
            )
        )
    return evaluated


def aggregate(
    manifest: dict[str, Any],
    pairs: list[EvaluatedPair],
) -> dict[str, Any]:
    dataset_manifest.validate_manifest(manifest)
    expected_scenes = [pair["scene"] for pair in manifest["pairs"]]
    actual_scenes = [pair.scene for pair in pairs]
    if actual_scenes != expected_scenes:
        raise ValueError("RawNIND evaluated pair order or membership mismatch")
    deltas = [pair.quality.psnr_delta_db for pair in pairs]
    ratios = [pair.quality.mse_ratio for pair in pairs]
    if not deltas or not all(math.isfinite(value) for value in deltas + ratios):
        raise ValueError("RawNIND benchmark metrics must be finite")

    median_delta = statistics.median(deltas)
    failures: list[str] = []
    for pair in pairs:
        if not pair.quality.improved_on_probe:
            failures.append(f"{pair.scene}: denoised MSE did not improve")
        if pair.quality.psnr_delta_db < MIN_PAIR_PSNR_DELTA_DB:
            failures.append(
                f"{pair.scene}: PSNR delta "
                f"{pair.quality.psnr_delta_db:.3f} dB is below "
                f"{MIN_PAIR_PSNR_DELTA_DB:.3f} dB"
            )
    if median_delta < MIN_MEDIAN_PSNR_DELTA_DB:
        failures.append(
            f"median PSNR delta {median_delta:.3f} dB is below "
            f"{MIN_MEDIAN_PSNR_DELTA_DB:.3f} dB"
        )

    return {
        "schema": SCHEMA,
        "manifest_sha256": manifest["manifest_sha256"],
        "status": "passed" if not failures else "failed",
        "product_eligible": False,
        "foundation_benchmark_admitted": not failures,
        "thresholds": {
            "minimum_pair_psnr_delta_db": MIN_PAIR_PSNR_DELTA_DB,
            "minimum_median_psnr_delta_db": MIN_MEDIAN_PSNR_DELTA_DB,
            "every_pair_mse_must_improve": True,
        },
        "summary": {
            "pair_count": len(pairs),
            "median_psnr_delta_db": median_delta,
            "worst_psnr_delta_db": min(deltas),
            "median_mse_ratio": statistics.median(ratios),
            "worst_mse_ratio": max(ratios),
        },
        "gate_failures": failures,
        "pairs": [
            {
                "scene": pair.scene,
                "camera_make": pair.camera_make,
                "camera_model": pair.camera_model,
                "noisy_iso": pair.noisy_iso,
                "quality": quality.as_json(pair.quality),
            }
            for pair in pairs
        ],
        "limitations": [
            (
                "the v1 pilot evaluates one deterministic center region per "
                "scene; deterministic multi-region texture and flat-field "
                "metrics remain the next quality refinement"
            ),
            (
                "passing this benchmark admits native artifact-reader work, "
                "not UI exposure or default model selection"
            ),
        ],
    }


def write_report(value: dict[str, Any], path: Path) -> None:
    resolved = path.resolve()
    if resolved.exists():
        raise ValueError("RawNIND benchmark report must not already exist")
    resolved.parent.mkdir(parents=True, exist_ok=True)
    with resolved.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--raw-root", type=Path, required=True)
    parser.add_argument(
        "--provider",
        choices=("coreml", "cpu"),
        default="coreml",
    )
    parser.add_argument("--preview-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    manifest = dataset_manifest.load_manifest(arguments.manifest)
    pairs = evaluate_manifest(
        manifest,
        arguments.model,
        arguments.raw_root,
        arguments.provider,
        arguments.preview_root,
    )
    report = aggregate(manifest, pairs)
    write_report(report, arguments.output)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["foundation_benchmark_admitted"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as error:
        print(f"error: {error}")
        raise SystemExit(2) from error
