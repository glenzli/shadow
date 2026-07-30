#!/usr/bin/env python3
"""Build and measure Shadow's synthetic neural RAW denoise smoke checkpoint.

This is deliberately a development baseline, not a product model. It trains only
on procedurally generated normalized Bayer planes and writes every generated
artifact outside the source repository.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import shutil
import statistics
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Protocol

import numpy as np
import torch
from torch import Tensor, nn
from torch.nn import functional as F


MODEL_SCHEMA = "shadow-neural-raw-synthetic-baseline-v1"
PREPROCESSING_CONTRACT = (
    "shadow-neural-raw-preprocess-v1:"
    "bayer-r-gr-gb-b-f32-nchw:"
    "noise-read-stddev4-shot-slope4"
)
TREE_IDENTITY_CONTRACT = b"shadow-coreml-model-tree-v1\0"


@dataclass(frozen=True)
class TrainingConfiguration:
    seed: int
    tile_edge: int
    halo: int
    steps: int
    batch_size: int
    features: int
    learning_rate: float


@dataclass(frozen=True)
class QualityMetrics:
    noisy_psnr_db: float
    denoised_psnr_db: float
    improvement_db: float
    noisy_edge_psnr_db: float
    denoised_edge_psnr_db: float
    noisy_flat_psnr_db: float
    denoised_flat_psnr_db: float
    noise_conditioning_rmse: float
    coreml_tiled_vs_torch_max_abs: float
    coreml_tiled_vs_torch_rmse: float


@dataclass(frozen=True)
class LatencyMetrics:
    compiled_model_load_ms: float
    first_prediction_ms: float
    warm_prediction_p50_ms: float
    warm_prediction_p95_ms: float
    warm_prediction_samples: int


class PredictionModel(Protocol):
    def predict(self, features: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
        """Return a feature dictionary containing denoised_mosaic."""


class SyntheticRawDenoiser(nn.Module):
    """Small residual CNN whose noise input is expanded into spatial feature maps."""

    def __init__(self, features: int = 24) -> None:
        super().__init__()
        self.features = features
        self.input_projection = nn.Conv2d(12, features, kernel_size=3, padding=1)
        self.body = nn.Sequential(
            nn.ReLU(inplace=False),
            nn.Conv2d(features, features, kernel_size=3, padding=1),
            nn.ReLU(inplace=False),
            nn.Conv2d(features, features, kernel_size=3, padding=1),
            nn.ReLU(inplace=False),
        )
        self.output_projection = nn.Conv2d(features, 4, kernel_size=3, padding=1)
        nn.init.zeros_(self.output_projection.weight)
        nn.init.zeros_(self.output_projection.bias)

    def forward(self, mosaic: Tensor, noise: Tensor) -> Tensor:
        height = mosaic.shape[2]
        width = mosaic.shape[3]
        noise_maps = noise.reshape(noise.shape[0], 8, 1, 1).expand(
            noise.shape[0], 8, height, width
        )
        return self.forward_with_noise_maps(mosaic, noise_maps)

    def forward_with_noise_maps(self, mosaic: Tensor, noise_maps: Tensor) -> Tensor:
        features = torch.cat((mosaic, noise_maps), dim=1)
        residual = self.output_projection(self.body(self.input_projection(features)))
        return torch.clamp(mosaic + residual, 0.0, 1.0)


class FixedTileExportModel(nn.Module):
    """Freeze Shadow's static Core ML tile contract without traced shape-to-int ops."""

    def __init__(self, model: SyntheticRawDenoiser, tile_edge: int) -> None:
        super().__init__()
        self.model = model
        self.tile_edge = tile_edge

    def forward(self, mosaic: Tensor, noise: Tensor) -> Tensor:
        noise_maps = noise.reshape(1, 8, 1, 1).repeat(
            1,
            1,
            self.tile_edge,
            self.tile_edge,
        )
        return self.model.forward_with_noise_maps(mosaic, noise_maps)


def _uniform(
    shape: tuple[int, ...],
    low: float,
    high: float,
    generator: torch.Generator,
) -> Tensor:
    return torch.rand(shape, generator=generator, dtype=torch.float32) * (high - low) + low


def _log_uniform(
    shape: tuple[int, ...],
    low: float,
    high: float,
    generator: torch.Generator,
) -> Tensor:
    log_low = math.log(low)
    log_high = math.log(high)
    return torch.exp(_uniform(shape, log_low, log_high, generator))


