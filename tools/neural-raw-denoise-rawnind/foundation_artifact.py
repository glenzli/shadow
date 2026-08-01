#!/usr/bin/env python3
"""Durable single-file artifact for a materialized RawNIND RAW foundation."""

from __future__ import annotations

from dataclasses import asdict, dataclass, replace
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
from typing import BinaryIO
import uuid

import numpy as np

import package_contract
import stripe_lifecycle


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
ARTIFACT_EXTENSION = ".shadowrawf"
ARTIFACT_SCHEMA = "shadow-raw-foundation-artifact-v1"
HEADER_SCHEMA = "shadow-raw-foundation-header-v1"
CACHE_KEY_SCHEMA = "shadow-raw-foundation-cache-key-v1"
IMPLEMENTATION_REVISION = "rawnind-public-bayer-foundation-v1"
FILE_MAGIC = b"SHRAWF01"
FOOTER_MAGIC = b"SHRFEND1"
HEADER_PREFIX = struct.Struct("<8sQ")
FOOTER = struct.Struct("<8sQQ32s")
MAX_JSON_BYTES = 16 * 1024 * 1024
HASH_CHUNK_BYTES = 1024 * 1024


@dataclass(frozen=True)
class RawNindFoundationContract:
    source_sha256: str
    source_size_bytes: int
    source_pixel_contract_sha256: str
    raw_preprocessing: dict[str, object]
    model: dict[str, object]
    execution: dict[str, object]
    algorithm: dict[str, object]


@dataclass(frozen=True)
class ArtifactStripe:
    index: int
    y_start: int
    rows: int
    offset: int
    byte_length: int
    sha256: str


@dataclass(frozen=True)
class ArtifactVerification:
    path: str
    contract: RawNindFoundationContract
    publication: stripe_lifecycle.StripePublication
    output_shape_sensor: list[int]
    cache_key_sha256: str
    artifact_identity_sha256: str
    sequence_sha256: str
    payload_bytes: int
    file_bytes: int
    file_sha256: str
    stripes: list[ArtifactStripe]
    recovered_from_partial: bool = False


