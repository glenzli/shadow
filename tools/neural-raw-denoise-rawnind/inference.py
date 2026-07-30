#!/usr/bin/env python3
"""Inference and preprocessing contracts for the public RawNIND Bayer model."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import math
from pathlib import Path
import time
from typing import Any

import numpy as np
import onnxruntime as ort

import raw_frame_staging


TILE_EDGE = 512
OUTPUT_SCALE = 2
INPUT_CHANNELS = 4
OUTPUT_CHANNELS = 3
MODEL_RECEPTIVE_FIELD_PACKED = 200
CONSERVATIVE_EXACT_HALO_PACKED = 100
DARKTABLE_OVERLAP_PACKED = 32
DEMO_OVERLAP_PACKED = 64


@dataclass(frozen=True)
class BackendRun:
    requested_provider: str
    active_providers: list[str]
    load_ms: float
    first_prediction_ms: float
    warm_prediction_p50_ms: float
    warm_prediction_p95_ms: float
    warm_prediction_samples: int
    raw_output_mean: float
    matched_output_min: float
    matched_output_max: float
    matched_output_mean: float


@dataclass(frozen=True)
class BackendParity:
    cpu: BackendRun
    coreml: BackendRun | None
    coreml_vs_cpu_max_abs: float | None
    coreml_vs_cpu_rmse: float | None


@dataclass(frozen=True)
class RawTileReceipt:
    source: str
    sensor_shape: list[int]
    packed_shape: list[int]
    raw_pattern: list[list[int]]
    source_raw_pattern: list[list[int]]
    force_rggb_crop_sensor: list[int]
    color_description: str
    white_level: float
    black_level_per_channel: list[float]
    sample_origin_packed: list[int]
    sample_input_min: float
    sample_input_max: float
    sample_input_mean: float
    matched_output_min: float
    matched_output_max: float
    matched_output_mean: float
    matched_output_negative_fraction: float
    matched_output_above_one_fraction: float


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        raise ValueError("cannot take percentile of an empty collection")
    ordered = sorted(values)
    index = min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1)
    return ordered[max(index, 0)]


def representative_packed_bayer(
    height: int = TILE_EDGE,
    width: int = TILE_EDGE,
) -> np.ndarray:
    if height < TILE_EDGE or width < TILE_EDGE:
        raise ValueError("representative input must contain one complete tile")
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    xx /= max(width - 1, 1)
    yy /= max(height - 1, 1)
    texture = 0.015 * np.sin(xx * 37.0) * np.cos(yy * 29.0)
    planes = [
        0.035 + 0.32 * xx + 0.08 * yy + texture,
        0.030 + 0.27 * xx + 0.10 * yy - texture,
        0.032 + 0.26 * xx + 0.11 * yy + texture,
        0.025 + 0.20 * xx + 0.14 * yy - texture,
    ]
    return np.ascontiguousarray(
        np.clip(np.stack(planes, axis=0), 0.0, 1.0)[None].astype(np.float32)
    )


def match_gain(output: np.ndarray, anchor: np.ndarray) -> np.ndarray:
    if output.shape[0] != 1 or output.shape[1] != OUTPUT_CHANNELS:
        raise ValueError("RawNIND Bayer output must have shape [1, 3, H, W]")
    if anchor.shape[0] != 1 or anchor.shape[1] != INPUT_CHANNELS:
        raise ValueError("RawNIND Bayer input must have shape [1, 4, H, W]")
    output_mean = float(np.mean(output, dtype=np.float64))
    anchor_mean = float(np.mean(anchor, dtype=np.float64))
    threshold = 1e-6 * abs(anchor_mean)
    if not math.isfinite(output_mean) or abs(output_mean) <= threshold:
        raise ValueError("RawNIND output mean cannot be gain matched")
    matched = output.astype(np.float32, copy=True)
    matched *= np.float32(anchor_mean / output_mean)
    if not np.isfinite(matched).all():
        raise ValueError("RawNIND gain-matched output is not finite")
    return matched


def _providers(requested_provider: str) -> list[str]:
    available = ort.get_available_providers()
    if requested_provider == "cpu":
        return ["CPUExecutionProvider"]
    if requested_provider == "coreml":
        if "CoreMLExecutionProvider" not in available:
            raise ValueError("ONNX Runtime does not expose CoreMLExecutionProvider")
        return ["CoreMLExecutionProvider", "CPUExecutionProvider"]
    raise ValueError(f"unsupported RawNIND provider: {requested_provider}")


def create_session(
    model_path: Path,
    requested_provider: str,
) -> tuple[ort.InferenceSession, float]:
    started = time.perf_counter()
    session = ort.InferenceSession(
        str(model_path),
        providers=_providers(requested_provider),
    )
    load_ms = (time.perf_counter() - started) * 1000.0
    model_input = session.get_inputs()
    model_output = session.get_outputs()
    if len(model_input) != 1 or len(model_output) != 1:
        raise ValueError("RawNIND Bayer graph must have one input and one output")
    if model_input[0].name != "input" or model_input[0].shape != [1, 4, 512, 512]:
        raise ValueError("RawNIND Bayer ONNX input contract changed")
    if model_output[0].name != "output" or model_output[0].shape != [
        1,
        3,
        1024,
        1024,
    ]:
        raise ValueError("RawNIND Bayer ONNX output contract changed")
    return session, load_ms


def run_session_tile(
    session: ort.InferenceSession,
    model_input: np.ndarray,
    warm_runs: int,
) -> tuple[np.ndarray, float, list[float]]:
    if model_input.shape != (1, 4, 512, 512):
        raise ValueError("RawNIND inference requires one static 512-pixel tile")
    started = time.perf_counter()
    output = session.run(None, {"input": model_input})[0]
    first_prediction_ms = (time.perf_counter() - started) * 1000.0
    warm_ms: list[float] = []
    for _ in range(warm_runs):
        started = time.perf_counter()
        session.run(None, {"input": model_input})
        warm_ms.append((time.perf_counter() - started) * 1000.0)
    return output, first_prediction_ms, warm_ms


def run_tile(
    model_path: Path,
    model_input: np.ndarray,
    requested_provider: str,
    warm_runs: int,
) -> tuple[np.ndarray, BackendRun]:
    session, load_ms = create_session(model_path, requested_provider)
    raw_output, first_prediction_ms, warm_ms = run_session_tile(
        session,
        model_input,
        warm_runs,
    )
    if not np.isfinite(raw_output).all():
        raise ValueError(f"{requested_provider} RawNIND output is not finite")
    matched = match_gain(raw_output, model_input)
    if warm_ms:
        p50 = percentile(warm_ms, 0.50)
        p95 = percentile(warm_ms, 0.95)
    else:
        p50 = 0.0
        p95 = 0.0
    run = BackendRun(
        requested_provider=requested_provider,
        active_providers=session.get_providers(),
        load_ms=load_ms,
        first_prediction_ms=first_prediction_ms,
        warm_prediction_p50_ms=p50,
        warm_prediction_p95_ms=p95,
        warm_prediction_samples=len(warm_ms),
        raw_output_mean=float(np.mean(raw_output, dtype=np.float64)),
        matched_output_min=float(matched.min()),
        matched_output_max=float(matched.max()),
        matched_output_mean=float(np.mean(matched, dtype=np.float64)),
    )
    return matched, run


def audit_backend_parity(
    model_path: Path,
    warm_runs: int,
) -> tuple[BackendParity, np.ndarray]:
    model_input = representative_packed_bayer()
    cpu_output, cpu_run = run_tile(
        model_path,
        model_input,
        "cpu",
        warm_runs,
    )
    if "CoreMLExecutionProvider" not in ort.get_available_providers():
        return (
            BackendParity(
                cpu=cpu_run,
                coreml=None,
                coreml_vs_cpu_max_abs=None,
                coreml_vs_cpu_rmse=None,
            ),
            model_input,
        )
    coreml_output, coreml_run = run_tile(
        model_path,
        model_input,
        "coreml",
        warm_runs,
    )
    difference = coreml_output - cpu_output
    parity = BackendParity(
        cpu=cpu_run,
        coreml=coreml_run,
        coreml_vs_cpu_max_abs=float(np.max(np.abs(difference))),
        coreml_vs_cpu_rmse=float(
            np.sqrt(np.mean(difference * difference, dtype=np.float64))
        ),
    )
    return parity, model_input


def _color_description(raw: Any) -> str:
    description = raw.color_desc
    if isinstance(description, bytes):
        return description.decode("ascii")
    return str(description)


def force_rggb_geometry(
    pattern: np.ndarray,
    color_description: str,
) -> tuple[int, int, np.ndarray]:
    if pattern.shape != (2, 2):
        raise ValueError("RawNIND Bayer source must have a 2x2 CFA pattern")
    red_sites = [
        (row, column)
        for row in range(2)
        for column in range(2)
        if color_description[int(pattern[row, column])] == "R"
    ]
    if len(red_sites) != 1:
        raise ValueError("source does not contain exactly one red Bayer site")
    row_offset, column_offset = red_sites[0]
    forced = np.empty_like(pattern)
    for row in range(2):
        for column in range(2):
            forced[row, column] = pattern[
                (row + row_offset) % 2,
                (column + column_offset) % 2,
            ]
    forced_name = "".join(
        color_description[int(index)] for index in forced.flatten()
    )
    if forced_name != "RGGB":
        raise ValueError(f"cannot force Bayer source to RGGB: {forced_name}")
    return row_offset, column_offset, forced


def load_raw_as_packed_bayer(
    raw_path: Path,
    staged_raw_frame: Path | None = None,
) -> tuple[np.ndarray, dict[str, object]]:
    if staged_raw_frame is not None:
        staged = raw_frame_staging.load(staged_raw_frame)
        pattern = np.arange(4, dtype=np.uint8).reshape(2, 2)
        return _pack_normalized_bayer(
            staged.mosaic,
            pattern,
            staged.cfa,
            np.asarray(staged.black_levels, dtype=np.float32),
            staged.white_levels[0],
            decoder_provider_id=staged.provider_id,
            decoder_provider_version=staged.provider_version,
            decoded_samples_sha256=staged.samples_sha256,
        )

    import rawpy

    with rawpy.imread(str(raw_path)) as raw:
        pattern = raw.raw_pattern
        if pattern is None or pattern.shape != (2, 2):
            raise ValueError("RawNIND Bayer audit requires a 2x2 Bayer source")
        description = _color_description(raw)
        visible = raw.raw_image_visible.astype(np.uint16, copy=True)
        decoded_samples_sha256 = hashlib.sha256(
            visible.astype("<u2", copy=False).tobytes()
        ).hexdigest()
        return _pack_normalized_bayer(
            visible,
            pattern,
            description,
            np.asarray(raw.black_level_per_channel, dtype=np.float32),
            float(raw.white_level),
            decoder_provider_id="rawpy",
            decoder_provider_version=rawpy.__version__,
            decoded_samples_sha256=decoded_samples_sha256,
        )


def _pack_normalized_bayer(
    source_mosaic: np.ndarray,
    pattern: np.ndarray,
    description: str,
    black_levels: np.ndarray,
    white_level: float,
    *,
    decoder_provider_id: str,
    decoder_provider_version: str,
    decoded_samples_sha256: str,
) -> tuple[np.ndarray, dict[str, object]]:
    row_offset, column_offset, forced_pattern = force_rggb_geometry(
        pattern,
        description,
    )
    mosaic = source_mosaic.astype(np.float32, copy=True)
    row_end = mosaic.shape[0] - row_offset if row_offset else mosaic.shape[0]
    column_end = (
        mosaic.shape[1] - column_offset
        if column_offset
        else mosaic.shape[1]
    )
    mosaic = mosaic[row_offset:row_end, column_offset:column_end]
    if mosaic.shape[0] % 2 or mosaic.shape[1] % 2:
        mosaic = mosaic[: mosaic.shape[0] & ~1, : mosaic.shape[1] & ~1]
    sensor_shape = [int(mosaic.shape[0]), int(mosaic.shape[1])]
    planes: list[np.ndarray] = []
    for row in range(2):
        for column in range(2):
            color_index = int(forced_pattern[row, column])
            black = float(black_levels[color_index])
            value_range = max(white_level - black, 1.0)
            plane = np.clip(
                (mosaic[row::2, column::2] - black) / value_range,
                0.0,
                1.0,
            )
            planes.append(plane)
    packed = np.stack(planes, axis=0).astype(np.float32)
    metadata: dict[str, object] = {
        "sensor_shape": sensor_shape,
        "packed_shape": [int(value) for value in packed.shape],
        "raw_pattern": forced_pattern.astype(int).tolist(),
        "source_raw_pattern": pattern.astype(int).tolist(),
        "force_rggb_crop_sensor": [row_offset, column_offset],
        "color_description": description,
        "white_level": white_level,
        "black_level_per_channel": [
            float(value) for value in black_levels.tolist()
        ],
        "decoder_provider_id": decoder_provider_id,
        "decoder_provider_version": decoder_provider_version,
        "decoded_samples_sha256": decoded_samples_sha256,
    }
    return np.ascontiguousarray(packed), metadata


def centered_tile(packed: np.ndarray) -> tuple[np.ndarray, tuple[int, int]]:
    if packed.ndim != 3 or packed.shape[0] != 4:
        raise ValueError("packed Bayer source must have shape [4, H, W]")
    if packed.shape[1] < TILE_EDGE or packed.shape[2] < TILE_EDGE:
        raise ValueError("packed Bayer source is smaller than one model tile")
    row = (packed.shape[1] - TILE_EDGE) // 2
    column = (packed.shape[2] - TILE_EDGE) // 2
    tile = np.ascontiguousarray(
        packed[:, row : row + TILE_EDGE, column : column + TILE_EDGE][None]
    )
    return tile, (row, column)


def audit_raw_tile(
    model_path: Path,
    raw_path: Path,
    requested_provider: str,
) -> RawTileReceipt:
    packed, metadata = load_raw_as_packed_bayer(raw_path)
    tile, origin = centered_tile(packed)
    output, _ = run_tile(
        model_path,
        tile,
        requested_provider,
        warm_runs=0,
    )
    return RawTileReceipt(
        source=str(raw_path.resolve()),
        sensor_shape=metadata["sensor_shape"],
        packed_shape=metadata["packed_shape"],
        raw_pattern=metadata["raw_pattern"],
        source_raw_pattern=metadata["source_raw_pattern"],
        force_rggb_crop_sensor=metadata["force_rggb_crop_sensor"],
        color_description=metadata["color_description"],
        white_level=metadata["white_level"],
        black_level_per_channel=metadata["black_level_per_channel"],
        sample_origin_packed=[origin[0], origin[1]],
        sample_input_min=float(tile.min()),
        sample_input_max=float(tile.max()),
        sample_input_mean=float(np.mean(tile, dtype=np.float64)),
        matched_output_min=float(output.min()),
        matched_output_max=float(output.max()),
        matched_output_mean=float(np.mean(output, dtype=np.float64)),
        matched_output_negative_fraction=float(np.mean(output < 0.0)),
        matched_output_above_one_fraction=float(np.mean(output > 1.0)),
    )


def as_json(value: object) -> object:
    if hasattr(value, "__dataclass_fields__"):
        return asdict(value)
    raise TypeError(f"cannot serialize {type(value)!r}")