def generate_clean_mosaic(
    batch_size: int,
    edge: int,
    generator: torch.Generator,
) -> Tensor:
    """Generate correlated R/Gr/Gb/B planes with edges and fine texture."""

    coordinates = torch.linspace(-1.0, 1.0, edge, dtype=torch.float32)
    yy, xx = torch.meshgrid(coordinates, coordinates, indexing="ij")
    xx = xx.reshape(1, 1, edge, edge)
    yy = yy.reshape(1, 1, edge, edge)

    base = _uniform((batch_size, 1, 1, 1), 0.15, 0.55, generator)
    gradient_x = _uniform((batch_size, 1, 1, 1), -0.22, 0.22, generator)
    gradient_y = _uniform((batch_size, 1, 1, 1), -0.22, 0.22, generator)
    scene = base + gradient_x * xx + gradient_y * yy

    frequency_x = _uniform((batch_size, 1, 1, 1), 0.25, 3.5, generator)
    frequency_y = _uniform((batch_size, 1, 1, 1), 0.25, 3.5, generator)
    phase = _uniform((batch_size, 1, 1, 1), -math.pi, math.pi, generator)
    amplitude = _uniform((batch_size, 1, 1, 1), 0.03, 0.18, generator)
    scene = scene + amplitude * torch.sin(
        math.pi * (frequency_x * xx + frequency_y * yy) + phase
    )

    normal_x = _uniform((batch_size, 1, 1, 1), -1.0, 1.0, generator)
    normal_y = _uniform((batch_size, 1, 1, 1), -1.0, 1.0, generator)
    threshold = _uniform((batch_size, 1, 1, 1), -0.55, 0.55, generator)
    edge_contrast = _uniform((batch_size, 1, 1, 1), -0.34, 0.34, generator)
    hard_region = torch.sigmoid(
        70.0 * (normal_x * xx + normal_y * yy - threshold)
    )
    scene = scene + edge_contrast * hard_region

    center_x = _uniform((batch_size, 1, 1, 1), -0.65, 0.65, generator)
    center_y = _uniform((batch_size, 1, 1, 1), -0.65, 0.65, generator)
    radius = _uniform((batch_size, 1, 1, 1), 0.08, 0.42, generator)
    blob_amplitude = _uniform((batch_size, 1, 1, 1), -0.25, 0.25, generator)
    distance = ((xx - center_x) ** 2 + (yy - center_y) ** 2) / (2.0 * radius**2)
    scene = scene + blob_amplitude * torch.exp(-distance)

    channel_scale = _uniform((batch_size, 4, 1, 1), 0.72, 1.28, generator)
    channel_offset = _uniform((batch_size, 4, 1, 1), -0.05, 0.05, generator)
    fine_amplitude = _uniform((batch_size, 4, 1, 1), 0.0, 0.025, generator)
    fine_phase = _uniform((batch_size, 4, 1, 1), -math.pi, math.pi, generator)
    fine_texture = torch.sin(7.0 * math.pi * xx + 5.0 * math.pi * yy + fine_phase)
    return torch.clamp(
        scene * channel_scale + channel_offset + fine_amplitude * fine_texture,
        0.005,
        0.995,
    )


def add_poisson_gaussian_noise(
    clean: Tensor,
    generator: torch.Generator,
) -> tuple[Tensor, Tensor]:
    """Apply per-CFA shot and read noise in Shadow's normalized units."""

    batch_size = clean.shape[0]
    read = _log_uniform((batch_size, 4), 0.0015, 0.018, generator)
    shot = _log_uniform((batch_size, 4), 0.0004, 0.018, generator)
    read_maps = read.reshape(batch_size, 4, 1, 1)
    shot_maps = shot.reshape(batch_size, 4, 1, 1)
    poisson_rate = clean / shot_maps
    shot_sample = torch.poisson(poisson_rate, generator=generator) * shot_maps
    read_sample = torch.randn(
        clean.shape,
        generator=generator,
        dtype=torch.float32,
    ) * read_maps
    noisy = torch.clamp(shot_sample + read_sample, 0.0, 1.0)
    return noisy, torch.cat((read, shot), dim=1)


