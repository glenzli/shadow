#!/usr/bin/env python3
"""Read Shadow's bounded provider-neutral active Bayer staging contract."""

from __future__ import annotations

import dataclasses
import hashlib
import math
import pathlib


STAGING_SCHEMA = "shadow-raw-frame-staging-20260822.1"
DESCRIPTOR_CONTRACT = "active-camera-colour-response-20260822.1"
MAX_MANIFEST_BYTES = 16 * 1024
MAX_SAMPLE_BYTES = 512 * 1024 * 1024
REQUIRED_FIELDS = frozenset(
    {
        "descriptor_contract",
        "width",
        "height",
        "cfa",
        "black",
        "white",
        "linear_response",
        "has_linear_response",
        "orientation",
        "bits_per_sample",
        "as_shot_neutral",
        "camera_to_xyz_d50",
        "xyz_to_camera_d65",
        "camera_to_linear_srgb_d65",
        "pending_dng_opcode_bytes",
        "provider_id_hex",
        "provider_version_hex",
        "sample_bytes",
    }
)


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _integer(value: str, label: str, *, minimum: int = 0) -> int:
    try:
        parsed = int(value, 10)
    except ValueError as exc:
        raise ValueError(f"normalized mosaic {label} is not an integer") from exc
    if parsed < minimum:
        raise ValueError(f"normalized mosaic {label} is outside its supported range")
    return parsed


def _integer_tuple(value: str, count: int, label: str) -> tuple[int, ...]:
    parts = value.split(",")
    if len(parts) != count:
        raise ValueError(f"normalized mosaic {label} must contain {count} values")
    return tuple(_integer(part, label) for part in parts)


def _float_tuple(value: str, count: int, label: str) -> tuple[float, ...]:
    parts = value.split(",")
    if len(parts) != count:
        raise ValueError(f"normalized mosaic {label} must contain {count} values")
    try:
        parsed = tuple(float(part) for part in parts)
    except ValueError as exc:
        raise ValueError(f"normalized mosaic {label} is not numeric") from exc
    if not all(math.isfinite(item) for item in parsed):
        raise ValueError(f"normalized mosaic {label} contains a non-finite value")
    return parsed


def _matrix(value: str, label: str) -> tuple[float, ...] | None:
    return None if value == "-" else _float_tuple(value, 9, label)


def _decode_hex(value: str, label: str) -> str:
    try:
        return bytes.fromhex(value).decode("utf-8")
    except (ValueError, UnicodeDecodeError) as exc:
        raise ValueError(f"normalized mosaic {label} is not UTF-8 hex") from exc


@dataclasses.dataclass(frozen=True)
class NormalizedMosaic:
    manifest_path: pathlib.Path
    sample_path: pathlib.Path
    width: int
    height: int
    cfa: str
    black: tuple[int, int, int, int]
    white: tuple[int, int, int, int]
    linear_response: tuple[int, int, int, int]
    has_linear_response: bool
    orientation: int
    bits_per_sample: int
    as_shot_neutral: tuple[float, float, float, float]
    camera_to_xyz_d50: tuple[float, ...] | None
    xyz_to_camera_d65: tuple[float, ...] | None
    camera_to_linear_srgb_d65: tuple[float, ...] | None
    pending_dng_opcode_bytes: tuple[int, int, int]
    provider_id: str
    provider_version: str
    sample_bytes: int
    sample_sha256: str

    def metadata(self) -> dict[str, object]:
        """Return the exact serializable descriptor carried in DNGPrivateData."""

        return {
            "schema": STAGING_SCHEMA,
            "descriptor_contract": DESCRIPTOR_CONTRACT,
            "width": self.width,
            "height": self.height,
            "cfa": self.cfa,
            "black": list(self.black),
            "white": list(self.white),
            "linear_response": list(self.linear_response),
            "has_linear_response": self.has_linear_response,
            "orientation": self.orientation,
            "bits_per_sample": self.bits_per_sample,
            "as_shot_neutral": list(self.as_shot_neutral),
            "camera_to_xyz_d50": (
                list(self.camera_to_xyz_d50) if self.camera_to_xyz_d50 is not None else None
            ),
            "xyz_to_camera_d65": (
                list(self.xyz_to_camera_d65) if self.xyz_to_camera_d65 is not None else None
            ),
            "camera_to_linear_srgb_d65": (
                list(self.camera_to_linear_srgb_d65)
                if self.camera_to_linear_srgb_d65 is not None
                else None
            ),
            "pending_dng_opcode_bytes": list(self.pending_dng_opcode_bytes),
            "provider_id": self.provider_id,
            "provider_version": self.provider_version,
            "sample_bytes": self.sample_bytes,
            "sample_sha256": self.sample_sha256,
        }

    def colour_neutral(self) -> tuple[float, float, float]:
        buckets: dict[str, list[float]] = {"R": [], "G": [], "B": []}
        for site, colour in enumerate(self.cfa):
            buckets[colour].append(self.as_shot_neutral[site])
        if any(not values for values in buckets.values()):
            raise ValueError("normalized mosaic CFA does not contain R, G, and B")
        return tuple(sum(buckets[colour]) / len(buckets[colour]) for colour in "RGB")


