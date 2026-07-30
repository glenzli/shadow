#!/usr/bin/env python3
"""Bounded two-pass stripe lifecycle for full-image RawNIND output."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
import math
import time
from typing import Callable, Protocol

import numpy as np

import tiling


DEFAULT_MAX_ADDITIONAL_WORKING_BYTES = 256 * 1024 * 1024
STRIPE_SCRATCH_BYTES = 64 * 1024 * 1024
STRIPE_SEQUENCE_FORMAT = "shadow-linear-camera-rgb-f32-stripe-chw-v1"

CancellationProbe = Callable[[], bool]


class StripeCancelled(RuntimeError):
    """Raised after an atomic stripe lifecycle has been cancelled."""


@dataclass(frozen=True)
class StripePlan:
    tiling: tiling.TilingPlan
    inference_passes: int
    max_buffer_rows_sensor: int
    max_emitted_stripe_rows_sensor: int
    source_input_bytes_borrowed: int
    accumulator_bytes: int
    tile_io_bytes: int
    estimated_scratch_bytes: int
    estimated_additional_working_bytes: int
    max_additional_working_bytes: int
    source_extraction: str


@dataclass(frozen=True)
class InferencePassReceipt:
    pass_index: int
    tile_count: int
    tile_latency_p50_ms: float
    tile_latency_p95_ms: float
    wall_time_seconds: float
    raw_output_mean: float
    emitted_stripes: int
    max_emitted_stripe_rows_sensor: int


@dataclass(frozen=True)
class SinkReceipt:
    format: str
    output_shape_sensor: list[int]
    byte_count: int
    stripe_count: int
    sequence_sha256: str
    committed: bool
    artifact_path: str | None = None
    artifact_cache_key_sha256: str | None = None
    artifact_identity_sha256: str | None = None
    artifact_file_sha256: str | None = None
    artifact_file_bytes: int | None = None


@dataclass(frozen=True)
class StripePublication:
    producer: str
    global_input_mean: float
    first_pass_raw_output_mean: float
    second_pass_raw_output_mean: float
    replay_relative_mean_delta: float
    global_gain: float
    output_mean: float


@dataclass(frozen=True)
class StripeReceipt:
    plan: StripePlan
    first_pass: InferencePassReceipt
    second_pass: InferencePassReceipt
    total_tile_inferences: int
    wall_time_seconds: float
    global_input_mean: float
    first_pass_raw_output_mean: float
    second_pass_raw_output_mean: float
    replay_relative_mean_delta: float
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
    sink: SinkReceipt


@dataclass(frozen=True)
class StripeRun:
    receipt: StripeReceipt


class StripeSink(Protocol):
    def begin(self, output_shape_sensor: tuple[int, int, int]) -> None:
        """Reserve an unpublished destination for one complete output."""

    def write_stripe(self, y_start: int, stripe: np.ndarray) -> None:
        """Synchronously consume one contiguous logical output stripe."""

    def commit(self, publication: StripePublication) -> SinkReceipt:
        """Atomically publish the completed destination."""

    def abort(self) -> None:
        """Discard any unpublished destination state."""


class DigestStripeSink:
    """A no-payload sink that proves ordered writes and atomic completion."""

    def __init__(self) -> None:
        self._state = "idle"
        self._shape: tuple[int, int, int] | None = None
        self._next_y = 0
        self._stripe_count = 0
        self._byte_count = 0
        self._hasher: hashlib._Hash | None = None
        self._receipt: SinkReceipt | None = None

    @property
    def state(self) -> str:
        return self._state

    @property
    def output_shape_sensor(self) -> tuple[int, int, int] | None:
        return self._shape

    @property
    def stripe_count(self) -> int:
        return self._stripe_count

    @property
    def byte_count(self) -> int:
        return self._byte_count

    @property
    def sequence_sha256(self) -> str:
        if self._hasher is None:
            raise RuntimeError("stripe sink has no sequence identity")
        return self._hasher.copy().hexdigest()

    def begin(self, output_shape_sensor: tuple[int, int, int]) -> None:
        if self._state != "idle":
            raise RuntimeError("stripe sink can only begin once")
        if (
            len(output_shape_sensor) != 3
            or output_shape_sensor[0] != 3
            or min(output_shape_sensor) <= 0
        ):
            raise ValueError("stripe sink requires a positive 3-channel shape")
        self._shape = output_shape_sensor
        self._hasher = hashlib.sha256()
        self._hasher.update(sequence_header_bytes(output_shape_sensor))
        self._state = "writing"

    def write_stripe(self, y_start: int, stripe: np.ndarray) -> None:
        if self._state != "writing" or self._shape is None:
            raise RuntimeError("stripe sink is not accepting output")
        if y_start != self._next_y:
            raise ValueError("stripe writes must be contiguous and ordered")
        if (
            stripe.dtype != np.float32
            or stripe.ndim != 3
            or stripe.shape[0] != self._shape[0]
            or stripe.shape[2] != self._shape[2]
            or stripe.shape[1] <= 0
            or y_start + stripe.shape[1] > self._shape[1]
        ):
            raise ValueError("stripe does not match the reserved output")
        if not np.isfinite(stripe).all():
            raise ValueError("stripe sink received non-finite output")
        assert self._hasher is not None
        for channel in stripe:
            if not channel.flags.c_contiguous:
                raise ValueError("each stripe channel must be contiguous")
            self._hasher.update(memoryview(channel).cast("B"))
        self._next_y += stripe.shape[1]
        self._stripe_count += 1
        self._byte_count += stripe.size * stripe.dtype.itemsize

    def _validate_commit(
        self,
        publication: StripePublication,
    ) -> tuple[int, int, int]:
        if self._state != "writing" or self._shape is None:
            raise RuntimeError("stripe sink has no active output")
        if self._next_y != self._shape[1]:
            raise RuntimeError("stripe sink cannot commit an incomplete output")
        validate_publication(publication)
        return self._shape

    def commit(self, publication: StripePublication) -> SinkReceipt:
        shape = self._validate_commit(publication)
        receipt = SinkReceipt(
            format=STRIPE_SEQUENCE_FORMAT,
            output_shape_sensor=list(shape),
            byte_count=self._byte_count,
            stripe_count=self._stripe_count,
            sequence_sha256=self.sequence_sha256,
            committed=True,
        )
        return self._mark_committed(receipt)

    def _mark_committed(self, receipt: SinkReceipt) -> SinkReceipt:
        if self._state != "writing":
            raise RuntimeError("stripe sink cannot finish outside a write")
        self._receipt = receipt
        self._state = "committed"
        return receipt

    def abort(self) -> None:
        if self._state == "committed":
            raise RuntimeError("a committed stripe sink cannot be aborted")
        self._shape = None
        self._hasher = None
        self._receipt = None
        self._state = "aborted"


class ArrayStripeSink(DigestStripeSink):
    """Reference-only sink used to compare stripes with full-frame output."""

    def __init__(self) -> None:
        super().__init__()
        self.output: np.ndarray | None = None

    def begin(self, output_shape_sensor: tuple[int, int, int]) -> None:
        super().begin(output_shape_sensor)
        self.output = np.empty(output_shape_sensor, dtype=np.float32)

    def write_stripe(self, y_start: int, stripe: np.ndarray) -> None:
        if self.output is None:
            raise RuntimeError("array stripe sink has no reserved output")
        self.output[:, y_start : y_start + stripe.shape[1]] = stripe
        super().write_stripe(y_start, stripe)

    def abort(self) -> None:
        if self.state == "committed":
            super().abort()
        self.output = None
        super().abort()


def sequence_header_bytes(
    output_shape_sensor: tuple[int, int, int],
) -> bytes:
    return json.dumps(
        {
            "format": STRIPE_SEQUENCE_FORMAT,
            "shape": list(output_shape_sensor),
        },
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def validate_publication(publication: StripePublication) -> None:
    values = (
        publication.global_input_mean,
        publication.first_pass_raw_output_mean,
        publication.second_pass_raw_output_mean,
        publication.replay_relative_mean_delta,
        publication.global_gain,
        publication.output_mean,
    )
    if publication.producer != "rawnind-bayer-two-pass-stripe-v1":
        raise ValueError("stripe publication producer changed")
    if not all(math.isfinite(value) for value in values):
        raise ValueError("stripe publication contains non-finite values")
    if publication.replay_relative_mean_delta < 0.0:
        raise ValueError("stripe replay delta must be non-negative")
    raw_scale = 0.5 * (
        abs(publication.first_pass_raw_output_mean)
        + abs(publication.second_pass_raw_output_mean)
    )
    expected_replay_delta = (
        0.0
        if raw_scale == 0.0
        else abs(
            publication.first_pass_raw_output_mean
            - publication.second_pass_raw_output_mean
        )
        / raw_scale
    )
    if not math.isclose(
        publication.replay_relative_mean_delta,
        expected_replay_delta,
        rel_tol=1e-12,
        abs_tol=1e-15,
    ):
        raise ValueError("stripe publication replay delta is inconsistent")
    if abs(publication.first_pass_raw_output_mean) <= (
        1e-6 * abs(publication.global_input_mean)
    ):
        raise ValueError("stripe publication raw mean is degenerate")
    expected_gain = (
        publication.global_input_mean
        / publication.first_pass_raw_output_mean
    )
    if not math.isclose(
        publication.global_gain,
        expected_gain,
        rel_tol=1e-12,
        abs_tol=0.0,
    ):
        raise ValueError("stripe publication global gain is inconsistent")
    input_scale = max(abs(publication.global_input_mean), 1e-12)
    if (
        abs(publication.output_mean - publication.global_input_mean)
        / input_scale
        > 1e-6
    ):
        raise ValueError("stripe publication output mean is inconsistent")


@dataclass
class _PassResult:
    receipt: InferencePassReceipt
    raw_sum_by_channel: np.ndarray
    value_count: int
    local_gains: list[float]
    coverage_min: float
    coverage_max: float
    coverage_max_error: float
    overlap_squared_sum: float
    overlap_max_abs: float
    overlap_samples: int
    output_sum_by_channel: np.ndarray
    output_squared_sum: float
    output_min: float
    output_max: float
    negative_count: int
    above_one_count: int


def _row_output_end(plan: tiling.TilingPlan, row: int) -> int:
    return tiling.tile_placement(plan, row, 0).global_y1


def _first_positive_output_row(
    plan: tiling.TilingPlan,
    row: int,
) -> int:
    placement = tiling.tile_placement(plan, row, 0)
    weights = tiling.axis_weight(
        plan,
        has_previous=row > 0,
        has_next=row + 1 < plan.grid_rows,
    )
    local = weights[placement.local_y0 : placement.local_y1]
    positive = np.flatnonzero(local > 0.0)
    if positive.size == 0:
        raise ValueError("tile row has no trusted output support")
    return placement.global_y0 + int(positive[0])


def _finalization_end(plan: tiling.TilingPlan, row: int) -> int:
    if row + 1 == plan.grid_rows:
        return plan.output_shape_sensor[1]
    return _first_positive_output_row(plan, row + 1)


def plan_striped_image(
    height_packed: int,
    width_packed: int,
    *,
    exact_halo_packed: int = tiling.DEFAULT_EXACT_HALO_PACKED,
    blend_overlap_packed: int = tiling.DEFAULT_BLEND_OVERLAP_PACKED,
    max_additional_working_bytes: int = (
        DEFAULT_MAX_ADDITIONAL_WORKING_BYTES
    ),
) -> StripePlan:
    geometry = tiling.plan_image_geometry(
        height_packed,
        width_packed,
        exact_halo_packed=exact_halo_packed,
        blend_overlap_packed=blend_overlap_packed,
    )
    buffer_start = 0
    max_buffer_rows = 0
    max_emitted_rows = 0
    for row in range(geometry.grid_rows):
        row_end = _row_output_end(geometry, row)
        max_buffer_rows = max(max_buffer_rows, row_end - buffer_start)
        finalize_end = _finalization_end(geometry, row)
        max_emitted_rows = max(
            max_emitted_rows,
            finalize_end - buffer_start,
        )
        buffer_start = finalize_end
    if buffer_start != geometry.output_shape_sensor[1]:
        raise ValueError("stripe plan does not finalize the complete output")

    itemsize = np.dtype(np.float32).itemsize
    output_width = geometry.output_shape_sensor[2]
    accumulator_bytes = (
        4 * max_buffer_rows * output_width * itemsize
    )
    tile_io_bytes = (
        (
            4 * geometry.tile_edge_packed * geometry.tile_edge_packed
            + 3
            * geometry.tile_edge_packed
            * geometry.output_scale
            * geometry.tile_edge_packed
            * geometry.output_scale
        )
        * itemsize
    )
    estimated = accumulator_bytes + tile_io_bytes + STRIPE_SCRATCH_BYTES
    if estimated > max_additional_working_bytes:
        raise ValueError(
            "striped lifecycle exceeds the configured additional "
            f"working-memory budget: {estimated} > "
            f"{max_additional_working_bytes}"
        )
    return StripePlan(
        tiling=geometry,
        inference_passes=2,
        max_buffer_rows_sensor=max_buffer_rows,
        max_emitted_stripe_rows_sensor=max_emitted_rows,
        source_input_bytes_borrowed=(
            4 * height_packed * width_packed * itemsize
        ),
        accumulator_bytes=accumulator_bytes,
        tile_io_bytes=tile_io_bytes,
        estimated_scratch_bytes=STRIPE_SCRATCH_BYTES,
        estimated_additional_working_bytes=estimated,
        max_additional_working_bytes=max_additional_working_bytes,
        source_extraction="direct-reflect-from-borrowed-packed-input",
    )


def _check_cancelled(cancelled: CancellationProbe | None) -> None:
    if cancelled is not None and cancelled():
        raise StripeCancelled("striped RawNIND lifecycle was cancelled")


def _percentile(values: list[float], fraction: float) -> float:
    if not values:
        raise ValueError("cannot take percentile of an empty collection")
    ordered = sorted(values)
    index = min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1)
    return ordered[max(index, 0)]


def _run_pass(
    packed: np.ndarray,
    runner: tiling.TileRunner,
    plan: StripePlan,
    *,
    pass_index: int,
    global_gain: float | None,
    sink: StripeSink | None,
    cancelled: CancellationProbe | None,
    collect_diagnostics: bool,
) -> _PassResult:
    started = time.perf_counter()
    geometry = plan.tiling
    output_width = geometry.output_shape_sensor[2]
    accumulator = np.zeros(
        (3, plan.max_buffer_rows_sensor, output_width),
        dtype=np.float32,
    )
    coverage = np.zeros(
        (plan.max_buffer_rows_sensor, output_width),
        dtype=np.float32,
    )
    weights_x = [
        tiling.axis_weight(
            geometry,
            has_previous=column > 0,
            has_next=column + 1 < geometry.grid_columns,
        )
        for column in range(geometry.grid_columns)
    ]
    tile_latency_ms: list[float] = []
    local_gains: list[float] = []
    raw_sum_by_channel = np.zeros(3, dtype=np.float64)
    output_sum_by_channel = np.zeros(3, dtype=np.float64)
    output_squared_sum = 0.0
    output_min = math.inf
    output_max = -math.inf
    negative_count = 0
    above_one_count = 0
    value_count = 0
    coverage_min = math.inf
    coverage_max = -math.inf
    coverage_max_error = 0.0
    overlap_squared_sum = 0.0
    overlap_max_abs = 0.0
    overlap_samples = 0
    emitted_stripes = 0
    max_emitted_rows = 0
    buffer_start = 0
    active_end = 0

    for row in range(geometry.grid_rows):
        _check_cancelled(cancelled)
        row_end = _row_output_end(geometry, row)
        if row_end > active_end:
            old_active_rows = active_end - buffer_start
            new_active_rows = row_end - buffer_start
            if new_active_rows > plan.max_buffer_rows_sensor:
                raise ValueError("stripe accumulator exceeded its plan")
            accumulator[:, old_active_rows:new_active_rows] = 0.0
            coverage[old_active_rows:new_active_rows] = 0.0
            active_end = row_end

        weight_y = tiling.axis_weight(
            geometry,
            has_previous=row > 0,
            has_next=row + 1 < geometry.grid_rows,
        )
        for column in range(geometry.grid_columns):
            _check_cancelled(cancelled)
            placement = tiling.tile_placement(geometry, row, column)
            tile_input = tiling.extract_tile(packed, geometry, placement)
            tile_started = time.perf_counter()
            tile_output = runner(tile_input)
            tile_latency_ms.append(
                (time.perf_counter() - tile_started) * 1000.0
            )
            expected_shape = (
                1,
                3,
                geometry.tile_edge_packed * geometry.output_scale,
                geometry.tile_edge_packed * geometry.output_scale,
            )
            if tile_output.shape != expected_shape:
                raise ValueError("tile runner returned an unexpected shape")
            if tile_output.dtype != np.float32:
                tile_output = tile_output.astype(np.float32)
            if not np.isfinite(tile_output).all():
                raise ValueError("tile runner returned non-finite output")

            input_mean = float(np.mean(tile_input, dtype=np.float64))
            output_mean = float(np.mean(tile_output, dtype=np.float64))
            if abs(output_mean) <= 1e-6 * abs(input_mean):
                raise ValueError("tile output cannot be gain diagnosed")
            if collect_diagnostics:
                local_gains.append(input_mean / output_mean)

            global_y0 = max(placement.global_y0, buffer_start)
            global_y1 = placement.global_y1
            if global_y1 <= global_y0:
                continue
            local_y0 = (
                placement.local_y0 + global_y0 - placement.global_y0
            )
            local_y1 = local_y0 + global_y1 - global_y0
            local_x = slice(placement.local_x0, placement.local_x1)
            buffer_y = slice(
                global_y0 - buffer_start,
                global_y1 - buffer_start,
            )
            global_x = slice(placement.global_x0, placement.global_x1)
            local_weight = (
                weight_y[local_y0:local_y1, None]
                * weights_x[column][None, local_x]
            )
            tile_crop = tile_output[
                0,
                :,
                local_y0:local_y1,
                local_x,
            ]
            accumulator_view = accumulator[:, buffer_y, global_x]
            coverage_view = coverage[buffer_y, global_x]
            if collect_diagnostics:
                trusted_overlap = (
                    (coverage_view > 0.0) & (local_weight > 0.0)
                )
                if np.any(trusted_overlap):
                    existing = (
                        accumulator_view[:, trusted_overlap]
                        / coverage_view[trusted_overlap][None]
                    )
                    difference = (
                        tile_crop[:, trusted_overlap] - existing
                    ).astype(np.float64)
                    overlap_squared_sum += float(
                        np.sum(difference * difference)
                    )
                    overlap_max_abs = max(
                        overlap_max_abs,
                        float(np.max(np.abs(difference))),
                    )
                    overlap_samples += difference.size
            accumulator_view += tile_crop * local_weight[None]
            coverage_view += local_weight

        finalize_end = _finalization_end(geometry, row)
        emitted_rows = finalize_end - buffer_start
        if emitted_rows <= 0:
            raise ValueError("stripe plan emitted an empty stripe")
        finalized_coverage = coverage[:emitted_rows]
        stripe_coverage_min = float(np.min(finalized_coverage))
        stripe_coverage_max = float(np.max(finalized_coverage))
        if stripe_coverage_min <= 0.0:
            raise ValueError("stripe plan left uncovered output pixels")
        coverage_min = min(coverage_min, stripe_coverage_min)
        coverage_max = max(coverage_max, stripe_coverage_max)
        coverage_max_error = max(
            coverage_max_error,
            float(np.max(np.abs(finalized_coverage - 1.0))),
        )

        stripe = accumulator[:, :emitted_rows]
        stripe /= finalized_coverage[None]
        raw_sum_by_channel += np.sum(
            stripe,
            axis=(1, 2),
            dtype=np.float64,
        )
        value_count += stripe.size
        if global_gain is not None:
            stripe *= np.float32(global_gain)
        if sink is not None:
            sink.write_stripe(buffer_start, stripe)
        output_sum_by_channel += np.sum(
            stripe,
            axis=(1, 2),
            dtype=np.float64,
        )
        output_squared_sum += float(
            np.sum(stripe * stripe, dtype=np.float64)
        )
        output_min = min(output_min, float(np.min(stripe)))
        output_max = max(output_max, float(np.max(stripe)))
        negative_count += int(np.count_nonzero(stripe < 0.0))
        above_one_count += int(np.count_nonzero(stripe > 1.0))
        emitted_stripes += 1
        max_emitted_rows = max(max_emitted_rows, emitted_rows)

        retained_rows = active_end - finalize_end
        previous_active_rows = active_end - buffer_start
        if retained_rows:
            accumulator[:, :retained_rows] = accumulator[
                :,
                emitted_rows : emitted_rows + retained_rows,
            ].copy()
            coverage[:retained_rows] = coverage[
                emitted_rows : emitted_rows + retained_rows
            ].copy()
        accumulator[:, retained_rows:previous_active_rows] = 0.0
        coverage[retained_rows:previous_active_rows] = 0.0
        buffer_start = finalize_end

    if buffer_start != geometry.output_shape_sensor[1]:
        raise ValueError("stripe pass did not finalize the complete output")
    raw_output_mean = float(
        np.sum(raw_sum_by_channel, dtype=np.float64) / value_count
    )
    return _PassResult(
        receipt=InferencePassReceipt(
            pass_index=pass_index,
            tile_count=len(tile_latency_ms),
            tile_latency_p50_ms=_percentile(tile_latency_ms, 0.50),
            tile_latency_p95_ms=_percentile(tile_latency_ms, 0.95),
            wall_time_seconds=time.perf_counter() - started,
            raw_output_mean=raw_output_mean,
            emitted_stripes=emitted_stripes,
            max_emitted_stripe_rows_sensor=max_emitted_rows,
        ),
        raw_sum_by_channel=raw_sum_by_channel,
        value_count=value_count,
        local_gains=local_gains,
        coverage_min=coverage_min,
        coverage_max=coverage_max,
        coverage_max_error=coverage_max_error,
        overlap_squared_sum=overlap_squared_sum,
        overlap_max_abs=overlap_max_abs,
        overlap_samples=overlap_samples,
        output_sum_by_channel=output_sum_by_channel,
        output_squared_sum=output_squared_sum,
        output_min=output_min,
        output_max=output_max,
        negative_count=negative_count,
        above_one_count=above_one_count,
    )


def run_striped_image(
    packed: np.ndarray,
    runner: tiling.TileRunner,
    plan: StripePlan,
    sink: StripeSink,
    *,
    cancelled: CancellationProbe | None = None,
) -> StripeRun:
    tiling.validate_input(packed, plan.tiling)
    started = time.perf_counter()
    begun = False
    try:
        sink.begin(tuple(plan.tiling.output_shape_sensor))
        begun = True
        first = _run_pass(
            packed,
            runner,
            plan,
            pass_index=1,
            global_gain=None,
            sink=None,
            cancelled=cancelled,
            collect_diagnostics=False,
        )
        input_mean = float(np.mean(packed, dtype=np.float64))
        first_raw_mean = first.receipt.raw_output_mean
        if abs(first_raw_mean) <= 1e-6 * abs(input_mean):
            raise ValueError("striped output cannot be globally gain matched")
        global_gain = input_mean / first_raw_mean
        second = _run_pass(
            packed,
            runner,
            plan,
            pass_index=2,
            global_gain=global_gain,
            sink=sink,
            cancelled=cancelled,
            collect_diagnostics=True,
        )
        _check_cancelled(cancelled)
        second_raw_mean = second.receipt.raw_output_mean
        replay_denominator = 0.5 * (
            abs(first_raw_mean) + abs(second_raw_mean)
        )
        replay_delta = (
            0.0
            if replay_denominator == 0.0
            else abs(first_raw_mean - second_raw_mean) / replay_denominator
        )
        output_mean = float(
            np.sum(second.output_sum_by_channel, dtype=np.float64)
            / second.value_count
        )
        publication = StripePublication(
            producer="rawnind-bayer-two-pass-stripe-v1",
            global_input_mean=input_mean,
            first_pass_raw_output_mean=first_raw_mean,
            second_pass_raw_output_mean=second_raw_mean,
            replay_relative_mean_delta=replay_delta,
            global_gain=global_gain,
            output_mean=output_mean,
        )
        sink_receipt = sink.commit(publication)
    except BaseException:
        if begun:
            try:
                sink.abort()
            except BaseException:
                pass
        raise

    local_gain_min = min(second.local_gains)
    local_gain_max = max(second.local_gains)
    local_gain_midpoint = 0.5 * (
        abs(local_gain_min) + abs(local_gain_max)
    )
    local_gain_span = (
        0.0
        if local_gain_midpoint == 0.0
        else abs(local_gain_max - local_gain_min) / local_gain_midpoint
    )
    overlap_rmse = (
        0.0
        if second.overlap_samples == 0
        else abs(global_gain)
        * math.sqrt(
            second.overlap_squared_sum / second.overlap_samples
        )
    )
    output_rms = math.sqrt(
        second.output_squared_sum / second.value_count
    )
    receipt = StripeReceipt(
        plan=plan,
        first_pass=first.receipt,
        second_pass=second.receipt,
        total_tile_inferences=(
            first.receipt.tile_count + second.receipt.tile_count
        ),
        wall_time_seconds=time.perf_counter() - started,
        global_input_mean=input_mean,
        first_pass_raw_output_mean=first_raw_mean,
        second_pass_raw_output_mean=second_raw_mean,
        replay_relative_mean_delta=replay_delta,
        global_gain=global_gain,
        local_gain_min=local_gain_min,
        local_gain_max=local_gain_max,
        local_gain_relative_span=local_gain_span,
        coverage_weight_min=second.coverage_min,
        coverage_weight_max=second.coverage_max,
        coverage_max_abs_error=second.coverage_max_error,
        trusted_overlap_samples=second.overlap_samples,
        trusted_overlap_max_abs=(
            second.overlap_max_abs * abs(global_gain)
        ),
        trusted_overlap_rmse=overlap_rmse,
        trusted_overlap_relative_rmse=(
            0.0 if output_rms == 0.0 else overlap_rmse / output_rms
        ),
        output_min=second.output_min,
        output_max=second.output_max,
        output_mean=output_mean,
        output_negative_fraction=(
            second.negative_count / second.value_count
        ),
        output_above_one_fraction=(
            second.above_one_count / second.value_count
        ),
        sink=sink_receipt,
    )
    return StripeRun(receipt=receipt)


def gate_failures(receipt: StripeReceipt) -> list[str]:
    failures: list[str] = []
    if receipt.coverage_max_abs_error > 1e-5:
        failures.append("stripe blend weights do not sum to one")
    if receipt.trusted_overlap_samples == 0:
        failures.append("stripe plan produced no trusted overlap samples")
    if receipt.trusted_overlap_relative_rmse > 0.01:
        failures.append("trusted stripe-overlap RMSE exceeds 1% of output RMS")
    # Local gain is never applied. Its span describes scene/content variation
    # between tiles and remains diagnostic rather than a publication gate.
    if receipt.replay_relative_mean_delta > 1e-6:
        failures.append("two inference passes are not mean-deterministic")
    input_scale = max(abs(receipt.global_input_mean), 1e-12)
    if abs(receipt.output_mean - receipt.global_input_mean) / input_scale > 1e-6:
        failures.append("global gain did not preserve the input mean")
    if not receipt.sink.committed:
        failures.append("stripe sink did not commit atomically")
    return failures


def as_json(value: object) -> object:
    if hasattr(value, "__dataclass_fields__"):
        return asdict(value)
    raise TypeError(f"cannot serialize {type(value)!r}")