def generate_batch(
    batch_size: int,
    edge: int,
    generator: torch.Generator,
) -> tuple[Tensor, Tensor, Tensor]:
    clean = generate_clean_mosaic(batch_size, edge, generator)
    noisy, noise = add_poisson_gaussian_noise(clean, generator)
    return clean, noisy, noise


def _gradient_loss(output: Tensor, target: Tensor) -> Tensor:
    output_dx = output[:, :, :, 1:] - output[:, :, :, :-1]
    target_dx = target[:, :, :, 1:] - target[:, :, :, :-1]
    output_dy = output[:, :, 1:, :] - output[:, :, :-1, :]
    target_dy = target[:, :, 1:, :] - target[:, :, :-1, :]
    return F.l1_loss(output_dx, target_dx) + F.l1_loss(output_dy, target_dy)


def train_model(configuration: TrainingConfiguration) -> tuple[SyntheticRawDenoiser, list[float]]:
    if configuration.halo < 4:
        raise ValueError("halo must cover the CNN's three-pixel receptive radius")
    torch.manual_seed(configuration.seed)
    torch.set_num_threads(1)
    torch.use_deterministic_algorithms(True)
    generator = torch.Generator(device="cpu")
    generator.manual_seed(configuration.seed)

    model = SyntheticRawDenoiser(configuration.features)
    model.train()
    optimizer = torch.optim.Adam(
        model.parameters(),
        lr=configuration.learning_rate,
    )
    losses: list[float] = []
    for step in range(configuration.steps):
        clean, noisy, noise = generate_batch(
            configuration.batch_size,
            configuration.tile_edge,
            generator,
        )
        optimizer.zero_grad(set_to_none=True)
        output = model(noisy, noise)
        loss = F.mse_loss(output, clean) + 0.08 * _gradient_loss(output, clean)
        loss.backward()
        optimizer.step()
        losses.append(float(loss.detach()))
        if step == 0 or (step + 1) % 100 == 0 or step + 1 == configuration.steps:
            print(
                f"train step {step + 1:4d}/{configuration.steps}: "
                f"loss={losses[-1]:.7f}",
                flush=True,
            )
    model.eval()
    return model, losses


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return f"sha256:{digest.hexdigest()}"


def state_dict_identity(model: nn.Module) -> str:
    """Hash learned tensors independently of Torch's archive/container metadata."""

    digest = hashlib.sha256()
    digest.update(b"shadow-neural-raw-state-v1\0")
    for name, tensor in sorted(model.state_dict().items()):
        value = tensor.detach().cpu().contiguous().numpy()
        encoded_name = name.encode("utf-8")
        encoded_dtype = value.dtype.str.encode("ascii")
        digest.update(len(encoded_name).to_bytes(8, "big"))
        digest.update(encoded_name)
        digest.update(len(encoded_dtype).to_bytes(8, "big"))
        digest.update(encoded_dtype)
        digest.update(value.ndim.to_bytes(8, "big"))
        for dimension in value.shape:
            digest.update(int(dimension).to_bytes(8, "big"))
        raw = value.tobytes(order="C")
        digest.update(len(raw).to_bytes(8, "big"))
        digest.update(raw)
    return f"sha256-state-v1:{digest.hexdigest()}"


def compiled_model_tree_identity(root: Path) -> str:
    root = root.resolve()
    if root.suffix != ".mlmodelc" or not root.is_dir() or root.is_symlink():
        raise ValueError("compiled model must be a non-symlink .mlmodelc directory")
    files: list[tuple[str, Path]] = []
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError("compiled model tree may not contain symbolic links")
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError("compiled model tree contains a non-regular entry")
        relative = path.relative_to(root).as_posix()
        files.append((relative, path))
    if not files:
        raise ValueError("compiled model tree is empty")

    digest = hashlib.sha256()
    digest.update(TREE_IDENTITY_CONTRACT)
    for relative, path in sorted(files):
        encoded = relative.encode("utf-8")
        size = path.stat().st_size
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
        digest.update(size.to_bytes(8, "big"))
        with path.open("rb") as source:
            while chunk := source.read(64 * 1024):
                digest.update(chunk)
    return f"sha256-tree-v1:{digest.hexdigest()}"


