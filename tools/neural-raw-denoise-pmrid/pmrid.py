#!/usr/bin/env python3
"""Audit and convert the pinned public PMRID RAW denoise checkpoint.

All upstream and generated payloads remain outside the Shadow worktree. This
tool proves public-checkpoint intake and fixed-tile Core ML parity; it does not
admit the model for product use.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
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
from types import ModuleType
from typing import Protocol, cast

import numpy as np
import torch
from torch import Tensor, nn


UPSTREAM_REPOSITORY = "https://github.com/MegEngine/PMRID"
UPSTREAM_REVISION = "8ebb9e8e96559881dee957f34243933c5beb77dd"
UPSTREAM_LICENSE = "Apache-2.0"
PINNED_FILES = {
    "LICENSE": "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4",
    "README.md": "633d9bc52e32b96eeeb0f9fec1f28ef3c6c41cc5711a570f9699ea85f8a035e2",
    "models/net_torch.py": (
        "56290ec2bc430c58c16345bcac4ed14381b8f059fc1fe66428438bb0bb328970"
    ),
    "models/torch_pretrained.ckp": (
        "9361614f3514d27351d81909f2215c0fdc38619c0288d936b7266485ac106c14"
    ),
}
MODEL_SCHEMA = "shadow-pmrid-public-checkpoint-adapter-v1"
PREPROCESSING_CONTRACT = (
    "shadow-neural-raw-preprocess-v1:"
    "bayer-r-gr-gb-b-f32-nchw:"
    "noise-read-stddev4-shot-slope4"
)
TREE_IDENTITY_CONTRACT = b"shadow-coreml-model-tree-v1\0"

# PMRID's published Reno 10x noise model and ISO-1600 anchor.
PMRID_K_COEFFICIENTS = (0.0005995267, 0.00868861)
PMRID_B_COEFFICIENTS = (7.11772e-7, 6.514934e-4, 0.11492713)
PMRID_ANCHOR_ISO = 1600.0
PMRID_SENSOR_SCALE = 959.0
PMRID_INPUT_SCALE = 256.0

# The longest dependency path through the pinned encoder/decoder is 965 packed
# pixels. A core sample therefore needs 482 packed pixels on each side for
# strict independence from an artificial tile boundary.
PMRID_RECEPTIVE_FIELD = 965
PMRID_EXACT_HALO = (PMRID_RECEPTIVE_FIELD - 1) // 2


@dataclass(frozen=True)
class UpstreamReceipt:
    repository: str
    revision: str
    license: str
    files: dict[str, str]


@dataclass(frozen=True)
class ConversionMetrics:
    pmrid_iso_vs_noise_parameter_max_abs: float
    torch_adapter_vs_pmrid_reference_max_abs: float
    coreml_vs_torch_max_abs: float
    coreml_vs_torch_rmse: float
    noise_conditioning_rmse: float


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


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def verify_upstream(upstream_directory: Path) -> UpstreamReceipt:
    resolved = upstream_directory.resolve()
    if not resolved.is_dir():
        raise ValueError("PMRID upstream directory does not exist")
    observed: dict[str, str] = {}
    for relative_path, expected in PINNED_FILES.items():
        path = resolved / relative_path
        if not path.is_file():
            raise ValueError(f"pinned PMRID file is missing: {relative_path}")
        identity = sha256_file(path)
        if identity != expected:
            raise ValueError(
                f"pinned PMRID file identity mismatch: {relative_path}; "
                f"expected {expected}, observed {identity}"
            )
        observed[relative_path] = identity
    return UpstreamReceipt(
        repository=UPSTREAM_REPOSITORY,
        revision=UPSTREAM_REVISION,
        license=UPSTREAM_LICENSE,
        files=observed,
    )


def _load_verified_network_module(upstream_directory: Path) -> ModuleType:
    source_path = upstream_directory.resolve() / "models/net_torch.py"
    spec = importlib.util.spec_from_file_location(
        "shadow_verified_pmrid_net_torch",
        source_path,
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("could not create the verified PMRID network module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_verified_network(upstream_directory: Path) -> nn.Module:
    verify_upstream(upstream_directory)
    module = _load_verified_network_module(upstream_directory)
    network_type = getattr(module, "Network", None)
    if network_type is None:
        raise RuntimeError("verified PMRID module does not expose Network")
    network = cast(nn.Module, network_type())
    checkpoint_path = upstream_directory.resolve() / "models/torch_pretrained.ckp"
    state = torch.load(
        checkpoint_path,
        map_location="cpu",
        weights_only=True,
    )
    if not isinstance(state, dict) or not state:
        raise RuntimeError("verified PMRID checkpoint is not a non-empty state dictionary")
    network.load_state_dict(state, strict=True)
    network.eval()
    return network


def _pmrid_k(iso: float) -> float:
    slope, intercept = PMRID_K_COEFFICIENTS
    return slope * iso + intercept


def _pmrid_b(iso: float) -> float:
    quadratic, linear, intercept = PMRID_B_COEFFICIENTS
    return quadratic * iso * iso + linear * iso + intercept


def pmrid_normalized_noise(iso: float) -> tuple[float, float]:
    if not math.isfinite(iso) or iso <= 0.0:
        raise ValueError("PMRID ISO must be finite and positive")
    shot = _pmrid_k(iso) / PMRID_SENSOR_SCALE
    read_variance = _pmrid_b(iso) / (PMRID_SENSOR_SCALE**2)
    if shot <= 0.0 or read_variance < 0.0:
        raise ValueError("PMRID noise polynomial is invalid at the requested ISO")
    return math.sqrt(read_variance), shot


def noise_transform_parameters(
    read_stddev: np.ndarray,
    shot: np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    read = np.asarray(read_stddev, dtype=np.float64)
    source_shot = np.asarray(shot, dtype=np.float64)
    if (
        read.shape != source_shot.shape
        or not np.isfinite(read).all()
        or not np.isfinite(source_shot).all()
        or np.any(read < 0.0)
        or np.any(source_shot <= 0.0)
    ):
        raise ValueError("noise arrays must be finite, same-shaped, and physically valid")
    anchor_read, anchor_shot = pmrid_normalized_noise(PMRID_ANCHOR_ISO)
    scale = anchor_shot / source_shot
    offset = (
        scale * scale * read * read - anchor_read * anchor_read
    ) / anchor_shot
    return scale, offset


def pmrid_iso_transform_parameters(iso: float) -> tuple[float, float]:
    k = _pmrid_k(iso)
    variance = _pmrid_b(iso)
    anchor_k = _pmrid_k(PMRID_ANCHOR_ISO)
    anchor_variance = _pmrid_b(PMRID_ANCHOR_ISO)
    scale = anchor_k / k
    offset = (
        variance / (k * k)
        - anchor_variance / (anchor_k * anchor_k)
    ) * anchor_k / PMRID_SENSOR_SCALE
    return scale, offset


class NoiseConditionedPmrid(nn.Module):
    """Wrap the pinned PMRID model with Shadow's calibrated noise contract."""

    def __init__(self, network: nn.Module) -> None:
        super().__init__()
        self.network = network
        anchor_read, anchor_shot = pmrid_normalized_noise(PMRID_ANCHOR_ISO)
        self.register_buffer(
            "anchor_read_variance",
            torch.full((1, 4, 1, 1), anchor_read * anchor_read),
        )
        self.register_buffer(
            "anchor_shot",
            torch.full((1, 4, 1, 1), anchor_shot),
        )

    def forward(self, mosaic: Tensor, noise: Tensor) -> Tensor:
        # Shadow's Core ML tile ABI is deliberately fixed to batch one. Keeping
        # this shape static avoids exporting a traced tensor-to-Python `int`
        # operation that Core ML cannot represent.
        read = noise[:, :4].reshape(1, 4, 1, 1)
        shot = noise[:, 4:].reshape(1, 4, 1, 1)
        scale = self.anchor_shot / torch.clamp(shot, min=1.0e-8)
        offset = (
            scale * scale * read * read - self.anchor_read_variance
        ) / self.anchor_shot
        normalized = torch.clamp(mosaic, 0.0, 1.0) * scale + offset
        prediction = self.network(normalized * PMRID_INPUT_SCALE) / PMRID_INPUT_SCALE
        return (prediction - offset) / scale


