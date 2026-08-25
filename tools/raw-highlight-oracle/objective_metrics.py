"""Versioned, region-aware objective comparison for RAW highlight oracles."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import resource
import sys
import time
from typing import Iterable

import numpy as np

from cfa_topology import MASK_BITS, read_topology_mask
from linear_image import (
    LinearImage,
    linear_image_dimensions,
    read_linear_image,
    sha256_file,
    write_pfm,
    write_pgm,
)


ANALYSIS_SCHEMA = "shadow.raw-highlight-objective-analysis.v1"
REGION_SCHEMA = "shadow.raw-highlight-region.v1"
LUMA_WEIGHTS = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def _parse_assignment(value: str) -> tuple[str, pathlib.Path]:
    identifier, separator, raw_path = value.partition("=")
    if not separator or not identifier or not raw_path:
        raise argparse.ArgumentTypeError("expected ID=/absolute/path")
    if not identifier.replace("-", "").replace("_", "").isalnum():
        raise argparse.ArgumentTypeError("ID may contain only letters, numbers, '-' and '_'")
    return identifier, pathlib.Path(raw_path).expanduser().resolve()


def _parse_transfer_assignment(value: str) -> tuple[str, str]:
    identifier, separator, transfer = value.partition("=")
    if not separator or not identifier or transfer not in {"linear", "srgb"}:
        raise argparse.ArgumentTypeError("expected ID=linear or ID=srgb")
    return identifier, transfer


def _parse_orientation_assignment(value: str) -> tuple[str, str]:
    identifier, separator, orientation = value.partition("=")
    if not separator or not identifier or orientation not in {"bottom-up", "top-down"}:
        raise argparse.ArgumentTypeError("expected ID=bottom-up or ID=top-down")
    return identifier, orientation


def _identity(path: pathlib.Path) -> dict[str, object]:
    if not path.is_file():
        raise ValueError(f"input does not exist: {path}")
    return {
        "basename": path.name,
        "size_bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def _normalized_crop(
    dimensions: tuple[int, int], normalized: Iterable[float]
) -> tuple[int, int, int, int]:
    values = tuple(float(value) for value in normalized)
    if len(values) != 4:
        raise ValueError("crop_normalized must contain x, y, width, height")
    x, y, width, height = values
    if x < 0.0 or y < 0.0 or width <= 0.0 or height <= 0.0 or x + width > 1.0 or y + height > 1.0:
        raise ValueError("crop_normalized must be a positive rectangle within [0, 1]")
    full_width, full_height = dimensions
    x0 = max(0, min(full_width - 1, math.floor(x * full_width)))
    y0 = max(0, min(full_height - 1, math.floor(y * full_height)))
    x1 = max(x0 + 1, min(full_width, math.ceil((x + width) * full_width)))
    y1 = max(y0 + 1, min(full_height, math.ceil((y + height) * full_height)))
    return (x0, y0, x1 - x0, y1 - y0)


def _centre_crop(array: np.ndarray, shape: tuple[int, int]) -> np.ndarray:
    height, width = shape
    if height > array.shape[0] or width > array.shape[1]:
        raise ValueError("centre crop cannot enlarge its input")
    y = (array.shape[0] - height) // 2
    x = (array.shape[1] - width) // 2
    return array[y : y + height, x : x + width]


def _centre_match(reference: np.ndarray, candidate: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    shape = (
        min(reference.shape[0], candidate.shape[0]),
        min(reference.shape[1], candidate.shape[1]),
    )
    return _centre_crop(reference, shape), _centre_crop(candidate, shape)


def _luma(pixels: np.ndarray) -> np.ndarray:
    return np.tensordot(pixels, LUMA_WEIGHTS, axes=([2], [0])).astype(np.float32)


def decode_transfer(pixels: np.ndarray, transfer: str) -> tuple[np.ndarray, dict[str, object]]:
    values = np.asarray(pixels, dtype=np.float32)
    if transfer == "linear":
        return values, {"input_transfer": "linear", "decode": "identity"}
    if transfer == "srgb":
        decoded = np.empty_like(values)
        linear_segment = values <= 0.04045
        decoded[linear_segment] = values[linear_segment] / 12.92
        decoded[~linear_segment] = np.power(
            (values[~linear_segment] + 0.055) / 1.055, 2.4
        )
        return decoded, {
            "input_transfer": "srgb",
            "decode": "IEC-61966-2-1-piecewise-to-linear-v1",
            "negative_values_preserved_by_linear_segment": True,
            "values_above_one_preserved": True,
        }
    raise ValueError(f"unsupported transfer: {transfer}")


def _overlap_for_shift(
    reference: np.ndarray, candidate: np.ndarray, dx: int, dy: int
) -> tuple[np.ndarray, np.ndarray]:
    height, width = reference.shape[:2]
    ref_x0 = max(0, dx)
    ref_y0 = max(0, dy)
    cand_x0 = max(0, -dx)
    cand_y0 = max(0, -dy)
    overlap_width = width - abs(dx)
    overlap_height = height - abs(dy)
    if overlap_width <= 0 or overlap_height <= 0:
        raise ValueError("registration shift removed the complete overlap")
    return (
        reference[ref_y0 : ref_y0 + overlap_height, ref_x0 : ref_x0 + overlap_width],
        candidate[cand_y0 : cand_y0 + overlap_height, cand_x0 : cand_x0 + overlap_width],
    )


def register_translation(
    reference: np.ndarray, candidate: np.ndarray, max_shift: int
) -> tuple[np.ndarray, np.ndarray, dict[str, object]]:
    reference, candidate = _centre_match(reference, candidate)
    reference_luma = _luma(reference)
    candidate_luma = _luma(candidate)
    stride = max(1, min(reference.shape[0], reference.shape[1]) // 384)
    best: tuple[float, int, int] | None = None
    for dy in range(-max_shift, max_shift + 1):
        for dx in range(-max_shift, max_shift + 1):
            ref_view, cand_view = _overlap_for_shift(reference_luma, candidate_luma, dx, dy)
            ref_sample = ref_view[::stride, ::stride]
            cand_sample = cand_view[::stride, ::stride]
            ref_median = float(np.median(ref_sample))
            cand_median = float(np.median(cand_sample))
            scale = ref_median / max(cand_median, 1.0e-8)
            score = float(np.median(np.abs(ref_sample - cand_sample * scale)))
            proposal = (score, dy, dx)
            if best is None or proposal < best:
                best = proposal
    assert best is not None
    score, dy, dx = best
    registered_reference, registered_candidate = _overlap_for_shift(
        reference, candidate, dx, dy
    )
    return registered_reference, registered_candidate, {
        "method": "bounded-integer-translation-median-luma-v1",
        "dx_pixels": dx,
        "dy_pixels": dy,
        "max_shift_pixels": max_shift,
        "sampling_stride": stride,
        "median_absolute_luma_score": score,
        "matched_shape": [registered_reference.shape[1], registered_reference.shape[0]],
    }


def _rectangle_mask(
    shape: tuple[int, int], rectangles: Iterable[Iterable[float]]
) -> np.ndarray:
    height, width = shape
    mask = np.zeros((height, width), dtype=bool)
    for rectangle in rectangles:
        x, y, rect_width, rect_height = (float(value) for value in rectangle)
        if x < 0.0 or y < 0.0 or rect_width <= 0.0 or rect_height <= 0.0 or x + rect_width > 1.0 or y + rect_height > 1.0:
            raise ValueError("mask rectangles must be positive and within [0, 1]")
        x0 = max(0, min(width - 1, math.floor(x * width)))
        y0 = max(0, min(height - 1, math.floor(y * height)))
        x1 = max(x0 + 1, min(width, math.ceil((x + rect_width) * width)))
        y1 = max(y0 + 1, min(height, math.ceil((y + rect_height) * height)))
        mask[y0:y1, x0:x1] = True
    return mask


def _shift_mask(mask: np.ndarray, dx: int, dy: int) -> np.ndarray:
    shifted = np.zeros_like(mask)
    source_x0 = max(0, -dx)
    source_y0 = max(0, -dy)
    target_x0 = max(0, dx)
    target_y0 = max(0, dy)
    width = mask.shape[1] - abs(dx)
    height = mask.shape[0] - abs(dy)
    if width > 0 and height > 0:
        shifted[target_y0 : target_y0 + height, target_x0 : target_x0 + width] = mask[
            source_y0 : source_y0 + height, source_x0 : source_x0 + width
        ]
    return shifted


def _dilate(mask: np.ndarray, radius: int) -> np.ndarray:
    result = mask.copy()
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            if dx * dx + dy * dy <= radius * radius:
                result |= _shift_mask(mask, dx, dy)
    return result


def _erode(mask: np.ndarray, radius: int) -> np.ndarray:
    return ~_dilate(~mask, radius)


def registered_reference_mask(
    mask: np.ndarray,
    candidate_shape: tuple[int, int],
    registration: dict[str, object],
) -> np.ndarray:
    matched_shape = (
        min(mask.shape[0], candidate_shape[0]),
        min(mask.shape[1], candidate_shape[1]),
    )
    matched = _centre_crop(mask, matched_shape)
    registered, _ = _overlap_for_shift(
        matched,
        matched,
        int(registration["dx_pixels"]),
        int(registration["dy_pixels"]),
    )
    return registered


def build_masks(
    shape: tuple[int, int],
    region: dict[str, object],
    clipped_core: np.ndarray | None = None,
) -> dict[str, np.ndarray]:
    factual_core = clipped_core is not None
    core = (
        np.asarray(clipped_core, dtype=bool)
        if clipped_core is not None
        else _rectangle_mask(shape, region.get("clipped_core_rectangles", []))
    )
    if core.shape != shape:
        raise ValueError("clipped core dimensions do not match the registered image")
    if not factual_core and not bool(np.any(core)):
        raise ValueError("region must define a non-empty clipped core")
    radius = int(region.get("boundary_radius_pixels", 8))
    if radius < 1 or radius > 128:
        raise ValueError("boundary_radius_pixels must be between 1 and 128")
    expanded = _dilate(core, radius)
    contracted = _erode(core, radius)
    boundary = expanded & ~contracted
    exterior_rectangles = region.get("reliable_exterior_rectangles", [])
    exterior = (
        _rectangle_mask(shape, exterior_rectangles)
        if exterior_rectangles
        else ~expanded
    )
    exterior &= ~expanded
    if bool(np.any(core)) and not bool(np.any(boundary)):
        raise ValueError("region mask must contain boundary pixels")
    if not bool(np.any(exterior)):
        raise ValueError("region mask must contain reliable exterior pixels")
    return {"clipped_core": core, "boundary_band": boundary, "reliable_exterior": exterior}


def _masked_summary(values: np.ndarray, mask: np.ndarray) -> dict[str, object]:
    selected = np.asarray(values)[mask]
    if selected.size == 0:
        raise ValueError("metric mask is empty")
    if selected.ndim == 1:
        return {
            "count": int(selected.size),
            "mae": float(np.mean(np.abs(selected))),
            "rmse": float(np.sqrt(np.mean(np.square(selected)))),
            "p95_absolute": float(np.percentile(np.abs(selected), 95.0)),
        }
    return {
        "count": int(selected.shape[0]),
        "mae_rgb": [float(value) for value in np.mean(np.abs(selected), axis=0)],
        "rmse_rgb": [
            float(value) for value in np.sqrt(np.mean(np.square(selected), axis=0))
        ],
        "p95_absolute_rgb": [
            float(value) for value in np.percentile(np.abs(selected), 95.0, axis=0)
        ],
    }


def _hue_chroma(pixels: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    opponent_a = pixels[..., 0] - pixels[..., 1]
    opponent_b = pixels[..., 2] - pixels[..., 1]
    return np.arctan2(opponent_b, opponent_a), np.hypot(opponent_a, opponent_b)


def _angular_difference(left: np.ndarray, right: np.ndarray) -> np.ndarray:
    return np.abs(np.arctan2(np.sin(left - right), np.cos(left - right)))


def _gradient_and_curvature(luma: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    gradient_y, gradient_x = np.gradient(luma)
    gradient = np.hypot(gradient_x, gradient_y)
    curvature = np.abs(
        -4.0 * luma
        + np.roll(luma, 1, axis=0)
        + np.roll(luma, -1, axis=0)
        + np.roll(luma, 1, axis=1)
        + np.roll(luma, -1, axis=1)
    )
    return gradient, curvature


def _normalise_exposure(
    reference: np.ndarray, candidate: np.ndarray, exterior: np.ndarray
) -> tuple[np.ndarray, dict[str, object]]:
    reference_white = np.percentile(reference[exterior], 99.5, axis=0)
    candidate_white = np.percentile(candidate[exterior], 99.5, axis=0)
    if bool(np.any(candidate_white <= 1.0e-8)):
        raise ValueError("candidate reliable exterior has an unmeasurable colour channel")
    scale = reference_white / candidate_white
    if bool(np.any(scale < 0.0625)) or bool(np.any(scale > 16.0)):
        raise ValueError(f"RGB white normalization scale {scale.tolist()} is implausible")
    return candidate * scale.astype(np.float32), {
        "method": "reliable-exterior-rgb-p99.5-v1",
        "reference_white_rgb": [float(value) for value in reference_white],
        "candidate_white_rgb_before": [float(value) for value in candidate_white],
        "candidate_scale_rgb": [float(value) for value in scale],
        "linear_domain": True,
        "output_clipped": False,
    }


def normalise_candidate(
    reference: np.ndarray,
    candidate: np.ndarray,
    exterior: np.ndarray,
    mode: str,
) -> tuple[np.ndarray, dict[str, object]]:
    if mode == "identity":
        return candidate, {
            "method": "identity-same-pipeline-v1",
            "candidate_scale_rgb": [1.0, 1.0, 1.0],
            "linear_domain": True,
            "output_clipped": False,
        }
    if mode == "exterior-rgb":
        return _normalise_exposure(reference, candidate, exterior)
    raise ValueError(f"unsupported normalization mode: {mode}")


def _artifact(path: pathlib.Path, root: pathlib.Path) -> dict[str, object]:
    return {
        "path": str(path.relative_to(root)),
        "size_bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def _manifest_receipt(path: pathlib.Path, image_sha256: str) -> dict[str, object]:
    document = json.loads(path.read_text(encoding="utf-8"))
    matches: list[dict[str, object]] = []
    for adapter in document.get("adapters", []):
        for artifact in adapter.get("artifacts", []):
            if artifact.get("sha256") == image_sha256:
                matches.append(adapter)
    if len(matches) != 1:
        raise ValueError("candidate manifest must contain exactly one matching artifact")
    adapter = matches[0]
    return {
        "manifest_sha256": sha256_file(path),
        "manifest_schema": document.get("schema"),
        "run_name": document.get("run_name"),
        "adapter_id": adapter.get("id"),
        "adapter_elapsed_ms": adapter.get("elapsed_ms"),
        "adapter_peak_memory_bytes": adapter.get("peak_memory_bytes"),
    }


def _peak_rss_bytes() -> int:
    value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return int(value if sys.platform == "darwin" else value * 1024)


def analyse(args: argparse.Namespace) -> int:
    started = time.monotonic()
    reference_path = args.reference.expanduser().resolve()
    region_path = args.region_spec.expanduser().resolve()
    output_root = args.output_root.expanduser().resolve()
    repository_root = pathlib.Path(__file__).resolve().parents[2]
    try:
        output_root.relative_to(repository_root)
    except ValueError:
        pass
    else:
        raise ValueError("analysis output root must be outside the Shadow repository")
    run_directory = output_root / args.run_name
    run_directory.mkdir(parents=True, exist_ok=False)
    working = run_directory / "working"
    region = json.loads(region_path.read_text(encoding="utf-8"))
    if region.get("schema") != REGION_SCHEMA:
        raise ValueError(f"region spec must use {REGION_SCHEMA}")
    reference_dimensions = linear_image_dimensions(reference_path, working / "reference", args.tiffcp)
    reference_crop = _normalized_crop(reference_dimensions, region["crop_normalized"])
    reference_image = read_linear_image(
        reference_path,
        working / "reference",
        reference_crop,
        args.tiffcp,
        args.reference_pfm_orientation,
    )
    reference_pixels, reference_transfer = decode_transfer(
        reference_image.pixels, args.reference_transfer
    )
    topology_core: np.ndarray | None = None
    topology_receipt: dict[str, object] | None = None
    if args.topology_manifest is not None:
        topology_core, topology_receipt = read_topology_mask(
            args.topology_manifest,
            args.topology_mask,
            args.topology_image_space,
            reference_dimensions,
            reference_crop,
        )
    manifest_assignments = dict(args.candidate_manifest)
    transfer_assignments = dict(args.candidate_transfer)
    orientation_assignments = dict(args.candidate_pfm_orientation)
    candidate_ids = {identifier for identifier, _ in args.candidate}
    if set(transfer_assignments) != candidate_ids:
        raise ValueError(
            "--candidate-transfer must be supplied exactly once for every candidate id"
        )
    unknown_orientation_ids = set(orientation_assignments) - candidate_ids
    if unknown_orientation_ids:
        raise ValueError("PFM orientation was supplied for an unknown candidate id")
    candidate_records: list[dict[str, object]] = []
    for identifier, candidate_path in args.candidate:
        candidate_dimensions = linear_image_dimensions(
            candidate_path, working / identifier, args.tiffcp
        )
        candidate_crop = _normalized_crop(candidate_dimensions, region["crop_normalized"])
        candidate_image = read_linear_image(
            candidate_path,
            working / identifier,
            candidate_crop,
            args.tiffcp,
            orientation_assignments.get(identifier, "bottom-up"),
        )
        candidate_pixels, candidate_transfer = decode_transfer(
            candidate_image.pixels, transfer_assignments[identifier]
        )
        registered_reference, registered_candidate, registration = register_translation(
            reference_pixels, candidate_pixels, args.max_shift
        )
        registered_core = (
            registered_reference_mask(
                topology_core,
                candidate_pixels.shape[:2],
                registration,
            )
            if topology_core is not None
            else None
        )
        masks = build_masks(
            registered_reference.shape[:2],
            region,
            registered_core,
        )
        registered_candidate, normalization = normalise_candidate(
            registered_reference,
            registered_candidate,
            masks["reliable_exterior"],
            args.normalization,
        )
        rgb_difference = registered_candidate - registered_reference
        luma_difference = _luma(registered_candidate) - _luma(registered_reference)
        reference_hue, reference_chroma = _hue_chroma(registered_reference)
        candidate_hue, candidate_chroma = _hue_chroma(registered_candidate)
        hue_difference = _angular_difference(candidate_hue, reference_hue)
        chroma_difference = candidate_chroma - reference_chroma
        reference_gradient, reference_curvature = _gradient_and_curvature(
            _luma(registered_reference)
        )
        candidate_gradient, candidate_curvature = _gradient_and_curvature(
            _luma(registered_candidate)
        )
        exterior = masks["reliable_exterior"]
        chromatic = np.maximum(reference_chroma, candidate_chroma) > args.false_colour_chroma
        false_colour = exterior & (
            (np.abs(chroma_difference) > args.false_colour_chroma)
            | (
                chromatic
                & (hue_difference > math.radians(args.false_colour_hue_degrees))
            )
        )
        artifact_directory = run_directory / "candidates" / identifier
        paths = {
            "registered_reference": artifact_directory / "registered-reference.pfm",
            "registered_candidate": artifact_directory / "registered-candidate.pfm",
            "absolute_rgb_difference": artifact_directory / "absolute-rgb-difference.pfm",
            "absolute_luma_difference": artifact_directory / "absolute-luma-difference.pfm",
            "clipped_core_mask": artifact_directory / "clipped-core.pgm",
            "boundary_band_mask": artifact_directory / "boundary-band.pgm",
            "reliable_exterior_mask": artifact_directory / "reliable-exterior.pgm",
            "false_colour_mask": artifact_directory / "false-colour-exterior.pgm",
        }
        write_pfm(paths["registered_reference"], registered_reference)
        write_pfm(paths["registered_candidate"], registered_candidate)
        write_pfm(paths["absolute_rgb_difference"], np.abs(rgb_difference))
        write_pfm(
            paths["absolute_luma_difference"],
            np.repeat(np.abs(luma_difference)[..., None], 3, axis=2),
        )
        write_pgm(paths["clipped_core_mask"], masks["clipped_core"])
        write_pgm(paths["boundary_band_mask"], masks["boundary_band"])
        write_pgm(paths["reliable_exterior_mask"], exterior)
        write_pgm(paths["false_colour_mask"], false_colour)
        metrics: dict[str, object] = {}
        for mask_name, mask in masks.items():
            if bool(np.any(mask)):
                metrics[mask_name] = {
                    "available": True,
                    "rgb_difference": _masked_summary(rgb_difference, mask),
                    "luma_difference": _masked_summary(luma_difference, mask),
                }
            else:
                metrics[mask_name] = {
                    "available": False,
                    "pixel_count": 0,
                    "reason": "factual-clipped-core-empty",
                }
        boundary = masks["boundary_band"]
        if bool(np.any(boundary)):
            metrics["boundary_continuity"] = {
                "available": True,
                "mean_hue_error_degrees": float(
                    np.degrees(np.mean(hue_difference[boundary]))
                ),
                "p95_hue_error_degrees": float(
                    np.degrees(np.percentile(hue_difference[boundary], 95.0))
                ),
                "mean_absolute_chroma_error": float(
                    np.mean(np.abs(chroma_difference[boundary]))
                ),
                "mean_absolute_luminance_slope_error": float(
                    np.mean(
                        np.abs(candidate_gradient[boundary] - reference_gradient[boundary])
                    )
                ),
                "mean_absolute_luminance_curvature_error": float(
                    np.mean(
                        np.abs(candidate_curvature[boundary] - reference_curvature[boundary])
                    )
                ),
            }
        else:
            metrics["boundary_continuity"] = {
                "available": False,
                "reason": "factual-clipped-core-empty",
            }
        metrics["false_colour_exterior"] = {
            "pixel_count": int(np.count_nonzero(false_colour)),
            "exterior_pixel_count": int(np.count_nonzero(exterior)),
            "area_fraction": float(np.count_nonzero(false_colour) / np.count_nonzero(exterior)),
            "hue_threshold_degrees": args.false_colour_hue_degrees,
            "chroma_threshold": args.false_colour_chroma,
        }
        image_identity = _identity(candidate_path)
        record: dict[str, object] = {
            "id": identifier,
            "image": image_identity,
            "image_format": candidate_image.source_format,
            "image_receipt": candidate_image.receipt,
            "transfer_decode": candidate_transfer,
            "requested_crop_xywh": list(candidate_crop),
            "registration": registration,
            "clipped_core_source": {
                "mode": (
                    "cfa-topology" if registered_core is not None else "manual-region-rectangles"
                ),
                "selected_pixel_count": int(np.count_nonzero(masks["clipped_core"])),
            },
            "exposure_white_normalization": normalization,
            "metrics": metrics,
            "artifacts": [_artifact(path, run_directory) for path in paths.values()],
        }
        manifest_path = manifest_assignments.get(identifier)
        if manifest_path is not None:
            record["oracle_run"] = _manifest_receipt(
                manifest_path, str(image_identity["sha256"])
            )
        candidate_records.append(record)
    elapsed_ms = (time.monotonic() - started) * 1000.0
    document = {
        "schema": ANALYSIS_SCHEMA,
        "analysis_version": "20260825.2",
        "run_name": args.run_name,
        "linear_contract": {
            "primaries": args.primaries,
            "transfer": "linear",
            "negative_values_preserved": True,
            "values_above_one_preserved": True,
        },
        "region": region,
        "region_spec": _identity(region_path),
        "reference": {
            "image": _identity(reference_path),
            "image_format": reference_image.source_format,
            "image_receipt": reference_image.receipt,
            "transfer_decode": reference_transfer,
            "requested_crop_xywh": list(reference_crop),
        },
        **({"topology": topology_receipt} if topology_receipt is not None else {}),
        "candidates": candidate_records,
        "resource_usage": {
            "analysis_elapsed_ms": round(elapsed_ms, 3),
            "analysis_peak_rss_bytes": _peak_rss_bytes(),
        },
    }
    manifest = run_directory / "analysis.json"
    manifest.write_text(
        json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"objective.run_directory={run_directory}")
    print(f"objective.analysis={manifest}")
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--reference", type=pathlib.Path, required=True)
    result.add_argument(
        "--candidate", type=_parse_assignment, action="append", required=True, metavar="ID=PATH"
    )
    result.add_argument(
        "--candidate-transfer",
        type=_parse_transfer_assignment,
        action="append",
        required=True,
        metavar="ID=linear|srgb",
    )
    result.add_argument(
        "--candidate-pfm-orientation",
        type=_parse_orientation_assignment,
        action="append",
        default=[],
        metavar="ID=bottom-up|top-down",
    )
    result.add_argument(
        "--candidate-manifest",
        type=_parse_assignment,
        action="append",
        default=[],
        metavar="ID=PATH",
    )
    result.add_argument("--region-spec", type=pathlib.Path, required=True)
    result.add_argument("--topology-manifest", type=pathlib.Path)
    result.add_argument(
        "--topology-mask",
        choices=tuple(MASK_BITS),
        default="physical-white",
        help="factual CFA mask used as the clipped core when --topology-manifest is supplied",
    )
    result.add_argument(
        "--topology-image-space",
        choices=("active", "display"),
        default="display",
        help="project the active sensor mask through RawFrame orientation before cropping",
    )
    result.add_argument("--output-root", type=pathlib.Path, required=True)
    result.add_argument("--run-name", required=True)
    result.add_argument("--primaries", default="sRGB-D65")
    result.add_argument(
        "--normalization",
        choices=("identity", "exterior-rgb"),
        default="identity",
        help="identity for same-pipeline ablations; exterior-rgb for declared full-pipeline comparisons",
    )
    result.add_argument(
        "--reference-transfer", choices=("linear", "srgb"), default="linear"
    )
    result.add_argument(
        "--reference-pfm-orientation",
        choices=("bottom-up", "top-down"),
        default="bottom-up",
    )
    result.add_argument("--max-shift", type=int, default=12)
    result.add_argument("--false-colour-hue-degrees", type=float, default=8.0)
    result.add_argument("--false-colour-chroma", type=float, default=0.02)
    result.add_argument("--tiffcp")
    return result


def main(argv: list[str] | None = None) -> int:
    try:
        return analyse(parser().parse_args(argv))
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