def export_and_compile_coreml(
    model: SyntheticRawDenoiser,
    configuration: TrainingConfiguration,
    output_directory: Path,
) -> tuple[Path, Path]:
    coreml_work_directory = output_directory / "coreml-work"
    coreml_work_directory.mkdir()
    # Core ML asks Foundation for a compilation workspace when it is first imported.
    # Bind that workspace to the already validated external payload directory so the
    # result does not depend on a sandbox-specific /var/folders alias.
    os.environ["TMPDIR"] = str(coreml_work_directory)
    import coremltools as ct

    example_mosaic = torch.zeros(
        (1, 4, configuration.tile_edge, configuration.tile_edge),
        dtype=torch.float32,
    )
    example_noise = torch.zeros((1, 8), dtype=torch.float32)
    export_model = FixedTileExportModel(model, configuration.tile_edge).eval()
    traced = torch.jit.trace(
        export_model,
        (example_mosaic, example_noise),
        strict=True,
    )
    converted = ct.convert(
        traced,
        convert_to="mlprogram",
        inputs=[
            ct.TensorType(
                name="mosaic",
                shape=example_mosaic.shape,
                dtype=np.float32,
            ),
            ct.TensorType(
                name="noise",
                shape=example_noise.shape,
                dtype=np.float32,
            ),
        ],
        outputs=[
            ct.TensorType(name="denoised_mosaic", dtype=np.float32),
        ],
        minimum_deployment_target=ct.target.macOS13,
        compute_precision=ct.precision.FLOAT32,
    )
    converted.author = "Shadow development baseline"
    converted.short_description = (
        "Synthetic-only noise-conditioned Bayer denoise smoke checkpoint"
    )
    converted.version = MODEL_SCHEMA
    converted.user_defined_metadata["shadow.model.schema"] = MODEL_SCHEMA
    converted.user_defined_metadata[
        "shadow.preprocessing.contract"
    ] = PREPROCESSING_CONTRACT
    converted.user_defined_metadata["shadow.product.status"] = (
        "synthetic-smoke-only-not-product"
    )

    package_path = output_directory / "synthetic-neural-raw.mlpackage"
    converted.save(str(package_path))
    temporary_compiled = Path(converted.get_compiled_model_path())
    compiled_path = output_directory / "synthetic-neural-raw.mlmodelc"
    shutil.copytree(temporary_compiled, compiled_path, symlinks=False)
    compiled_model_tree_identity(compiled_path)
    return package_path, compiled_path


def tiled_predict(
    model: PredictionModel,
    mosaic: np.ndarray,
    noise: np.ndarray,
    tile_edge: int,
    halo: int,
) -> tuple[np.ndarray, list[float]]:
    """Run the same clamped tile/core assembly contract as shadow-image."""

    if mosaic.ndim != 4 or mosaic.shape[0] != 1 or mosaic.shape[1] != 4:
        raise ValueError("mosaic must have shape [1,4,H,W]")
    if noise.shape != (1, 8):
        raise ValueError("noise must have shape [1,8]")
    if tile_edge < 4 or halo < 0 or halo >= tile_edge // 2:
        raise ValueError("invalid tile/halo contract")
    core_edge = tile_edge - 2 * halo
    height = mosaic.shape[2]
    width = mosaic.shape[3]
    result = np.empty_like(mosaic)
    prediction_ms: list[float] = []
    for core_y in range(0, height, core_edge):
        y_indices = np.clip(
            np.arange(tile_edge, dtype=np.int64) + core_y - halo,
            0,
            height - 1,
        )
        for core_x in range(0, width, core_edge):
            x_indices = np.clip(
                np.arange(tile_edge, dtype=np.int64) + core_x - halo,
                0,
                width - 1,
            )
            tile = mosaic[:, :, y_indices][:, :, :, x_indices]
            started = time.perf_counter()
            prediction = model.predict(
                {
                    "mosaic": np.ascontiguousarray(tile, dtype=np.float32),
                    "noise": np.ascontiguousarray(noise, dtype=np.float32),
                }
            )
            prediction_ms.append((time.perf_counter() - started) * 1000.0)
            output = np.asarray(prediction["denoised_mosaic"], dtype=np.float32)
            if output.shape != tile.shape or not np.isfinite(output).all():
                raise RuntimeError("Core ML returned an invalid denoised_mosaic tensor")
            copy_height = min(core_edge, height - core_y)
            copy_width = min(core_edge, width - core_x)
            result[
                :,
                :,
                core_y : core_y + copy_height,
                core_x : core_x + copy_width,
            ] = output[
                :,
                :,
                halo : halo + copy_height,
                halo : halo + copy_width,
            ]
    return result, prediction_ms