def read_staging(manifest_path: pathlib.Path) -> NormalizedMosaic:
    path = manifest_path.expanduser().resolve(strict=False)
    if not path.is_file() or path.stat().st_size == 0 or path.stat().st_size > MAX_MANIFEST_BYTES:
        raise ValueError("normalized mosaic staging manifest is missing or invalid")
    text = path.read_text(encoding="utf-8")
    if not text.endswith("\n") or text.count("\n") != 1:
        raise ValueError("normalized mosaic staging manifest must be one complete line")
    tokens = text[:-1].split()
    if not tokens or tokens[0] != STAGING_SCHEMA:
        raise ValueError("normalized mosaic staging schema is unsupported")
    fields: dict[str, str] = {}
    for token in tokens[1:]:
        key, separator, value = token.partition("=")
        if not separator or not key or not value or key in fields:
            raise ValueError("normalized mosaic staging fields are malformed")
        fields[key] = value
    if fields.keys() != REQUIRED_FIELDS:
        raise ValueError("normalized mosaic staging field set changed")
    if fields["descriptor_contract"] != DESCRIPTOR_CONTRACT:
        raise ValueError("normalized mosaic descriptor contract is unsupported")

    width = _integer(fields["width"], "width", minimum=1)
    height = _integer(fields["height"], "height", minimum=1)
    sample_bytes = _integer(fields["sample_bytes"], "sample byte count", minimum=1)
    expected_bytes = width * height * 2
    if sample_bytes != expected_bytes or expected_bytes > MAX_SAMPLE_BYTES:
        raise ValueError("normalized mosaic sample dimensions are invalid")
    sample_path = pathlib.Path(f"{path}.u16le")
    if not sample_path.is_file() or sample_path.stat().st_size != sample_bytes:
        raise ValueError("normalized mosaic sample payload is incomplete")

    cfa = fields["cfa"]
    if len(cfa) != 4 or any(colour not in "RGB" for colour in cfa) or set(cfa) != set("RGB"):
        raise ValueError("normalized mosaic CFA is invalid")
    bits_per_sample = _integer(fields["bits_per_sample"], "bits per sample", minimum=1)
    if bits_per_sample > 16:
        raise ValueError("normalized mosaic bits per sample exceed uint16")
    black = _integer_tuple(fields["black"], 4, "black levels")
    white = _integer_tuple(fields["white"], 4, "white levels")
    linear_response = _integer_tuple(fields["linear_response"], 4, "linear response")
    if any(low >= high or high > 65535 for low, high in zip(black, white, strict=True)):
        raise ValueError("normalized mosaic black and white levels are invalid")
    has_linear_response_value = fields["has_linear_response"]
    if has_linear_response_value not in {"0", "1"}:
        raise ValueError("normalized mosaic linear-response availability is invalid")

    return NormalizedMosaic(
        manifest_path=path,
        sample_path=sample_path,
        width=width,
        height=height,
        cfa=cfa,
        black=black,  # type: ignore[arg-type]
        white=white,  # type: ignore[arg-type]
        linear_response=linear_response,  # type: ignore[arg-type]
        has_linear_response=has_linear_response_value == "1",
        orientation=_integer(fields["orientation"], "orientation", minimum=-2**31),
        bits_per_sample=bits_per_sample,
        as_shot_neutral=_float_tuple(  # type: ignore[arg-type]
            fields["as_shot_neutral"], 4, "as-shot neutral"
        ),
        camera_to_xyz_d50=_matrix(fields["camera_to_xyz_d50"], "D50 camera matrix"),
        xyz_to_camera_d65=_matrix(fields["xyz_to_camera_d65"], "D65 camera matrix"),
        camera_to_linear_srgb_d65=_matrix(
            fields["camera_to_linear_srgb_d65"], "linear sRGB camera matrix"
        ),
        pending_dng_opcode_bytes=_integer_tuple(  # type: ignore[arg-type]
            fields["pending_dng_opcode_bytes"], 3, "pending DNG opcode byte counts"
        ),
        provider_id=_decode_hex(fields["provider_id_hex"], "provider id"),
        provider_version=_decode_hex(fields["provider_version_hex"], "provider version"),
        sample_bytes=sample_bytes,
        sample_sha256=sha256_file(sample_path),
    )