def pmrid_reference_prediction(
    network: nn.Module,
    mosaic: Tensor,
    iso: float,
) -> Tensor:
    scale, offset = pmrid_iso_transform_parameters(iso)
    normalized = torch.clamp(mosaic, 0.0, 1.0) * scale + offset
    prediction = network(normalized * PMRID_INPUT_SCALE) / PMRID_INPUT_SCALE
    return (prediction - offset) / scale


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
        raise ValueError("model and report payloads may not be written inside the repository")
    if resolved.exists() and any(resolved.iterdir()):
        raise ValueError("output directory must not already contain files")
    resolved.mkdir(parents=True, exist_ok=True)
    return resolved


def compiled_model_tree_identity(model_directory: Path) -> str:
    if not model_directory.is_dir():
        raise ValueError("compiled Core ML model must be a directory")
    files = sorted(
        path
        for path in model_directory.rglob("*")
        if path.is_file() and not path.is_symlink()
    )
    if not files:
        raise ValueError("compiled Core ML model directory is empty")
    digest = hashlib.sha256()
    digest.update(TREE_IDENTITY_CONTRACT)
    for path in files:
        relative = path.relative_to(model_directory).as_posix().encode("utf-8")
        content = path.read_bytes()
        digest.update(len(relative).to_bytes(8, "little"))
        digest.update(relative)
        digest.update(len(content).to_bytes(8, "little"))
        digest.update(content)
    return f"sha256-tree-v1:{digest.hexdigest()}"


