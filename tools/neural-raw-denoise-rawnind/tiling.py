#!/usr/bin/env python3
"""Deterministic full-image tiling for the public RawNIND Bayer graph."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import math
import time
from typing import Callable

import numpy as np


MODEL_TILE_EDGE_PACKED = 512
MODEL_OUTPUT_SCALE = 2
MODEL_POOL_ALIGNMENT_PACKED = 16
DEFAULT_EXACT_HALO_PACKED = 100
DEFAULT_BLEND_OVERLAP_PACKED = 112
DEFAULT_MAX_WORKING_BYTES = 768 * 1024 * 1024
ESTIMATED_SCRATCH_BYTES = 64 * 1024 * 1024

TileRunner = Callable[[np.ndarray], np.ndarray]


@dataclass(frozen=True)
class TilingPlan:
    input_shape_packed: list[int]
    output_shape_sensor: list[int]
    tile_edge_packed: int
    output_scale: int
    exact_halo_packed: int
    blend_overlap_packed: int
    blend_width_packed: int
    step_packed: int
    pool_alignment_packed: int
    grid_rows: int
    grid_columns: int
    tile_count: int
    pad_before_packed: int
    pad_after_packed: list[int]
    estimated_working_bytes: int
    max_working_bytes: int


@dataclass(frozen=True)
class TilingReceipt:
    plan: TilingPlan
    tile_latency_p50_ms: float
    tile_latency_p95_ms: float
    tile_latency_samples: int
    wall_time_seconds: float
    global_input_mean: float
    raw_output_mean: float
    global_gain: float
    local_gain_min: float
    local_gain_max: float
    local_gain_relative_span: float
    coverage_weight_min: float
    coverage_weight_max: float
    coverage_max_abs_error: float
    trusted_overlap_samples: int
    trusted_overlap_max_abs: float
    trusted_overlap_rmse: float
    trusted_overlap_relative_rmse: float
    output_min: float
    output_max: float
    output_mean: float
    output_negative_fraction: float
    output_above_one_fraction: float


@dataclass
class FullImageRun:
    output: np.ndarray
    receipt: TilingReceipt


@dataclass(frozen=True)
class TilePlacement:
    row: int
    column: int
    packed_y: int
    packed_x: int
    global_y0: int
    global_y1: int
    global_x0: int
    global_x1: int
    local_y0: int
    local_y1: int
    local_x0: int
    local_x1: int


def _percentile(values: list[float], fraction: float) -> float:
    if not values:
        raise ValueError("cannot take percentile of an empty collection")
    ordered = sorted(values)
    index = min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1)
    return ordered[max(index, 0)]


def _axis_count(length: int, step: int) -> int:
    return (length + step - 1) // step


def plan_image_geometry(
    height_packed: int,
    width_packed: int,
    *,
    exact_halo_packed: int = DEFAULT_EXACT_HALO_PACKED,
    blend_overlap_packed: int = DEFAULT_BLEND_OVERLAP_PACKED,
    max_working_bytes: int = DEFAULT_MAX_WORKING_BYTES,
) -> TilingPlan:
    if height_packed < 2 or width_packed < 2:
        raise ValueError("full-image tiling requires at least 2x2 packed pixels")
    if exact_halo_packed < 0:
        raise ValueError("exact halo must be non-negative")
    if blend_overlap_packed <= exact_halo_packed:
        raise ValueError("blend overlap must exceed the exact halo")
    if blend_overlap_packed * 2 >= MODEL_TILE_EDGE_PACKED:
        raise ValueError("blend overlap leaves no tile interior")
    step = MODEL_TILE_EDGE_PACKED - 2 * blend_overlap_packed
    if step % MODEL_POOL_ALIGNMENT_PACKED:
        raise ValueError("tile step must preserve the model pooling phase")

    rows = _axis_count(height_packed, step)
    columns = _axis_count(width_packed, step)
    pad_after_y = max(
        blend_overlap_packed,
        (rows - 1) * step
        + MODEL_TILE_EDGE_PACKED
        - height_packed
        - blend_overlap_packed,
    )
    pad_after_x = max(
        blend_overlap_packed,
        (columns - 1) * step
        + MODEL_TILE_EDGE_PACKED
        - width_packed
        - blend_overlap_packed,
    )
    output_height = height_packed * MODEL_OUTPUT_SCALE
    output_width = width_packed * MODEL_OUTPUT_SCALE
    padded_input_bytes = (
        4
        * (height_packed + blend_overlap_packed + pad_after_y)
        * (width_packed + blend_overlap_packed + pad_after_x)
        * np.dtype(np.float32).itemsize
    )
    output_bytes = (
        3 * output_height * output_width * np.dtype(np.float32).itemsize
    )
    coverage_bytes = (
        output_height * output_width * np.dtype(np.float32).itemsize
    )
    tile_bytes = (
        (
            4 * MODEL_TILE_EDGE_PACKED * MODEL_TILE_EDGE_PACKED
            + 3
            * MODEL_TILE_EDGE_PACKED
            * MODEL_OUTPUT_SCALE
            * MODEL_TILE_EDGE_PACKED
            * MODEL_OUTPUT_SCALE
        )
        * np.dtype(np.float32).itemsize
    )
    estimated_working_bytes = (
        padded_input_bytes
        + output_bytes
        + coverage_bytes
        + tile_bytes
        + ESTIMATED_SCRATCH_BYTES
    )
    return TilingPlan(
        input_shape_packed=[4, height_packed, width_packed],
        output_shape_sensor=[3, output_height, output_width],
        tile_edge_packed=MODEL_TILE_EDGE_PACKED,
        output_scale=MODEL_OUTPUT_SCALE,
        exact_halo_packed=exact_halo_packed,
        blend_overlap_packed=blend_overlap_packed,
        blend_width_packed=2 * (
            blend_overlap_packed - exact_halo_packed
        ),
        step_packed=step,
        pool_alignment_packed=MODEL_POOL_ALIGNMENT_PACKED,
        grid_rows=rows,
        grid_columns=columns,
        tile_count=rows * columns,
        pad_before_packed=blend_overlap_packed,
        pad_after_packed=[pad_after_y, pad_after_x],
        estimated_working_bytes=estimated_working_bytes,
        max_working_bytes=max_working_bytes,
    )


def plan_full_image(
    height_packed: int,
    width_packed: int,
    *,
    exact_halo_packed: int = DEFAULT_EXACT_HALO_PACKED,
    blend_overlap_packed: int = DEFAULT_BLEND_OVERLAP_PACKED,
    max_working_bytes: int = DEFAULT_MAX_WORKING_BYTES,
) -> TilingPlan:
    plan = plan_image_geometry(
        height_packed,
        width_packed,
        exact_halo_packed=exact_halo_packed,
        blend_overlap_packed=blend_overlap_packed,
        max_working_bytes=max_working_bytes,
    )
    if plan.estimated_working_bytes > plan.max_working_bytes:
        raise ValueError(
            "full-image tiling exceeds the configured working-memory budget: "
            f"{plan.estimated_working_bytes} > {plan.max_working_bytes}"
        )
    return plan


def axis_weight(
    plan: TilingPlan,
    *,
    has_previous: bool,
    has_next: bool,
) -> np.ndarray:
    scale = plan.output_scale
    edge = plan.tile_edge_packed * scale
    halo = plan.exact_halo_packed * scale
    overlap = plan.blend_overlap_packed * scale
    blend_width = plan.blend_width_packed * scale
    weight = np.ones(edge, dtype=np.float32)
    if has_previous:
        weight[:halo] = 0.0
        weight[halo : halo + blend_width] = (
            np.arange(blend_width, dtype=np.float32) + 0.5
        ) / np.float32(blend_width)
    if has_next:
        right_start = edge - halo - blend_width
        weight[right_start : edge - halo] = (
            np.arange(blend_width, 0, -1, dtype=np.float32) - 0.5
        ) / np.float32(blend_width)
        weight[edge - halo :] = 0.0
    return weight


def tile_placement(
    plan: TilingPlan,
    row: int,
    column: int,
) -> TilePlacement:
    if not 0 <= row < plan.grid_rows:
        raise ValueError("tile row is outside the tiling plan")
    if not 0 <= column < plan.grid_columns:
        raise ValueError("tile column is outside the tiling plan")
    scale = plan.output_scale
    origin_y = row * plan.step_packed - plan.blend_overlap_packed
    origin_x = column * plan.step_packed - plan.blend_overlap_packed
    output_height = plan.output_shape_sensor[1]
    output_width = plan.output_shape_sensor[2]
    global_y0 = max(0, origin_y * scale)
    global_x0 = max(0, origin_x * scale)
    global_y1 = min(
        output_height,
        (origin_y + plan.tile_edge_packed) * scale,
    )
    global_x1 = min(
        output_width,
        (origin_x + plan.tile_edge_packed) * scale,
    )
    local_y0 = global_y0 - origin_y * scale
    local_x0 = global_x0 - origin_x * scale
    local_y1 = local_y0 + global_y1 - global_y0
    local_x1 = local_x0 + global_x1 - global_x0
    return TilePlacement(
        row=row,
        column=column,
        packed_y=row * plan.step_packed,
        packed_x=column * plan.step_packed,
        global_y0=global_y0,
        global_y1=global_y1,
        global_x0=global_x0,
        global_x1=global_x1,
        local_y0=local_y0,
        local_y1=local_y1,
        local_x0=local_x0,
        local_x1=local_x1,
    )


def tile_output_slices(
    placement: TilePlacement,
) -> tuple[slice, slice, slice, slice]:
    return (
        slice(placement.global_y0, placement.global_y1),
        slice(placement.global_x0, placement.global_x1),
        slice(placement.local_y0, placement.local_y1),
        slice(placement.local_x0, placement.local_x1),
    )


def validate_input(packed: np.ndarray, plan: TilingPlan) -> None:
    if packed.shape != tuple(plan.input_shape_packed):
        raise ValueError("packed image does not match the tiling plan")
    if packed.dtype != np.float32:
        raise ValueError("packed image must be float32")
    if not np.isfinite(packed).all():
        raise ValueError("packed image is not finite")


def reflected_indices(length: int, start: int, count: int) -> np.ndarray:
    if length < 2:
        raise ValueError("reflection requires an axis of at least two pixels")
    coordinates = np.arange(start, start + count, dtype=np.int64)
    period = 2 * length - 2
    folded = np.mod(coordinates, period)
    return np.where(folded < length, folded, period - folded)


def extract_tile(
    packed: np.ndarray,
    plan: TilingPlan,
    placement: TilePlacement,
) -> np.ndarray:
    origin_y = (
        placement.row * plan.step_packed - plan.blend_overlap_packed
    )
    origin_x = (
        placement.column * plan.step_packed - plan.blend_overlap_packed
    )
    indices_y = reflected_indices(
        packed.shape[1],
        origin_y,
        plan.tile_edge_packed,
    )
    indices_x = reflected_indices(
        packed.shape[2],
        origin_x,
        plan.tile_edge_packed,
    )
    tile = packed[:, indices_y[:, None], indices_x[None, :]]
    return np.ascontiguousarray(tile[None])


def run_full_image(
    packed: np.ndarray,
    runner: TileRunner,
    plan: TilingPlan,
) -> FullImageRun:
    validate_input(packed, plan)
    started = time.perf_counter()
    overlap = plan.blend_overlap_packed
    padded = np.pad(
        packed,
        (
            (0, 0),
            (overlap, plan.pad_after_packed[0]),
            (overlap, plan.pad_after_packed[1]),
        ),
        mode="reflect",
    )
    output = np.zeros(plan.output_shape_sensor, dtype=np.float32)
    coverage = np.zeros(plan.output_shape_sensor[1:], dtype=np.float32)
    tile_latency_ms: list[float] = []
    local_gains: list[float] = []
    overlap_squared_sum = 0.0
    overlap_max_abs = 0.0
    overlap_samples = 0

    for row in range(plan.grid_rows):
        weight_y = axis_weight(
            plan,
            has_previous=row > 0,
            has_next=row + 1 < plan.grid_rows,
        )
        packed_y = row * plan.step_packed
        for column in range(plan.grid_columns):
            weight_x = axis_weight(
                plan,
                has_previous=column > 0,
                has_next=column + 1 < plan.grid_columns,
            )
            packed_x = column * plan.step_packed
            tile_input = np.ascontiguousarray(
                padded[
                    :,
                    packed_y : packed_y + plan.tile_edge_packed,
                    packed_x : packed_x + plan.tile_edge_packed,
                ][None]
            )
            tile_started = time.perf_counter()
            tile_output = runner(tile_input)
            tile_latency_ms.append(
                (time.perf_counter() - tile_started) * 1000.0
            )
            expected_output_shape = (
                1,
                3,
                plan.tile_edge_packed * plan.output_scale,
                plan.tile_edge_packed * plan.output_scale,
            )
            if tile_output.shape != expected_output_shape:
                raise ValueError("tile runner returned an unexpected shape")
            if tile_output.dtype != np.float32:
                tile_output = tile_output.astype(np.float32)
            if not np.isfinite(tile_output).all():
                raise ValueError("tile runner returned non-finite output")
            input_mean = float(np.mean(tile_input, dtype=np.float64))
            output_mean = float(np.mean(tile_output, dtype=np.float64))
            if abs(output_mean) <= 1e-6 * abs(input_mean):
                raise ValueError("tile output cannot be gain diagnosed")
            local_gains.append(input_mean / output_mean)

            placement = tile_placement(plan, row, column)
            global_y, global_x, local_y, local_x = tile_output_slices(
                placement
            )
            local_weight = (
                weight_y[local_y, None] * weight_x[None, local_x]
            )
            tile_crop = tile_output[0, :, local_y, local_x]
            existing_weight = coverage[global_y, global_x]
            trusted_overlap = (existing_weight > 0.0) & (local_weight > 0.0)
            if np.any(trusted_overlap):
                existing = (
                    output[:, global_y, global_x][:, trusted_overlap]
                    / existing_weight[trusted_overlap][None]
                )
                difference = (
                    tile_crop[:, trusted_overlap] - existing
                ).astype(np.float64)
                overlap_squared_sum += float(np.sum(difference * difference))
                overlap_max_abs = max(
                    overlap_max_abs,
                    float(np.max(np.abs(difference))),
                )
                overlap_samples += difference.size
            output[:, global_y, global_x] += (
                tile_crop * local_weight[None]
            )
            coverage[global_y, global_x] += local_weight

    del padded
    coverage_min = float(np.min(coverage))
    coverage_max = float(np.max(coverage))
    coverage_error = float(np.max(np.abs(coverage - 1.0)))
    if coverage_min <= 0.0:
        raise ValueError("tile plan left uncovered output pixels")
    output /= coverage[None]
    raw_output_mean = float(np.mean(output, dtype=np.float64))
    input_mean = float(np.mean(packed, dtype=np.float64))
    if abs(raw_output_mean) <= 1e-6 * abs(input_mean):
        raise ValueError("stitched output cannot be globally gain matched")
    global_gain = input_mean / raw_output_mean
    output *= np.float32(global_gain)
    overlap_rmse = (
        0.0
        if overlap_samples == 0
        else abs(global_gain)
        * math.sqrt(overlap_squared_sum / overlap_samples)
    )
    output_squared_sum = 0.0
    negative_count = 0
    above_one_count = 0
    row_chunk = 256
    for row_start in range(0, output.shape[1], row_chunk):
        chunk = output[:, row_start : row_start + row_chunk]
        output_squared_sum += float(
            np.sum(chunk * chunk, dtype=np.float64)
        )
        negative_count += int(np.count_nonzero(chunk < 0.0))
        above_one_count += int(np.count_nonzero(chunk > 1.0))
    output_rms = math.sqrt(output_squared_sum / output.size)
    local_gain_min = min(local_gains)
    local_gain_max = max(local_gains)
    local_gain_midpoint = 0.5 * (abs(local_gain_min) + abs(local_gain_max))
    local_gain_span = (
        0.0
        if local_gain_midpoint == 0.0
        else abs(local_gain_max - local_gain_min) / local_gain_midpoint
    )
    receipt = TilingReceipt(
        plan=plan,
        tile_latency_p50_ms=_percentile(tile_latency_ms, 0.50),
        tile_latency_p95_ms=_percentile(tile_latency_ms, 0.95),
        tile_latency_samples=len(tile_latency_ms),
        wall_time_seconds=time.perf_counter() - started,
        global_input_mean=input_mean,
        raw_output_mean=raw_output_mean,
        global_gain=global_gain,
        local_gain_min=local_gain_min,
        local_gain_max=local_gain_max,
        local_gain_relative_span=local_gain_span,
        coverage_weight_min=coverage_min,
        coverage_weight_max=coverage_max,
        coverage_max_abs_error=coverage_error,
        trusted_overlap_samples=overlap_samples,
        trusted_overlap_max_abs=overlap_max_abs * abs(global_gain),
        trusted_overlap_rmse=overlap_rmse,
        trusted_overlap_relative_rmse=(
            0.0 if output_rms == 0.0 else overlap_rmse / output_rms
        ),
        output_min=float(np.min(output)),
        output_max=float(np.max(output)),
        output_mean=float(np.mean(output, dtype=np.float64)),
        output_negative_fraction=negative_count / output.size,
        output_above_one_fraction=above_one_count / output.size,
    )
    return FullImageRun(output=output, receipt=receipt)


def gate_failures(receipt: TilingReceipt) -> list[str]:
    failures: list[str] = []
    if receipt.coverage_max_abs_error > 1e-5:
        failures.append("tiling blend weights do not sum to one")
    if receipt.trusted_overlap_samples == 0:
        failures.append("tiling plan produced no trusted overlap samples")
    if receipt.trusted_overlap_relative_rmse > 0.01:
        failures.append("trusted tile-overlap RMSE exceeds 1% of output RMS")
    # Local gain is never applied. Its span describes scene/content variation
    # between tiles and remains diagnostic rather than a quality gate.
    return failures


def as_json(value: object) -> object:
    if hasattr(value, "__dataclass_fields__"):
        return asdict(value)
    raise TypeError(f"cannot serialize {type(value)!r}")