def _canonical_json(value: object) -> bytes:
    return json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def _json_copy(value: object) -> object:
    return json.loads(_canonical_json(value))


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _validate_sha256(value: object, label: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise ValueError(f"{label} must be a lowercase SHA-256")
    return value


def _require_exact_keys(
    value: object,
    keys: set[str],
    label: str,
) -> dict[str, object]:
    if not isinstance(value, dict) or set(value) != keys:
        raise ValueError(f"{label} does not match its schema")
    return value


def _path_is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
    except ValueError:
        return False
    return True


def _validate_external_destination(path: Path) -> Path:
    resolved = path.resolve()
    if resolved.suffix != ARTIFACT_EXTENSION:
        raise ValueError(
            f"foundation artifact must use {ARTIFACT_EXTENSION}"
        )
    if _path_is_within(resolved, REPOSITORY_ROOT):
        raise ValueError(
            "foundation artifact payload must stay outside the repository"
        )
    return resolved


def _expected_model_identity() -> dict[str, object]:
    return {
        "graph_member": "rawdenoise-nind/model_bayer.onnx",
        "graph_sha256": package_contract.PACKAGE_MEMBERS[
            "rawdenoise-nind/model_bayer.onnx"
        ][1],
        "license": "GPL-3.0",
        "package_sha256": package_contract.PACKAGE_SHA256,
        "release": package_contract.UPSTREAM_RELEASE,
        "repository": package_contract.UPSTREAM_REPOSITORY,
        "revision": package_contract.UPSTREAM_REVISION,
        "training_repository": package_contract.TRAINING_REPOSITORY,
        "training_revision": package_contract.TRAINING_REVISION,
    }


def _algorithm_identity(
    plan: stripe_lifecycle.StripePlan,
) -> dict[str, object]:
    geometry = plan.tiling
    return {
        "blend_overlap_packed": geometry.blend_overlap_packed,
        "blend_width_packed": geometry.blend_width_packed,
        "exact_halo_packed": geometry.exact_halo_packed,
        "implementation_revision": IMPLEMENTATION_REVISION,
        "inference_passes": plan.inference_passes,
        "input_channel_order": ["R", "G1", "G2", "B"],
        "normalization": "per-cfa-site-black-to-white-range-clipped",
        "output_scale": geometry.output_scale,
        "output_space": "linear-camera-rgb",
        "padding": "numpy-reflect-direct-index",
        "pool_alignment_packed": geometry.pool_alignment_packed,
        "scale_policy": "one-global-output-mean-to-input-mean",
        "step_packed": geometry.step_packed,
        "tile_edge_packed": geometry.tile_edge_packed,
        "white_balance": "none",
    }


def make_contract(
    source_path: Path,
    raw_preprocessing: dict[str, object],
    plan: stripe_lifecycle.StripePlan,
    source_pixel_contract_sha256: str,
    *,
    execution: dict[str, object],
) -> RawNindFoundationContract:
    source = source_path.resolve()
    if not source.is_file():
        raise ValueError("foundation source RAW does not exist")
    preprocessing = _json_copy(raw_preprocessing)
    if not isinstance(preprocessing, dict):
        raise ValueError("RAW preprocessing receipt must be an object")
    required_preprocessing = {
        "sensor_shape",
        "packed_shape",
        "raw_pattern",
        "source_raw_pattern",
        "force_rggb_crop_sensor",
        "color_description",
        "white_level",
        "black_level_per_channel",
        "decoder_provider_id",
        "decoder_provider_version",
        "decoded_samples_sha256",
    }
    if set(preprocessing) != required_preprocessing:
        raise ValueError("RAW preprocessing receipt changed")
    if preprocessing["packed_shape"] != plan.tiling.input_shape_packed:
        raise ValueError("RAW preprocessing shape does not match the stripe plan")
    execution_copy = _json_copy(execution)
    if not isinstance(execution_copy, dict):
        raise ValueError("execution identity must be an object")
    required_execution = {
        "engine",
        "runtime_version",
        "requested_provider",
        "active_providers",
        "platform",
        "machine",
    }
    if set(execution_copy) != required_execution:
        raise ValueError("execution identity changed")
    return RawNindFoundationContract(
        source_sha256=package_contract.sha256_file(source),
        source_size_bytes=source.stat().st_size,
        source_pixel_contract_sha256=_validate_sha256(
            source_pixel_contract_sha256,
            "source pixel contract",
        ),
        raw_preprocessing=preprocessing,
        model=_expected_model_identity(),
        execution=execution_copy,
        algorithm=_algorithm_identity(plan),
    )


def _contract_from_json(value: object) -> RawNindFoundationContract:
    payload = _require_exact_keys(
        value,
        {
            "source_sha256",
            "source_size_bytes",
            "source_pixel_contract_sha256",
            "raw_preprocessing",
            "model",
            "execution",
            "algorithm",
        },
        "foundation contract",
    )
    source_sha256 = _validate_sha256(
        payload["source_sha256"],
        "source identity",
    )
    source_size = payload["source_size_bytes"]
    if not isinstance(source_size, int) or source_size <= 0:
        raise ValueError("source size must be positive")
    source_pixel_contract_sha256 = _validate_sha256(
        payload["source_pixel_contract_sha256"],
        "source pixel contract",
    )
    raw_preprocessing = payload["raw_preprocessing"]
    model = payload["model"]
    execution = payload["execution"]
    algorithm = payload["algorithm"]
    raw_preprocessing = _require_exact_keys(
        raw_preprocessing,
        {
            "sensor_shape",
            "packed_shape",
            "raw_pattern",
            "source_raw_pattern",
            "force_rggb_crop_sensor",
            "color_description",
            "white_level",
            "black_level_per_channel",
            "decoder_provider_id",
            "decoder_provider_version",
            "decoded_samples_sha256",
        },
        "RAW preprocessing identity",
    )
    sensor_shape = raw_preprocessing["sensor_shape"]
    packed_shape = raw_preprocessing["packed_shape"]
    if (
        not isinstance(sensor_shape, list)
        or len(sensor_shape) != 2
        or any(not isinstance(item, int) or item <= 0 for item in sensor_shape)
        or not isinstance(packed_shape, list)
        or len(packed_shape) != 3
        or packed_shape[0] != 4
        or any(not isinstance(item, int) or item <= 0 for item in packed_shape)
        or sensor_shape != [packed_shape[1] * 2, packed_shape[2] * 2]
    ):
        raise ValueError("RAW preprocessing dimensions are invalid")
    for pattern_key in ("raw_pattern", "source_raw_pattern"):
        pattern = raw_preprocessing[pattern_key]
        if (
            not isinstance(pattern, list)
            or len(pattern) != 2
            or any(not isinstance(row, list) or len(row) != 2 for row in pattern)
            or any(
                not isinstance(item, int)
                for row in pattern
                for item in row
            )
        ):
            raise ValueError(f"{pattern_key} is invalid")
    crop = raw_preprocessing["force_rggb_crop_sensor"]
    if (
        not isinstance(crop, list)
        or len(crop) != 2
        or any(item not in (0, 1) for item in crop)
    ):
        raise ValueError("RAW force-RGGB crop is invalid")
    color_description = raw_preprocessing["color_description"]
    if not isinstance(color_description, str) or not color_description:
        raise ValueError("RAW color description is invalid")
    black_levels = raw_preprocessing["black_level_per_channel"]
    white_level = raw_preprocessing["white_level"]
    decoder_provider_id = raw_preprocessing["decoder_provider_id"]
    decoder_provider_version = raw_preprocessing["decoder_provider_version"]
    decoded_samples_sha256 = raw_preprocessing["decoded_samples_sha256"]
    if (
        not isinstance(white_level, (int, float))
        or not math.isfinite(white_level)
        or not isinstance(black_levels, list)
        or len(black_levels) < 4
        or any(
            not isinstance(item, (int, float)) or not math.isfinite(item)
            for item in black_levels
        )
        or not isinstance(decoder_provider_id, str)
        or not decoder_provider_id
        or not isinstance(decoder_provider_version, str)
        or not decoder_provider_version
    ):
        raise ValueError("RAW level metadata is invalid")
    _validate_sha256(decoded_samples_sha256, "decoded RAW samples")
    if model != _expected_model_identity():
        raise ValueError("foundation model identity changed")
    execution = _require_exact_keys(
        execution,
        {
            "engine",
            "runtime_version",
            "requested_provider",
            "active_providers",
            "platform",
            "machine",
        },
        "execution identity",
    )
    for key in (
        "engine",
        "runtime_version",
        "requested_provider",
        "platform",
        "machine",
    ):
        if not isinstance(execution[key], str) or not execution[key]:
            raise ValueError(f"execution {key} is invalid")
    providers = execution["active_providers"]
    if (
        not isinstance(providers, list)
        or not providers
        or any(not isinstance(item, str) or not item for item in providers)
    ):
        raise ValueError("execution providers are invalid")
    algorithm = _require_exact_keys(
        algorithm,
        {
            "blend_overlap_packed",
            "blend_width_packed",
            "exact_halo_packed",
            "implementation_revision",
            "inference_passes",
            "input_channel_order",
            "normalization",
            "output_scale",
            "output_space",
            "padding",
            "pool_alignment_packed",
            "scale_policy",
            "step_packed",
            "tile_edge_packed",
            "white_balance",
        },
        "algorithm identity",
    )
    if algorithm.get("implementation_revision") != IMPLEMENTATION_REVISION:
        raise ValueError("foundation implementation revision changed")
    fixed_algorithm = {
        "inference_passes": 2,
        "input_channel_order": ["R", "G1", "G2", "B"],
        "normalization": "per-cfa-site-black-to-white-range-clipped",
        "output_scale": 2,
        "output_space": "linear-camera-rgb",
        "padding": "numpy-reflect-direct-index",
        "pool_alignment_packed": 16,
        "scale_policy": "one-global-output-mean-to-input-mean",
        "tile_edge_packed": 512,
        "white_balance": "none",
    }
    for key, expected in fixed_algorithm.items():
        if algorithm[key] != expected:
            raise ValueError(f"foundation algorithm {key} changed")
    halo = algorithm["exact_halo_packed"]
    overlap = algorithm["blend_overlap_packed"]
    blend_width = algorithm["blend_width_packed"]
    step = algorithm["step_packed"]
    if (
        not isinstance(halo, int)
        or halo < 0
        or not isinstance(overlap, int)
        or overlap <= halo
        or not isinstance(blend_width, int)
        or blend_width != 2 * (overlap - halo)
        or not isinstance(step, int)
        or step != 512 - 2 * overlap
        or step <= 0
        or step % 16
    ):
        raise ValueError("foundation tiling identity is invalid")
    return RawNindFoundationContract(
        source_sha256=source_sha256,
        source_size_bytes=source_size,
        source_pixel_contract_sha256=source_pixel_contract_sha256,
        raw_preprocessing=raw_preprocessing,
        model=model,
        execution=execution,
        algorithm=algorithm,
    )


def _header(
    contract: RawNindFoundationContract,
    output_shape_sensor: tuple[int, int, int],
) -> dict[str, object]:
    return {
        "cache_key_sha256": artifact_cache_key(
            contract,
            output_shape_sensor,
        ),
        "contract": asdict(contract),
        "output": {
            "byte_order": "little",
            "channels": ["R", "G", "B"],
            "demosaiced": True,
            "dtype": "float32",
            "layout": "stripe-chw",
            "shape_sensor": list(output_shape_sensor),
            "space": "linear-camera-rgb",
        },
        "schema": HEADER_SCHEMA,
        "semantic_boundary": "raw-foundation-materialization",
    }


def artifact_cache_key(
    contract: RawNindFoundationContract,
    output_shape_sensor: tuple[int, int, int],
) -> str:
    material = {
        "contract": asdict(contract),
        "output_shape_sensor": list(output_shape_sensor),
        "schema": CACHE_KEY_SCHEMA,
    }
    return _sha256_bytes(_canonical_json(material))


def _f64_identity(value: float) -> str:
    return struct.pack(">d", value).hex()


def _f64_from_identity(value: object, label: str) -> float:
    if (
        not isinstance(value, str)
        or len(value) != 16
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise ValueError(
            f"{label} must be 16 lowercase IEEE-754 hexadecimal digits"
        )
    return struct.unpack(">d", bytes.fromhex(value))[0]


def _publication_identity_material(
    publication: stripe_lifecycle.StripePublication,
) -> dict[str, object]:
    return {
        "first_pass_raw_output_mean_f64_bits": _f64_identity(
            publication.first_pass_raw_output_mean
        ),
        "global_gain_f64_bits": _f64_identity(publication.global_gain),
        "global_input_mean_f64_bits": _f64_identity(
            publication.global_input_mean
        ),
        "output_mean_f64_bits": _f64_identity(publication.output_mean),
        "producer": publication.producer,
        "replay_relative_mean_delta_f64_bits": _f64_identity(
            publication.replay_relative_mean_delta
        ),
        "second_pass_raw_output_mean_f64_bits": _f64_identity(
            publication.second_pass_raw_output_mean
        ),
    }


def _identity_material(manifest: dict[str, object]) -> dict[str, object]:
    return {
        key: value
        for key, value in manifest.items()
        if key != "artifact_identity_sha256"
    }


def _write_all(stream: BinaryIO, value: memoryview | bytes) -> None:
    remaining = memoryview(value)
    while remaining:
        written = stream.write(remaining)
        if written is None or written <= 0:
            raise OSError("foundation artifact write made no progress")
        remaining = remaining[written:]


def _read_exact(stream: BinaryIO, length: int) -> bytes:
    value = stream.read(length)
    if len(value) != length:
        raise ValueError("foundation artifact ended unexpectedly")
    return value


def _readinto_exact(stream: BinaryIO, target: memoryview) -> None:
    remaining = target
    while remaining:
        read = stream.readinto(remaining)
        if read is None or read <= 0:
            raise ValueError("foundation artifact ended during row read")
        remaining = remaining[read:]


def _fsync_directory(directory: Path) -> None:
    descriptor = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


class AtomicFoundationArtifactSink(stripe_lifecycle.DigestStripeSink):
    """Write one verified file and publish it without overwriting a target."""

    def __init__(
        self,
        destination: Path,
        contract: RawNindFoundationContract,
    ) -> None:
        super().__init__()
        self.destination = _validate_external_destination(destination)
        self.contract = contract
        self.partial_path: Path | None = None
        self._stream: BinaryIO | None = None
        self._header_sha256: str | None = None
        self._stripes: list[ArtifactStripe] = []

    def begin(self, output_shape_sensor: tuple[int, int, int]) -> None:
        if sys.byteorder != "little":
            raise ValueError("foundation artifact writer requires little endian")
        if self.destination.exists():
            raise ValueError("foundation artifact destination already exists")
        self.destination.parent.mkdir(parents=True, exist_ok=True)
        partial = self.destination.with_name(
            f".{self.destination.name}.partial-{uuid.uuid4().hex}"
        )
        try:
            super().begin(output_shape_sensor)
            stream = partial.open("xb+", buffering=0)
            header_bytes = _canonical_json(
                _header(self.contract, output_shape_sensor)
            )
            if len(header_bytes) > MAX_JSON_BYTES:
                raise ValueError("foundation artifact header is too large")
            _write_all(
                stream,
                HEADER_PREFIX.pack(FILE_MAGIC, len(header_bytes)),
            )
            _write_all(stream, header_bytes)
            self.partial_path = partial
            self._stream = stream
            self._header_sha256 = _sha256_bytes(header_bytes)
        except BaseException:
            if self.state == "writing":
                self.abort()
            else:
                partial.unlink(missing_ok=True)
            raise

    def write_stripe(self, y_start: int, stripe: np.ndarray) -> None:
        if self._stream is None:
            raise RuntimeError("foundation artifact has no open payload")
        super().write_stripe(y_start, stripe)
        offset = self._stream.tell()
        digest = hashlib.sha256()
        for channel in stripe:
            payload = memoryview(channel).cast("B")
            digest.update(payload)
            _write_all(self._stream, payload)
        byte_length = stripe.size * stripe.dtype.itemsize
        self._stripes.append(
            ArtifactStripe(
                index=len(self._stripes),
                y_start=y_start,
                rows=stripe.shape[1],
                offset=offset,
                byte_length=byte_length,
                sha256=digest.hexdigest(),
            )
        )

    def commit(
        self,
        publication: stripe_lifecycle.StripePublication,
    ) -> stripe_lifecycle.SinkReceipt:
        shape = self._validate_commit(publication)
        if (
            self._stream is None
            or self.partial_path is None
            or self._header_sha256 is None
        ):
            raise RuntimeError("foundation artifact is not writable")
        manifest: dict[str, object] = {
            "artifact_identity_sha256": "",
            "cache_key_sha256": artifact_cache_key(self.contract, shape),
            "header_sha256": self._header_sha256,
            "payload_bytes": self.byte_count,
            "publication": _publication_identity_material(publication),
            "schema": ARTIFACT_SCHEMA,
            "sequence_sha256": self.sequence_sha256,
            "stripes": [asdict(stripe) for stripe in self._stripes],
        }
        manifest["artifact_identity_sha256"] = _sha256_bytes(
            _canonical_json(_identity_material(manifest))
        )
        manifest_bytes = _canonical_json(manifest)
        if len(manifest_bytes) > MAX_JSON_BYTES:
            raise ValueError("foundation artifact manifest is too large")
        manifest_offset = self._stream.tell()
        _write_all(self._stream, manifest_bytes)
        _write_all(
            self._stream,
            FOOTER.pack(
                FOOTER_MAGIC,
                manifest_offset,
                len(manifest_bytes),
                hashlib.sha256(manifest_bytes).digest(),
            ),
        )
        os.fsync(self._stream.fileno())
        self._stream.close()
        self._stream = None

        verification = verify_artifact(self.partial_path)
        if verification.sequence_sha256 != self.sequence_sha256:
            raise ValueError("independent artifact sequence verification failed")
        try:
            os.link(self.partial_path, self.destination)
            try:
                _fsync_directory(self.destination.parent)
            except BaseException:
                self.destination.unlink(missing_ok=True)
                try:
                    _fsync_directory(self.destination.parent)
                except OSError:
                    pass
                raise
        except FileExistsError as error:
            raise ValueError(
                "foundation artifact destination appeared during publication"
            ) from error

        receipt = stripe_lifecycle.SinkReceipt(
            format=stripe_lifecycle.STRIPE_SEQUENCE_FORMAT,
            output_shape_sensor=list(shape),
            byte_count=self.byte_count,
            stripe_count=self.stripe_count,
            sequence_sha256=self.sequence_sha256,
            committed=True,
            artifact_path=str(self.destination),
            artifact_cache_key_sha256=verification.cache_key_sha256,
            artifact_identity_sha256=verification.artifact_identity_sha256,
            artifact_file_sha256=verification.file_sha256,
            artifact_file_bytes=verification.file_bytes,
        )
        self._mark_committed(receipt)
        try:
            self.partial_path.unlink()
            _fsync_directory(self.destination.parent)
        except OSError:
            pass
        return receipt

    def abort(self) -> None:
        if self.state == "committed":
            super().abort()
        if self._stream is not None:
            self._stream.close()
            self._stream = None
        if self.partial_path is not None:
            self.partial_path.unlink(missing_ok=True)
        super().abort()


class OwnedFoundationArtifactPartialSink(AtomicFoundationArtifactSink):
    """Seal bytes directly into one absent partial owned by the caller.

    The application cache store already supplied a unique unpublished path.
    Avoiding another sibling temporary means a killed provider can leave only
    that exact owned path, which the application may safely discard or recover.
    This sink never publishes a cache entry and never creates parent folders.
    """

    def begin(self, output_shape_sensor: tuple[int, int, int]) -> None:
        if sys.byteorder != "little":
            raise ValueError("foundation artifact writer requires little endian")
        if self.destination.exists():
            raise ValueError("foundation artifact partial already exists")
        if not self.destination.parent.is_dir():
            raise ValueError("foundation artifact partial parent is missing")
        try:
            stripe_lifecycle.DigestStripeSink.begin(
                self,
                output_shape_sensor,
            )
            stream = self.destination.open("xb+", buffering=0)
            header_bytes = _canonical_json(
                _header(self.contract, output_shape_sensor)
            )
            if len(header_bytes) > MAX_JSON_BYTES:
                raise ValueError("foundation artifact header is too large")
            _write_all(
                stream,
                HEADER_PREFIX.pack(FILE_MAGIC, len(header_bytes)),
            )
            _write_all(stream, header_bytes)
            self.partial_path = self.destination
            self._stream = stream
            self._header_sha256 = _sha256_bytes(header_bytes)
        except BaseException:
            if self.state == "writing":
                self.abort()
            else:
                self.destination.unlink(missing_ok=True)
            raise

    def commit(
        self,
        publication: stripe_lifecycle.StripePublication,
    ) -> stripe_lifecycle.SinkReceipt:
        shape = self._validate_commit(publication)
        if (
            self._stream is None
            or self.partial_path != self.destination
            or self._header_sha256 is None
        ):
            raise RuntimeError("foundation artifact partial is not writable")
        manifest: dict[str, object] = {
            "artifact_identity_sha256": "",
            "cache_key_sha256": artifact_cache_key(self.contract, shape),
            "header_sha256": self._header_sha256,
            "payload_bytes": self.byte_count,
            "publication": _publication_identity_material(publication),
            "schema": ARTIFACT_SCHEMA,
            "sequence_sha256": self.sequence_sha256,
            "stripes": [asdict(stripe) for stripe in self._stripes],
        }
        manifest["artifact_identity_sha256"] = _sha256_bytes(
            _canonical_json(_identity_material(manifest))
        )
        manifest_bytes = _canonical_json(manifest)
        if len(manifest_bytes) > MAX_JSON_BYTES:
            raise ValueError("foundation artifact manifest is too large")
        manifest_offset = self._stream.tell()
        _write_all(self._stream, manifest_bytes)
        _write_all(
            self._stream,
            FOOTER.pack(
                FOOTER_MAGIC,
                manifest_offset,
                len(manifest_bytes),
                hashlib.sha256(manifest_bytes).digest(),
            ),
        )
        os.fsync(self._stream.fileno())
        self._stream.close()
        self._stream = None

        verification = verify_artifact(self.destination)
        if verification.sequence_sha256 != self.sequence_sha256:
            raise ValueError("independent artifact sequence verification failed")
        receipt = stripe_lifecycle.SinkReceipt(
            format=stripe_lifecycle.STRIPE_SEQUENCE_FORMAT,
            output_shape_sensor=list(shape),
            byte_count=self.byte_count,
            stripe_count=self.stripe_count,
            sequence_sha256=self.sequence_sha256,
            committed=True,
            artifact_path=str(self.destination),
            artifact_cache_key_sha256=verification.cache_key_sha256,
            artifact_identity_sha256=verification.artifact_identity_sha256,
            artifact_file_sha256=verification.file_sha256,
            artifact_file_bytes=verification.file_bytes,
        )
        return self._mark_committed(receipt)


def _validate_header(
    value: object,
) -> tuple[
    RawNindFoundationContract,
    tuple[int, int, int],
    str,
]:
    header = _require_exact_keys(
        value,
        {
            "cache_key_sha256",
            "contract",
            "output",
            "schema",
            "semantic_boundary",
        },
        "foundation header",
    )
    if header["schema"] != HEADER_SCHEMA:
        raise ValueError("foundation header schema changed")
    if header["semantic_boundary"] != "raw-foundation-materialization":
        raise ValueError("foundation semantic boundary changed")
    contract = _contract_from_json(header["contract"])
    output = _require_exact_keys(
        header["output"],
        {
            "byte_order",
            "channels",
            "demosaiced",
            "dtype",
            "layout",
            "shape_sensor",
            "space",
        },
        "foundation output",
    )
    expected_output = {
        "byte_order": "little",
        "channels": ["R", "G", "B"],
        "demosaiced": True,
        "dtype": "float32",
        "layout": "stripe-chw",
        "space": "linear-camera-rgb",
    }
    for key, expected in expected_output.items():
        if output[key] != expected:
            raise ValueError(f"foundation output {key} changed")
    shape = output["shape_sensor"]
    if (
        not isinstance(shape, list)
        or len(shape) != 3
        or shape[0] != 3
        or any(not isinstance(value, int) or value <= 0 for value in shape)
    ):
        raise ValueError("foundation output shape is invalid")
    output_shape = (shape[0], shape[1], shape[2])
    preprocessing_shape = contract.raw_preprocessing["sensor_shape"]
    if list(output_shape[1:]) != preprocessing_shape:
        raise ValueError(
            "foundation output does not match RAW preprocessing dimensions"
        )
    cache_key = _validate_sha256(
        header["cache_key_sha256"],
        "foundation cache key",
    )
    if cache_key != artifact_cache_key(contract, output_shape):
        raise ValueError("foundation cache key does not match its contract")
    return contract, output_shape, cache_key


def _publication_from_json(
    value: object,
) -> stripe_lifecycle.StripePublication:
    payload = _require_exact_keys(
        value,
        {
            "first_pass_raw_output_mean_f64_bits",
            "global_gain_f64_bits",
            "global_input_mean_f64_bits",
            "output_mean_f64_bits",
            "producer",
            "replay_relative_mean_delta_f64_bits",
            "second_pass_raw_output_mean_f64_bits",
        },
        "stripe publication",
    )
    producer = payload["producer"]
    if not isinstance(producer, str):
        raise ValueError("stripe publication producer must be text")
    publication = stripe_lifecycle.StripePublication(
        producer=producer,
        global_input_mean=_f64_from_identity(
            payload["global_input_mean_f64_bits"],
            "global input mean",
        ),
        first_pass_raw_output_mean=_f64_from_identity(
            payload["first_pass_raw_output_mean_f64_bits"],
            "first-pass RAW output mean",
        ),
        second_pass_raw_output_mean=_f64_from_identity(
            payload["second_pass_raw_output_mean_f64_bits"],
            "second-pass RAW output mean",
        ),
        replay_relative_mean_delta=_f64_from_identity(
            payload["replay_relative_mean_delta_f64_bits"],
            "replay relative mean delta",
        ),
        global_gain=_f64_from_identity(
            payload["global_gain_f64_bits"],
            "global gain",
        ),
        output_mean=_f64_from_identity(
            payload["output_mean_f64_bits"],
            "output mean",
        ),
    )
    stripe_lifecycle.validate_publication(publication)
    return publication


def _hash_file(stream: BinaryIO) -> str:
    stream.seek(0)
    digest = hashlib.sha256()
    while chunk := stream.read(HASH_CHUNK_BYTES):
        digest.update(chunk)
    return digest.hexdigest()


def _verify_stream(
    stream: BinaryIO,
    path: Path,
) -> ArtifactVerification:
    file_bytes = os.fstat(stream.fileno()).st_size
    if file_bytes < HEADER_PREFIX.size + FOOTER.size:
        raise ValueError("foundation artifact is too small")
    stream.seek(0)
    magic, header_length = HEADER_PREFIX.unpack(
        _read_exact(stream, HEADER_PREFIX.size)
    )
    if magic != FILE_MAGIC:
        raise ValueError("foundation artifact magic changed")
    if header_length <= 0 or header_length > MAX_JSON_BYTES:
        raise ValueError("foundation artifact header length is invalid")
    header_bytes = _read_exact(stream, header_length)
    try:
        header_json = json.loads(header_bytes)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("foundation artifact header is invalid JSON") from error
    contract, output_shape, cache_key = _validate_header(header_json)

    stream.seek(file_bytes - FOOTER.size)
    (
        footer_magic,
        manifest_offset,
        manifest_length,
        manifest_digest,
    ) = FOOTER.unpack(_read_exact(stream, FOOTER.size))
    if footer_magic != FOOTER_MAGIC:
        raise ValueError("foundation artifact footer is missing")
    payload_start = HEADER_PREFIX.size + header_length
    if (
        manifest_length <= 0
        or manifest_length > MAX_JSON_BYTES
        or manifest_offset < payload_start
        or manifest_offset + manifest_length + FOOTER.size != file_bytes
    ):
        raise ValueError("foundation artifact manifest bounds are invalid")
    stream.seek(manifest_offset)
    manifest_bytes = _read_exact(stream, manifest_length)
    if hashlib.sha256(manifest_bytes).digest() != manifest_digest:
        raise ValueError("foundation artifact manifest digest mismatch")
    try:
        manifest_value = json.loads(manifest_bytes)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("foundation artifact manifest is invalid JSON") from error
    manifest = _require_exact_keys(
        manifest_value,
        {
            "artifact_identity_sha256",
            "cache_key_sha256",
            "header_sha256",
            "payload_bytes",
            "publication",
            "schema",
            "sequence_sha256",
            "stripes",
        },
        "foundation manifest",
    )
    if manifest["schema"] != ARTIFACT_SCHEMA:
        raise ValueError("foundation artifact schema changed")
    if manifest["cache_key_sha256"] != cache_key:
        raise ValueError("manifest cache key differs from the header")
    if manifest["header_sha256"] != _sha256_bytes(header_bytes):
        raise ValueError("foundation artifact header digest mismatch")
    publication = _publication_from_json(manifest["publication"])
    artifact_identity = _validate_sha256(
        manifest["artifact_identity_sha256"],
        "foundation artifact identity",
    )
    if artifact_identity != _sha256_bytes(
        _canonical_json(_identity_material(manifest))
    ):
        raise ValueError("foundation artifact identity mismatch")
    sequence_sha256 = _validate_sha256(
        manifest["sequence_sha256"],
        "stripe sequence",
    )
    stripe_values = manifest["stripes"]
    if (
        not isinstance(stripe_values, list)
        or not stripe_values
        or len(stripe_values) > output_shape[1]
    ):
        raise ValueError("foundation stripe table is invalid")

    stripes: list[ArtifactStripe] = []
    cursor = payload_start
    next_y = 0
    payload_bytes = 0
    sequence_digest = hashlib.sha256()
    sequence_digest.update(
        stripe_lifecycle.sequence_header_bytes(output_shape)
    )
    for index, value in enumerate(stripe_values):
        entry = _require_exact_keys(
            value,
            {
                "index",
                "y_start",
                "rows",
                "offset",
                "byte_length",
                "sha256",
            },
            "foundation stripe",
        )
        if entry["index"] != index or entry["y_start"] != next_y:
            raise ValueError("foundation stripes are not contiguous")
        rows = entry["rows"]
        offset = entry["offset"]
        byte_length = entry["byte_length"]
        if (
            not isinstance(rows, int)
            or rows <= 0
            or not isinstance(offset, int)
            or offset != cursor
            or not isinstance(byte_length, int)
            or next_y + rows > output_shape[1]
        ):
            raise ValueError("foundation stripe geometry is invalid")
        expected_bytes = 3 * rows * output_shape[2] * 4
        if byte_length != expected_bytes:
            raise ValueError("foundation stripe byte length is invalid")
        expected_digest = _validate_sha256(
            entry["sha256"],
            "foundation stripe",
        )
        stream.seek(offset)
        remaining = byte_length
        digest = hashlib.sha256()
        while remaining:
            chunk = _read_exact(
                stream,
                min(remaining, HASH_CHUNK_BYTES),
            )
            if len(chunk) % 4:
                raise ValueError("foundation float payload is misaligned")
            if not np.isfinite(np.frombuffer(chunk, dtype="<f4")).all():
                raise ValueError("foundation artifact contains non-finite pixels")
            digest.update(chunk)
            sequence_digest.update(chunk)
            remaining -= len(chunk)
        if digest.hexdigest() != expected_digest:
            raise ValueError("foundation stripe payload digest mismatch")
        stripes.append(
            ArtifactStripe(
                index=index,
                y_start=next_y,
                rows=rows,
                offset=offset,
                byte_length=byte_length,
                sha256=expected_digest,
            )
        )
        next_y += rows
        cursor += byte_length
        payload_bytes += byte_length
    if next_y != output_shape[1] or cursor != manifest_offset:
        raise ValueError("foundation stripes do not cover the complete output")
    if manifest["payload_bytes"] != payload_bytes:
        raise ValueError("foundation payload byte count mismatch")
    if sequence_digest.hexdigest() != sequence_sha256:
        raise ValueError("foundation stripe sequence digest mismatch")
    file_sha256 = _hash_file(stream)
    return ArtifactVerification(
        path=str(path.resolve()),
        contract=contract,
        publication=publication,
        output_shape_sensor=list(output_shape),
        cache_key_sha256=cache_key,
        artifact_identity_sha256=artifact_identity,
        sequence_sha256=sequence_sha256,
        payload_bytes=payload_bytes,
        file_bytes=file_bytes,
        file_sha256=file_sha256,
        stripes=stripes,
    )


def verify_artifact(path: Path) -> ArtifactVerification:
    resolved = path.resolve()
    if not resolved.is_file():
        raise ValueError("foundation artifact does not exist")
    with resolved.open("rb", buffering=0) as stream:
        return _verify_stream(stream, resolved)


class FoundationArtifactReader:
    """Hold one verified file descriptor for bounded random row reads."""

    def __init__(self, path: Path) -> None:
        if sys.byteorder != "little":
            raise ValueError("foundation artifact reader requires little endian")
        self.path = path.resolve()
        self._stream = self.path.open("rb", buffering=0)
        try:
            self.verification = _verify_stream(self._stream, self.path)
        except BaseException:
            self._stream.close()
            raise

    def close(self) -> None:
        self._stream.close()

    def __enter__(self) -> FoundationArtifactReader:
        return self

    def __exit__(self, *unused: object) -> None:
        self.close()

    def read_rows(self, y_start: int, rows: int) -> np.ndarray:
        shape = self.verification.output_shape_sensor
        if y_start < 0 or rows <= 0 or y_start + rows > shape[1]:
            raise ValueError("foundation row request is outside the output")
        output = np.empty((3, rows, shape[2]), dtype=np.float32)
        request_end = y_start + rows
        row_bytes = shape[2] * 4
        for stripe in self.verification.stripes:
            stripe_end = stripe.y_start + stripe.rows
            overlap_start = max(y_start, stripe.y_start)
            overlap_end = min(request_end, stripe_end)
            if overlap_start >= overlap_end:
                continue
            local_start = overlap_start - stripe.y_start
            destination_start = overlap_start - y_start
            overlap_rows = overlap_end - overlap_start
            channel_bytes = stripe.rows * row_bytes
            for channel in range(3):
                offset = (
                    stripe.offset
                    + channel * channel_bytes
                    + local_start * row_bytes
                )
                self._stream.seek(offset)
                target = output[
                    channel,
                    destination_start : destination_start + overlap_rows,
                ]
                _readinto_exact(
                    self._stream,
                    memoryview(target).cast("B"),
                )
        return output


def recover_completed_artifact(
    partial_path: Path,
    destination: Path,
) -> ArtifactVerification:
    partial = partial_path.resolve()
    final = _validate_external_destination(destination)
    verification = verify_artifact(partial)
    final.parent.mkdir(parents=True, exist_ok=True)
    if final.exists():
        existing = verify_artifact(final)
        if existing.artifact_identity_sha256 != (
            verification.artifact_identity_sha256
        ):
            raise ValueError(
                "recovery destination contains a different artifact"
            )
        partial.unlink(missing_ok=True)
        _fsync_directory(final.parent)
        return replace(existing, recovered_from_partial=True)
    try:
        os.link(partial, final)
        try:
            _fsync_directory(final.parent)
        except BaseException:
            final.unlink(missing_ok=True)
            try:
                _fsync_directory(final.parent)
            except OSError:
                pass
            raise
    except FileExistsError as error:
        raise ValueError(
            "recovery destination appeared during publication"
        ) from error
    partial.unlink(missing_ok=True)
    _fsync_directory(final.parent)
    return replace(
        verification,
        path=str(final),
        recovered_from_partial=True,
    )


def as_json(value: object) -> object:
    if hasattr(value, "__dataclass_fields__"):
        return asdict(value)
    raise TypeError(f"cannot serialize {type(value)!r}")