def _write_json(path: Path, value: object) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def export_and_compile_coreml(
    adapter: NoiseConditionedPmrid,
    tile_edge: int,
    output_directory: Path,
) -> tuple[Path, Path]:
    if tile_edge < 16 or tile_edge % 16 != 0:
        raise ValueError("PMRID tile edge must be a positive multiple of 16")
    coreml_work_directory = output_directory / "coreml-work"
    coreml_work_directory.mkdir()
    previous_tmpdir = os.environ.get("TMPDIR")
    os.environ["TMPDIR"] = str(coreml_work_directory)
    try:
        import coremltools as ct

        example_mosaic = torch.zeros(
            (1, 4, tile_edge, tile_edge),
            dtype=torch.float32,
        )
        anchor_read, anchor_shot = pmrid_normalized_noise(PMRID_ANCHOR_ISO)
        example_noise = torch.tensor(
            [[anchor_read] * 4 + [anchor_shot] * 4],
            dtype=torch.float32,
        )
        traced = torch.jit.trace(
            adapter,
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
        converted.author = "Shadow PMRID public-checkpoint compatibility experiment"
        converted.short_description = (
            "Pinned Apache-2.0 PMRID weights with calibrated Shadow noise conditioning"
        )
        converted.version = MODEL_SCHEMA
        converted.user_defined_metadata["shadow.model.schema"] = MODEL_SCHEMA
        converted.user_defined_metadata[
            "shadow.preprocessing.contract"
        ] = PREPROCESSING_CONTRACT
        converted.user_defined_metadata["shadow.product.status"] = (
            "public-research-checkpoint-not-admitted"
        )
        converted.user_defined_metadata["shadow.upstream.repository"] = (
            UPSTREAM_REPOSITORY
        )
        converted.user_defined_metadata["shadow.upstream.revision"] = UPSTREAM_REVISION
        converted.user_defined_metadata["shadow.upstream.license"] = UPSTREAM_LICENSE
        package_path = output_directory / "pmrid-shadow-adapter.mlpackage"
        converted.save(str(package_path))
        temporary_compiled = Path(converted.get_compiled_model_path())
        compiled_path = output_directory / "pmrid-shadow-adapter.mlmodelc"
        shutil.copytree(temporary_compiled, compiled_path, symlinks=False)
        compiled_model_tree_identity(compiled_path)
        return package_path, compiled_path
    finally:
        if previous_tmpdir is None:
            os.environ.pop("TMPDIR", None)
        else:
            os.environ["TMPDIR"] = previous_tmpdir


def _representative_mosaic(tile_edge: int) -> Tensor:
    generator = torch.Generator(device="cpu")
    generator.manual_seed(20260730)
    return torch.rand(
        (1, 4, tile_edge, tile_edge),
        generator=generator,
        dtype=torch.float32,
    )


def verify_pmrid_equivalence(
    network: nn.Module,
    adapter: NoiseConditionedPmrid,
    tile_edge: int,
) -> tuple[float, float]:
    mosaic = _representative_mosaic(tile_edge)
    parameter_errors: list[float] = []
    prediction_errors: list[float] = []
    with torch.no_grad():
        for iso in (100.0, 800.0, 1600.0, 6400.0):
            read, shot = pmrid_normalized_noise(iso)
            noise_scale, noise_offset = noise_transform_parameters(
                np.full(4, read),
                np.full(4, shot),
            )
            iso_scale, iso_offset = pmrid_iso_transform_parameters(iso)
            parameter_errors.extend(
                [
                    float(np.max(np.abs(noise_scale - iso_scale))),
                    float(np.max(np.abs(noise_offset - iso_offset))),
                ]
            )
            noise = torch.tensor(
                [[read] * 4 + [shot] * 4],
                dtype=torch.float32,
            )
            expected = pmrid_reference_prediction(network, mosaic, iso)
            actual = adapter(mosaic, noise)
            prediction_errors.append(
                float(torch.max(torch.abs(expected - actual)).item())
            )
    return max(parameter_errors), max(prediction_errors)


def benchmark_coreml(
    adapter: NoiseConditionedPmrid,
    compiled_path: Path,
    tile_edge: int,
    warm_predictions: int,
) -> tuple[ConversionMetrics, LatencyMetrics]:
    import coremltools as ct

    if warm_predictions < 1:
        raise ValueError("warm prediction count must be positive")
    load_started = time.perf_counter()
    compiled = ct.models.CompiledMLModel(
        str(compiled_path),
        compute_units=ct.ComputeUnit.ALL,
    )
    load_ms = (time.perf_counter() - load_started) * 1000.0

    mosaic = _representative_mosaic(tile_edge)
    anchor_read, anchor_shot = pmrid_normalized_noise(PMRID_ANCHOR_ISO)
    anchor_noise = np.array(
        [[anchor_read] * 4 + [anchor_shot] * 4],
        dtype=np.float32,
    )
    features = {
        "mosaic": np.ascontiguousarray(mosaic.numpy()),
        "noise": anchor_noise,
    }
    first_started = time.perf_counter()
    first_output = np.asarray(
        compiled.predict(features)["denoised_mosaic"],
        dtype=np.float32,
    )
    first_ms = (time.perf_counter() - first_started) * 1000.0
    warm_ms: list[float] = []
    for _ in range(warm_predictions):
        started = time.perf_counter()
        compiled.predict(features)
        warm_ms.append((time.perf_counter() - started) * 1000.0)

    with torch.no_grad():
        torch_output = adapter(
            mosaic,
            torch.from_numpy(anchor_noise),
        ).numpy()
    parity = first_output.astype(np.float64) - torch_output.astype(np.float64)

    low_read, low_shot = pmrid_normalized_noise(100.0)
    high_read, high_shot = pmrid_normalized_noise(6400.0)
    low_output = np.asarray(
        compiled.predict(
            {
                "mosaic": features["mosaic"],
                "noise": np.array(
                    [[low_read] * 4 + [low_shot] * 4],
                    dtype=np.float32,
                ),
            }
        )["denoised_mosaic"],
        dtype=np.float32,
    )
    high_output = np.asarray(
        compiled.predict(
            {
                "mosaic": features["mosaic"],
                "noise": np.array(
                    [[high_read] * 4 + [high_shot] * 4],
                    dtype=np.float32,
                ),
            }
        )["denoised_mosaic"],
        dtype=np.float32,
    )
    conditioning_difference = (
        high_output.astype(np.float64) - low_output.astype(np.float64)
    )

    parameter_error, adapter_error = verify_pmrid_equivalence(
        adapter.network,
        adapter,
        tile_edge,
    )
    sorted_warm = sorted(warm_ms)
    p95_index = min(
        len(sorted_warm) - 1,
        math.ceil(0.95 * len(sorted_warm)) - 1,
    )
    metrics = ConversionMetrics(
        pmrid_iso_vs_noise_parameter_max_abs=parameter_error,
        torch_adapter_vs_pmrid_reference_max_abs=adapter_error,
        coreml_vs_torch_max_abs=float(np.max(np.abs(parity))),
        coreml_vs_torch_rmse=float(math.sqrt(np.mean(parity**2))),
        noise_conditioning_rmse=float(
            math.sqrt(np.mean(conditioning_difference**2))
        ),
    )
    latency = LatencyMetrics(
        compiled_model_load_ms=load_ms,
        first_prediction_ms=first_ms,
        warm_prediction_p50_ms=statistics.median(warm_ms),
        warm_prediction_p95_ms=sorted_warm[p95_index],
        warm_prediction_samples=len(warm_ms),
    )
    return metrics, latency


def conversion_gate_failures(metrics: ConversionMetrics) -> list[str]:
    failures: list[str] = []
    if metrics.pmrid_iso_vs_noise_parameter_max_abs > 1.0e-12:
        failures.append("PMRID ISO and Shadow noise preprocessing are not equivalent")
    if metrics.torch_adapter_vs_pmrid_reference_max_abs > 1.0e-5:
        failures.append("Torch adapter differs from the published PMRID preprocessing")
    if metrics.coreml_vs_torch_max_abs > 1.0e-3:
        failures.append("Core ML output differs from the Torch adapter")
    if metrics.noise_conditioning_rmse < 1.0e-6:
        failures.append("converted model is not measurably conditioned by noise")
    return failures


def run(arguments: argparse.Namespace) -> int:
    started = time.perf_counter()
    upstream_directory = arguments.upstream_dir.resolve()
    receipt = verify_upstream(upstream_directory)
    output_directory = validate_output_directory(arguments.output_dir)
    if arguments.tile_edge < 16 or arguments.tile_edge % 16 != 0:
        raise ValueError("PMRID tile edge must be a positive multiple of 16")
    network = load_verified_network(upstream_directory)
    adapter = NoiseConditionedPmrid(network)
    adapter.eval()
    package_path, compiled_path = export_and_compile_coreml(
        adapter,
        arguments.tile_edge,
        output_directory,
    )
    identity = compiled_model_tree_identity(compiled_path)
    metrics, latency = benchmark_coreml(
        adapter,
        compiled_path,
        arguments.tile_edge,
        arguments.warm_predictions,
    )
    failures = conversion_gate_failures(metrics)
    report = {
        "schema": MODEL_SCHEMA,
        "status": (
            "public_checkpoint_conversion_gate_passed"
            if not failures
            else "public_checkpoint_conversion_gate_failed"
        ),
        "product_eligible": False,
        "preprocessing_contract": PREPROCESSING_CONTRACT,
        "upstream": asdict(receipt),
        "quality_scope": "conversion-only-no-public-benchmark-data",
        "metrics": asdict(metrics),
        "latency": asdict(latency),
        "tiling": {
            "compiled_tile_edge": arguments.tile_edge,
            "architectural_receptive_field": PMRID_RECEPTIVE_FIELD,
            "exact_halo": PMRID_EXACT_HALO,
            "minimum_tile_edge_for_one_exact_core_sample": (
                PMRID_EXACT_HALO * 2 + 1
            ),
            "production_tiling_admitted": False,
        },
        "gate_failures": failures,
        "limitations": [
            "official PMRID noisy/clean benchmark data was not evaluated",
            "public weights were trained for the OPPO Reno 10x camera domain",
            "fixed-tile parity does not establish seam-free Shadow tiling",
            "checkpoint is not bundled, selected by default, or exposed in UI",
        ],
        "artifacts": {
            "upstream_torch_checkpoint": str(
                upstream_directory / "models/torch_pretrained.ckp"
            ),
            "upstream_torch_checkpoint_sha256": PINNED_FILES[
                "models/torch_pretrained.ckp"
            ],
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
    _write_json(output_directory / "report.json", report)
    print(json.dumps(report, indent=2, sort_keys=True), flush=True)
    return 0 if not failures else 2


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--upstream-dir",
        type=Path,
        required=True,
        help="pinned external PMRID checkout",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="absolute empty directory outside the repository",
    )
    parser.add_argument("--tile-edge", type=int, default=64)
    parser.add_argument("--warm-predictions", type=int, default=10)
    return parser.parse_args()


if __name__ == "__main__":
    try:
        raise SystemExit(run(parse_arguments()))
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
