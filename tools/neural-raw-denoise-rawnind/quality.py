#!/usr/bin/env python3
"""Bounded noisy/clean quality probe for the public RawNIND Bayer model."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import math
from pathlib import Path

import numpy as np

import inference
import public_pair


ALIGNMENT_PATCH_PACKED = 640
ALIGNMENT_DOWNSAMPLE = 4
ALIGNMENT_MAX_SHIFT_SENSOR = 128
ALIGNMENT_REFINE_RADIUS_SENSOR = 8
EVALUATION_HALO_SENSOR = 2 * inference.CONSERVATIVE_EXACT_HALO_PACKED


@dataclass(frozen=True)
class ErrorMetrics:
    mse: float
    rmse: float
    mae: float
    psnr_db: float
    mean_bias: float


@dataclass(frozen=True)
class PairQualityReceipt:
    dataset_doi: str
    dataset_license: str
    noisy: public_pair.VerifiedPublicRaw
    ground_truth: public_pair.VerifiedPublicRaw
    provider: str
    metric_space: str
    ground_truth_method: str
    baseline_method: str
    gain_policy: str
    saturation_threshold: float
    alignment_shift_sensor: list[int]
    alignment_probe_mae: float
    noisy_sensor_shape: list[int]
    ground_truth_sensor_shape: list[int]
    noisy_raw_pattern: list[list[int]]
    ground_truth_raw_pattern: list[list[int]]
    noisy_force_rggb_crop_sensor: list[int]
    ground_truth_force_rggb_crop_sensor: list[int]
    tile_origin_packed_noisy: list[int]
    ground_truth_origin_sensor: list[int]
    evaluation_halo_sensor: int
    evaluation_shape_sensor: list[int]
    valid_fraction: float
    baseline_gain_to_ground_truth: float
    denoised_gain_to_ground_truth: float
    baseline: ErrorMetrics
    denoised: ErrorMetrics
    psnr_delta_db: float
    mse_ratio: float
    improved_on_probe: bool


def _pad_last(value: np.ndarray) -> np.ndarray:
    return np.pad(value, ((0, 1), (0, 1)), mode="edge")


def _pad_first(value: np.ndarray) -> np.ndarray:
    return np.pad(value, ((1, 0), (1, 0)), mode="edge")


def canonical_bilinear_demosaic(packed: np.ndarray) -> np.ndarray:
    """Demosaic canonical packed RGGB into linear camera RGB.

    The public graph's `force_rggb` contract makes channel order, rather than
    the camera's original top-left site, the spatial convention used here.
    """

    if packed.ndim != 3 or packed.shape[0] != 4:
        raise ValueError("packed Bayer image must have shape [4, H, W]")
    red, green_one, green_two, blue = (
        packed[index].astype(np.float32, copy=False) for index in range(4)
    )
    height, width = red.shape
    rgb = np.empty((3, height * 2, width * 2), dtype=np.float32)

    red_next = _pad_last(red)
    rgb[0, 0::2, 0::2] = red
    rgb[0, 0::2, 1::2] = 0.5 * (
        red_next[:-1, :-1] + red_next[:-1, 1:]
    )
    rgb[0, 1::2, 0::2] = 0.5 * (
        red_next[:-1, :-1] + red_next[1:, :-1]
    )
    rgb[0, 1::2, 1::2] = 0.25 * (
        red_next[:-1, :-1]
        + red_next[:-1, 1:]
        + red_next[1:, :-1]
        + red_next[1:, 1:]
    )

    blue_previous = _pad_first(blue)
    rgb[2, 1::2, 1::2] = blue
    rgb[2, 1::2, 0::2] = 0.5 * (
        blue_previous[1:, :-1] + blue_previous[1:, 1:]
    )
    rgb[2, 0::2, 1::2] = 0.5 * (
        blue_previous[:-1, 1:] + blue_previous[1:, 1:]
    )
    rgb[2, 0::2, 0::2] = 0.25 * (
        blue_previous[:-1, :-1]
        + blue_previous[:-1, 1:]
        + blue_previous[1:, :-1]
        + blue_previous[1:, 1:]
    )

    rgb[1, 0::2, 1::2] = green_one
    rgb[1, 1::2, 0::2] = green_two
    green_one_previous = np.pad(green_one, ((1, 0), (0, 1)), mode="edge")
    green_two_previous = np.pad(green_two, ((0, 1), (1, 0)), mode="edge")
    rgb[1, 0::2, 0::2] = 0.25 * (
        green_one_previous[1:, :-1]
        + green_one_previous[1:, 1:]
        + green_two_previous[:-1, 1:]
        + green_two_previous[1:, 1:]
    )
    green_one_next = np.pad(green_one, ((0, 1), (1, 0)), mode="edge")
    green_two_next = np.pad(green_two, ((1, 0), (0, 1)), mode="edge")
    rgb[1, 1::2, 1::2] = 0.25 * (
        green_one_next[:-1, 1:]
        + green_one_next[1:, 1:]
        + green_two_next[1:, :-1]
        + green_two_next[1:, 1:]
    )
    return rgb


def _mean_downsample(image: np.ndarray, factor: int) -> np.ndarray:
    height = image.shape[0] - image.shape[0] % factor
    width = image.shape[1] - image.shape[1] % factor
    cropped = image[:height, :width]
    return cropped.reshape(
        height // factor,
        factor,
        width // factor,
        factor,
    ).mean(axis=(1, 3))


def _phase_shift(reference: np.ndarray, target: np.ndarray) -> tuple[int, int]:
    if reference.shape != target.shape or reference.ndim != 2:
        raise ValueError("phase alignment inputs must be equally sized 2D arrays")
    height, width = reference.shape
    window = np.outer(np.hanning(height), np.hanning(width))
    reference_fft = np.fft.rfft2((reference - reference.mean()) * window)
    target_fft = np.fft.rfft2((target - target.mean()) * window)
    cross_power = reference_fft * np.conj(target_fft)
    magnitude = np.abs(cross_power)
    cross_power /= np.maximum(magnitude, 1e-12)
    correlation = np.fft.irfft2(cross_power, s=reference.shape)
    row, column = np.unravel_index(np.argmax(correlation), correlation.shape)
    if row > height // 2:
        row -= height
    if column > width // 2:
        column -= width
    return int(row), int(column)


def aligned_views(
    reference: np.ndarray,
    target: np.ndarray,
    shift: tuple[int, int],
) -> tuple[np.ndarray, np.ndarray]:
    """Return views where reference[y+dy, x+dx] corresponds to target[y, x]."""

    if reference.shape != target.shape:
        raise ValueError("alignment inputs must have the same shape")
    row_shift, column_shift = shift
    height, width = reference.shape[-2:]
    reference_rows = slice(max(row_shift, 0), min(height + row_shift, height))
    target_rows = slice(max(-row_shift, 0), min(height - row_shift, height))
    reference_columns = slice(
        max(column_shift, 0),
        min(width + column_shift, width),
    )
    target_columns = slice(
        max(-column_shift, 0),
        min(width - column_shift, width),
    )
    return (
        reference[..., reference_rows, reference_columns],
        target[..., target_rows, target_columns],
    )


def _matched_mae(
    reference: np.ndarray,
    target: np.ndarray,
    shift: tuple[int, int],
) -> float:
    reference_view, target_view = aligned_views(reference, target, shift)
    if min(reference_view.shape[-2:]) < 32:
        return math.inf
    gain = float(
        np.mean(reference_view, dtype=np.float64)
        / max(float(np.mean(target_view, dtype=np.float64)), 1e-12)
    )
    return float(
        np.mean(
            np.abs(reference_view - target_view * np.float32(gain)),
            dtype=np.float64,
        )
    )


def estimate_alignment(
    reference_rgb: np.ndarray,
    target_rgb: np.ndarray,
) -> tuple[tuple[int, int], float]:
    if reference_rgb.shape != target_rgb.shape or reference_rgb.shape[0] != 3:
        raise ValueError("alignment requires equally sized camera RGB images")
    reference_luma = np.mean(reference_rgb, axis=0, dtype=np.float32)
    target_luma = np.mean(target_rgb, axis=0, dtype=np.float32)
    reference_small = _mean_downsample(
        reference_luma,
        ALIGNMENT_DOWNSAMPLE,
    )
    target_small = _mean_downsample(target_luma, ALIGNMENT_DOWNSAMPLE)
    coarse = _phase_shift(reference_small, target_small)
    coarse_sensor = (
        coarse[0] * ALIGNMENT_DOWNSAMPLE,
        coarse[1] * ALIGNMENT_DOWNSAMPLE,
    )
    if (
        abs(coarse_sensor[0]) > ALIGNMENT_MAX_SHIFT_SENSOR
        or abs(coarse_sensor[1]) > ALIGNMENT_MAX_SHIFT_SENSOR
    ):
        raise ValueError("RawNIND pair alignment exceeds the bounded search")

    best_shift = coarse_sensor
    best_loss = math.inf
    for row in range(
        coarse_sensor[0] - ALIGNMENT_REFINE_RADIUS_SENSOR,
        coarse_sensor[0] + ALIGNMENT_REFINE_RADIUS_SENSOR + 1,
    ):
        for column in range(
            coarse_sensor[1] - ALIGNMENT_REFINE_RADIUS_SENSOR,
            coarse_sensor[1] + ALIGNMENT_REFINE_RADIUS_SENSOR + 1,
        ):
            if (
                abs(row) > ALIGNMENT_MAX_SHIFT_SENSOR
                or abs(column) > ALIGNMENT_MAX_SHIFT_SENSOR
            ):
                continue
            loss = _matched_mae(
                reference_luma,
                target_luma,
                (row, column),
            )
            if loss < best_loss:
                best_shift = (row, column)
                best_loss = loss
    return best_shift, best_loss


def _demosaic_sensor_crop(
    packed: np.ndarray,
    origin_sensor: tuple[int, int],
    edge_sensor: int,
) -> np.ndarray:
    row, column = origin_sensor
    if row < 0 or column < 0:
        raise ValueError("sensor crop origin must be non-negative")
    packed_row = row // 2
    packed_column = column // 2
    row_offset = row % 2
    column_offset = column % 2
    packed_edge = (edge_sensor + max(row_offset, column_offset) + 1) // 2
    margin = 1
    start_row = max(packed_row - margin, 0)
    start_column = max(packed_column - margin, 0)
    end_row = min(packed_row + packed_edge + margin, packed.shape[1])
    end_column = min(packed_column + packed_edge + margin, packed.shape[2])
    local = canonical_bilinear_demosaic(
        packed[:, start_row:end_row, start_column:end_column]
    )
    local_row = row - 2 * start_row
    local_column = column - 2 * start_column
    crop = local[
        :,
        local_row : local_row + edge_sensor,
        local_column : local_column + edge_sensor,
    ]
    if crop.shape != (3, edge_sensor, edge_sensor):
        raise ValueError("aligned ground-truth crop falls outside the RAW image")
    return crop


def _match_to_reference(
    reference: np.ndarray,
    candidate: np.ndarray,
    mask: np.ndarray,
) -> tuple[np.ndarray, float]:
    reference_mean = float(np.mean(reference[:, mask], dtype=np.float64))
    candidate_mean = float(np.mean(candidate[:, mask], dtype=np.float64))
    if abs(candidate_mean) <= 1e-12:
        raise ValueError("quality candidate has a degenerate mean")
    gain = reference_mean / candidate_mean
    return candidate * np.float32(gain), gain


def error_metrics(
    reference: np.ndarray,
    candidate: np.ndarray,
    mask: np.ndarray,
) -> ErrorMetrics:
    difference = candidate[:, mask] - reference[:, mask]
    mse = float(np.mean(difference * difference, dtype=np.float64))
    mae = float(np.mean(np.abs(difference), dtype=np.float64))
    return ErrorMetrics(
        mse=mse,
        rmse=math.sqrt(mse),
        mae=mae,
        psnr_db=(math.inf if mse == 0.0 else 10.0 * math.log10(1.0 / mse)),
        mean_bias=float(np.mean(difference, dtype=np.float64)),
    )


def write_comparison_ppm(
    reference: np.ndarray,
    baseline: np.ndarray,
    denoised: np.ndarray,
    path: Path,
) -> None:
    if reference.shape != baseline.shape or reference.shape != denoised.shape:
        raise ValueError("quality preview inputs must have identical shapes")
    if reference.ndim != 3 or reference.shape[0] != 3:
        raise ValueError("quality preview inputs must be camera RGB")
    if path.exists():
        raise ValueError("quality preview output must not already exist")
    display = np.concatenate([reference, baseline, denoised], axis=2)
    display = np.power(np.clip(display, 0.0, 1.0), 1.0 / 2.2)
    pixels = np.rint(display * 255.0).astype(np.uint8)
    height, width = pixels.shape[1:]
    with path.open("xb") as stream:
        stream.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
        stream.write(np.transpose(pixels, (1, 2, 0)).tobytes())


def _alignment_probe(
    ground_truth: np.ndarray,
    noisy: np.ndarray,
) -> tuple[tuple[int, int], float]:
    edge = min(
        ALIGNMENT_PATCH_PACKED,
        ground_truth.shape[1],
        ground_truth.shape[2],
        noisy.shape[1],
        noisy.shape[2],
    )
    if edge < inference.TILE_EDGE:
        raise ValueError("RawNIND pair is too small for bounded alignment")
    ground_truth_start = (
        (ground_truth.shape[1] - edge) // 2,
        (ground_truth.shape[2] - edge) // 2,
    )
    noisy_start = (
        (noisy.shape[1] - edge) // 2,
        (noisy.shape[2] - edge) // 2,
    )
    ground_truth_rgb = canonical_bilinear_demosaic(
        ground_truth[
            :,
            ground_truth_start[0] : ground_truth_start[0] + edge,
            ground_truth_start[1] : ground_truth_start[1] + edge,
        ]
    )
    noisy_rgb = canonical_bilinear_demosaic(
        noisy[
            :,
            noisy_start[0] : noisy_start[0] + edge,
            noisy_start[1] : noisy_start[1] + edge,
        ]
    )
    local_shift, loss = estimate_alignment(ground_truth_rgb, noisy_rgb)
    origin_delta = (
        2 * (ground_truth_start[0] - noisy_start[0]),
        2 * (ground_truth_start[1] - noisy_start[1]),
    )
    return (
        local_shift[0] - origin_delta[0],
        local_shift[1] - origin_delta[1],
    ), loss


def evaluate_public_pair(
    model_path: Path,
    noisy_path: Path,
    ground_truth_path: Path,
    requested_provider: str,
    preview_path: Path | None = None,
    *,
    expected_noisy: public_pair.PublicRawFile = public_pair.NOISY,
    expected_ground_truth: public_pair.PublicRawFile = public_pair.GROUND_TRUTH,
) -> PairQualityReceipt:
    verified_noisy = public_pair.verify_public_raw(
        noisy_path,
        expected_noisy,
    )
    verified_ground_truth = public_pair.verify_public_raw(
        ground_truth_path,
        expected_ground_truth,
    )
    noisy, noisy_metadata = inference.load_raw_as_packed_bayer(noisy_path)
    ground_truth, ground_truth_metadata = inference.load_raw_as_packed_bayer(
        ground_truth_path
    )
    if noisy_metadata["color_description"] != ground_truth_metadata["color_description"]:
        raise ValueError("RawNIND pair color descriptions differ")
    if noisy_metadata["raw_pattern"] != ground_truth_metadata["raw_pattern"]:
        raise ValueError("RawNIND pair Bayer patterns differ")

    alignment, alignment_loss = _alignment_probe(ground_truth, noisy)
    noisy_tile, noisy_origin = inference.centered_tile(noisy)
    model_output, _ = inference.run_tile(
        model_path,
        noisy_tile,
        requested_provider,
        warm_runs=0,
    )
    baseline = canonical_bilinear_demosaic(noisy_tile[0])
    ground_truth_origin_sensor = (
        2 * noisy_origin[0] + alignment[0],
        2 * noisy_origin[1] + alignment[1],
    )
    reference = _demosaic_sensor_crop(
        ground_truth,
        ground_truth_origin_sensor,
        inference.TILE_EDGE * inference.OUTPUT_SCALE,
    )

    halo = EVALUATION_HALO_SENSOR
    evaluation_slice = (
        slice(halo, reference.shape[1] - halo),
        slice(halo, reference.shape[2] - halo),
    )
    reference = reference[:, evaluation_slice[0], evaluation_slice[1]]
    baseline = baseline[:, evaluation_slice[0], evaluation_slice[1]]
    denoised = model_output[0, :, evaluation_slice[0], evaluation_slice[1]]
    valid = np.max(reference, axis=0) < 0.98
    if float(np.mean(valid)) < 0.25:
        raise ValueError("RawNIND quality probe has too few unsaturated pixels")

    baseline, baseline_gain = _match_to_reference(reference, baseline, valid)
    denoised, denoised_gain = _match_to_reference(reference, denoised, valid)
    if preview_path is not None:
        write_comparison_ppm(reference, baseline, denoised, preview_path)
    baseline_metrics = error_metrics(reference, baseline, valid)
    denoised_metrics = error_metrics(reference, denoised, valid)
    mse_ratio = denoised_metrics.mse / max(baseline_metrics.mse, 1e-20)
    return PairQualityReceipt(
        dataset_doi=public_pair.DATASET_DOI,
        dataset_license=public_pair.DATASET_LICENSE,
        noisy=verified_noisy,
        ground_truth=verified_ground_truth,
        provider=requested_provider,
        metric_space="normalized-linear-camera-rgb",
        ground_truth_method="canonical-bilinear-demosaic",
        baseline_method="canonical-bilinear-demosaic",
        gain_policy="independent-scalar-mean-match-to-ground-truth",
        saturation_threshold=0.98,
        alignment_shift_sensor=[alignment[0], alignment[1]],
        alignment_probe_mae=alignment_loss,
        noisy_sensor_shape=noisy_metadata["sensor_shape"],
        ground_truth_sensor_shape=ground_truth_metadata["sensor_shape"],
        noisy_raw_pattern=noisy_metadata["raw_pattern"],
        ground_truth_raw_pattern=ground_truth_metadata["raw_pattern"],
        noisy_force_rggb_crop_sensor=noisy_metadata["force_rggb_crop_sensor"],
        ground_truth_force_rggb_crop_sensor=(
            ground_truth_metadata["force_rggb_crop_sensor"]
        ),
        tile_origin_packed_noisy=[noisy_origin[0], noisy_origin[1]],
        ground_truth_origin_sensor=[
            ground_truth_origin_sensor[0],
            ground_truth_origin_sensor[1],
        ],
        evaluation_halo_sensor=halo,
        evaluation_shape_sensor=[
            int(reference.shape[1]),
            int(reference.shape[2]),
        ],
        valid_fraction=float(np.mean(valid)),
        baseline_gain_to_ground_truth=baseline_gain,
        denoised_gain_to_ground_truth=denoised_gain,
        baseline=baseline_metrics,
        denoised=denoised_metrics,
        psnr_delta_db=denoised_metrics.psnr_db - baseline_metrics.psnr_db,
        mse_ratio=mse_ratio,
        improved_on_probe=mse_ratio < 1.0,
    )


def as_json(value: object) -> object:
    if hasattr(value, "__dataclass_fields__"):
        return asdict(value)
    raise TypeError(f"cannot serialize {type(value)!r}")