def _psnr(mse: float) -> float:
    return -10.0 * math.log10(max(mse, 1.0e-12))


def _masked_mse(prediction: np.ndarray, target: np.ndarray, mask: np.ndarray) -> float:
    expanded = np.broadcast_to(mask, prediction.shape)
    selected = (prediction - target)[expanded]
    if selected.size == 0:
        raise RuntimeError("synthetic benchmark produced an empty metric region")
    return float(np.mean(selected.astype(np.float64) ** 2))


def _edge_and_flat_masks(clean: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    gradient = np.zeros_like(clean)
    gradient[:, :, :, 1:] = np.maximum(
        gradient[:, :, :, 1:],
        np.abs(clean[:, :, :, 1:] - clean[:, :, :, :-1]),
    )
    gradient[:, :, 1:, :] = np.maximum(
        gradient[:, :, 1:, :],
        np.abs(clean[:, :, 1:, :] - clean[:, :, :-1, :]),
    )
    aggregate = np.max(gradient, axis=1, keepdims=True)
    return aggregate >= 0.025, aggregate <= 0.006


def benchmark_coreml(
    torch_model: SyntheticRawDenoiser,
    compiled_path: Path,
    configuration: TrainingConfiguration,
    held_out_samples: int,
    warm_predictions: int,
) -> tuple[QualityMetrics, LatencyMetrics]:
    import coremltools as ct

    load_started = time.perf_counter()
    compiled = ct.models.CompiledMLModel(
        str(compiled_path),
        compute_units=ct.ComputeUnit.ALL,
    )
    load_ms = (time.perf_counter() - load_started) * 1000.0

    generator = torch.Generator(device="cpu")
    generator.manual_seed(configuration.seed + 10_000)
    representative_clean, representative_noisy, representative_noise = generate_batch(
        1,
        configuration.tile_edge,
        generator,
    )
    del representative_clean
    feature_values = {
        "mosaic": representative_noisy.numpy(),
        "noise": representative_noise.numpy(),
    }
    first_started = time.perf_counter()
    compiled.predict(feature_values)
    first_prediction_ms = (time.perf_counter() - first_started) * 1000.0
    warm_ms: list[float] = []
    for _ in range(warm_predictions):
        started = time.perf_counter()
        compiled.predict(feature_values)
        warm_ms.append((time.perf_counter() - started) * 1000.0)
    low_noise = np.array(
        [[0.0015, 0.0015, 0.0015, 0.0015, 0.0004, 0.0004, 0.0004, 0.0004]],
        dtype=np.float32,
    )
    high_noise = np.array(
        [[0.018, 0.018, 0.018, 0.018, 0.018, 0.018, 0.018, 0.018]],
        dtype=np.float32,
    )
    low_conditioning = np.asarray(
        compiled.predict(
            {
                "mosaic": feature_values["mosaic"],
                "noise": low_noise,
            }
        )["denoised_mosaic"],
        dtype=np.float32,
    )
    high_conditioning = np.asarray(
        compiled.predict(
            {
                "mosaic": feature_values["mosaic"],
                "noise": high_noise,
            }
        )["denoised_mosaic"],
        dtype=np.float32,
    )
    conditioning_difference = (
        high_conditioning.astype(np.float64) - low_conditioning.astype(np.float64)
    )
    noise_conditioning_rmse = float(
        math.sqrt(np.mean(conditioning_difference**2))
    )

    extent = configuration.tile_edge + configuration.tile_edge // 2
    noisy_errors: list[np.ndarray] = []
    denoised_errors: list[np.ndarray] = []
    noisy_edge_mse: list[float] = []
    denoised_edge_mse: list[float] = []
    noisy_flat_mse: list[float] = []
    denoised_flat_mse: list[float] = []
    parity_errors: list[np.ndarray] = []

    with torch.no_grad():
        for _ in range(held_out_samples):
            clean_t, noisy_t, noise_t = generate_batch(1, extent, generator)
            clean = clean_t.numpy()
            noisy = noisy_t.numpy()
            noise = noise_t.numpy()
            denoised, _ = tiled_predict(
                compiled,
                noisy,
                noise,
                configuration.tile_edge,
                configuration.halo,
            )
            padded = F.pad(
                noisy_t,
                (
                    configuration.halo,
                    configuration.halo,
                    configuration.halo,
                    configuration.halo,
                ),
                mode="replicate",
            )
            torch_reference = torch_model(padded, noise_t)[
                :,
                :,
                configuration.halo : configuration.halo + extent,
                configuration.halo : configuration.halo + extent,
            ].numpy()
            parity_errors.append(denoised - torch_reference)
            noisy_errors.append(noisy - clean)
            denoised_errors.append(denoised - clean)
            edge_mask, flat_mask = _edge_and_flat_masks(clean)
            noisy_edge_mse.append(_masked_mse(noisy, clean, edge_mask))
            denoised_edge_mse.append(_masked_mse(denoised, clean, edge_mask))
            noisy_flat_mse.append(_masked_mse(noisy, clean, flat_mask))
            denoised_flat_mse.append(_masked_mse(denoised, clean, flat_mask))

    noisy_error = np.concatenate([value.reshape(-1) for value in noisy_errors])
    denoised_error = np.concatenate([value.reshape(-1) for value in denoised_errors])
    parity_error = np.concatenate([value.reshape(-1) for value in parity_errors])
    noisy_mse = float(np.mean(noisy_error.astype(np.float64) ** 2))
    denoised_mse = float(np.mean(denoised_error.astype(np.float64) ** 2))
    quality = QualityMetrics(
        noisy_psnr_db=_psnr(noisy_mse),
        denoised_psnr_db=_psnr(denoised_mse),
        improvement_db=_psnr(denoised_mse) - _psnr(noisy_mse),
        noisy_edge_psnr_db=_psnr(statistics.fmean(noisy_edge_mse)),
        denoised_edge_psnr_db=_psnr(statistics.fmean(denoised_edge_mse)),
        noisy_flat_psnr_db=_psnr(statistics.fmean(noisy_flat_mse)),
        denoised_flat_psnr_db=_psnr(statistics.fmean(denoised_flat_mse)),
        noise_conditioning_rmse=noise_conditioning_rmse,
        coreml_tiled_vs_torch_max_abs=float(np.max(np.abs(parity_error))),
        coreml_tiled_vs_torch_rmse=float(
            math.sqrt(np.mean(parity_error.astype(np.float64) ** 2))
        ),
    )

    sorted_warm = sorted(warm_ms)
    p95_index = min(len(sorted_warm) - 1, math.ceil(0.95 * len(sorted_warm)) - 1)
    latency = LatencyMetrics(
        compiled_model_load_ms=load_ms,
        first_prediction_ms=first_prediction_ms,
        warm_prediction_p50_ms=statistics.median(warm_ms),
        warm_prediction_p95_ms=sorted_warm[p95_index],
        warm_prediction_samples=len(warm_ms),
    )
    return quality, latency


def quality_gate_failures(quality: QualityMetrics) -> list[str]:
    failures: list[str] = []
    if quality.improvement_db < 1.0:
        failures.append("held-out synthetic PSNR improvement is below 1.0 dB")
    if quality.denoised_edge_psnr_db + 0.10 < quality.noisy_edge_psnr_db:
        failures.append("edge-region PSNR regressed by more than 0.10 dB")
    if quality.denoised_flat_psnr_db - quality.noisy_flat_psnr_db < 1.0:
        failures.append("flat-region PSNR improvement is below 1.0 dB")
    if quality.noise_conditioning_rmse < 1.0e-5:
        failures.append("Core ML output is not measurably conditioned by the noise tensor")
    if quality.coreml_tiled_vs_torch_max_abs > 5.0e-4:
        failures.append("Core ML tiled output differs from the Torch reference")
    return failures


def _path_is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def validate_output_directory(path: Path) -> Path:
    if not path.is_absolute():
        raise ValueError("output directory must be absolute")
    resolved = path.resolve()
    repository_root = Path(__file__).resolve().parents[2]
    if _path_is_within(resolved, repository_root):
        raise ValueError("model and benchmark payloads may not be written inside the repository")
    if resolved.exists() and any(resolved.iterdir()):
        raise ValueError("output directory must not already contain files")
    resolved.mkdir(parents=True, exist_ok=True)
    return resolved


def _write_json(path: Path, value: object) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def run(arguments: argparse.Namespace) -> int:
    output_directory = validate_output_directory(arguments.output_dir)
    configuration = TrainingConfiguration(
        seed=arguments.seed,
        tile_edge=arguments.tile_edge,
        halo=arguments.halo,
        steps=arguments.steps,
        batch_size=arguments.batch_size,
        features=arguments.features,
        learning_rate=arguments.learning_rate,
    )
    if configuration.tile_edge < 16 or configuration.tile_edge > 1024:
        raise ValueError("tile edge must be between 16 and 1024")
    if configuration.halo < 4 or configuration.halo >= configuration.tile_edge // 2:
        raise ValueError("halo must be at least 4 and smaller than half the tile edge")
    if configuration.steps < 1 or configuration.batch_size < 1:
        raise ValueError("training steps and batch size must be positive")

    started = time.perf_counter()
    model, losses = train_model(configuration)
    checkpoint_path = output_directory / "synthetic-neural-raw-state.pt"
    torch.save(
        {
            "schema": MODEL_SCHEMA,
            "preprocessing_contract": PREPROCESSING_CONTRACT,
            "training": asdict(configuration),
            "state_dict": model.state_dict(),
        },
        checkpoint_path,
    )
    package_path, compiled_path = export_and_compile_coreml(
        model,
        configuration,
        output_directory,
    )
    identity = compiled_model_tree_identity(compiled_path)
    quality, latency = benchmark_coreml(
        model,
        compiled_path,
        configuration,
        arguments.held_out_samples,
        arguments.warm_predictions,
    )
    gate_failures = quality_gate_failures(quality)
    report = {
        "schema": MODEL_SCHEMA,
        "status": (
            "synthetic_smoke_gate_passed"
            if not gate_failures
            else "synthetic_smoke_gate_failed"
        ),
        "product_eligible": False,
        "limitations": [
            "trained only on procedurally generated normalized Bayer planes",
            "not evaluated on any real camera, RAW corpus, or held-out camera",
            "not a replacement for Shadow's deterministic RAW denoise",
            "must remain side-loaded and disabled in product/UI",
        ],
        "preprocessing_contract": PREPROCESSING_CONTRACT,
        "training": asdict(configuration),
        "training_loss": {
            "first": losses[0],
            "last": losses[-1],
            "minimum": min(losses),
        },
        "quality": asdict(quality),
        "latency": asdict(latency),
        "gate_failures": gate_failures,
        "artifacts": {
            "torch_checkpoint": str(checkpoint_path),
            "torch_checkpoint_identity": sha256_file(checkpoint_path),
            "learned_state_identity": state_dict_identity(model),
            "coreml_package": str(package_path),
            "compiled_coreml_model": str(compiled_path),
            "compiled_coreml_tree_identity": identity,
        },
        "environment": {
            "python": sys.version.split()[0],
            "platform": platform.platform(),
            "machine": platform.machine(),
            "torch": torch.__version__,
            "numpy": np.__version__,
            "coremltools": __import__("coremltools").__version__,
        },
        "wall_time_seconds": time.perf_counter() - started,
    }
    report_path = output_directory / "report.json"
    _write_json(report_path, report)
    print(json.dumps(report, indent=2, sort_keys=True), flush=True)
    print("\nNative gate environment:", flush=True)
    print(
        f"SHADOW_TEST_NEURAL_RAW_DENOISE_MODEL={compiled_path}",
        flush=True,
    )
    print(
        f"SHADOW_TEST_NEURAL_RAW_DENOISE_MODEL_IDENTITY={identity}",
        flush=True,
    )
    print(
        f"SHADOW_TEST_NEURAL_RAW_DENOISE_TILE_EDGE={configuration.tile_edge}",
        flush=True,
    )
    print(
        f"SHADOW_TEST_NEURAL_RAW_DENOISE_HALO={configuration.halo}",
        flush=True,
    )
    return 0 if not gate_failures else 2


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="absolute, empty directory outside the repository",
    )
    parser.add_argument("--seed", type=int, default=20260730)
    parser.add_argument("--tile-edge", type=int, default=64)
    parser.add_argument("--halo", type=int, default=8)
    parser.add_argument("--steps", type=int, default=500)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--features", type=int, default=24)
    parser.add_argument("--learning-rate", type=float, default=1.0e-3)
    parser.add_argument("--held-out-samples", type=int, default=4)
    parser.add_argument("--warm-predictions", type=int, default=20)
    return parser.parse_args()


if __name__ == "__main__":
    try:
        raise SystemExit(run(parse_arguments()))
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
