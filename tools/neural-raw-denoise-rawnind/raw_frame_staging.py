#!/usr/bin/env python3
"""Strict reader for Shadow's short-lived provider-neutral RawFrame staging."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path

import numpy as np


SCHEMA = "shadow-raw-frame-staging-v1"
MAX_MANIFEST_BYTES = 16 * 1024
MAX_DIMENSION = 100_000
FIELDS = {
    "width",
    "height",
    "cfa",
    "black",
    "white",
    "provider_id_hex",
    "provider_version_hex",
    "sample_bytes",
}


@dataclass(frozen=True)
class StagedRawFrame:
    mosaic: np.ndarray
    cfa: str
    black_levels: list[float]
    white_levels: list[float]
    provider_id: str
    provider_version: str
    samples_sha256: str


def _decode_identity(value: str, label: str) -> str:
    if value == "-":
        return ""
    if len(value) % 2 or any(
        character not in "0123456789abcdef" for character in value
    ):
        raise ValueError(f"staged RAW frame {label} is not lowercase hex")
    try:
        decoded = bytes.fromhex(value).decode("utf-8")
    except UnicodeError as error:
        raise ValueError(
            f"staged RAW frame {label} is not UTF-8"
        ) from error
    if not decoded:
        raise ValueError(f"staged RAW frame {label} is empty")
    return decoded


def _levels(value: str, label: str) -> list[float]:
    fields = value.split(",")
    if len(fields) != 4:
        raise ValueError(f"staged RAW frame {label} must have four sites")
    try:
        levels = [float(int(field)) for field in fields]
    except ValueError as error:
        raise ValueError(
            f"staged RAW frame {label} must contain unsigned integers"
        ) from error
    if any(level < 0 or level > 65_535 for level in levels):
        raise ValueError(f"staged RAW frame {label} is out of range")
    return levels


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def load(manifest_path: Path) -> StagedRawFrame:
    manifest = manifest_path.resolve(strict=True)
    if not manifest.is_file():
        raise ValueError("staged RAW frame manifest does not exist")
    size = manifest.stat().st_size
    if size == 0 or size > MAX_MANIFEST_BYTES:
        raise ValueError("staged RAW frame manifest has an invalid size")
    text = manifest.read_text(encoding="utf-8")
    if not text.endswith("\n") or "\n" in text[:-1]:
        raise ValueError("staged RAW frame manifest must be one complete line")
    tokens = text.split()
    if not tokens or tokens[0] != SCHEMA:
        raise ValueError("staged RAW frame schema is unsupported")
    pairs: dict[str, str] = {}
    for token in tokens[1:]:
        key, separator, value = token.partition("=")
        if not separator or not key or not value or key in pairs:
            raise ValueError("staged RAW frame manifest fields are malformed")
        pairs[key] = value
    if set(pairs) != FIELDS:
        raise ValueError("staged RAW frame manifest fields changed")

    try:
        width = int(pairs["width"])
        height = int(pairs["height"])
        declared_sample_bytes = int(pairs["sample_bytes"])
    except ValueError as error:
        raise ValueError(
            "staged RAW frame dimensions are not integers"
        ) from error
    if (
        width <= 0
        or height <= 0
        or width > MAX_DIMENSION
        or height > MAX_DIMENSION
    ):
        raise ValueError("staged RAW frame dimensions are out of range")
    expected_sample_bytes = width * height * 2
    if declared_sample_bytes != expected_sample_bytes:
        raise ValueError("staged RAW frame sample byte count changed")

    cfa = pairs["cfa"]
    if len(cfa) != 4 or sorted(cfa) != ["B", "G", "G", "R"]:
        raise ValueError("staged RAW frame is not a 2x2 Bayer source")
    black_levels = _levels(pairs["black"], "black levels")
    white_levels = _levels(pairs["white"], "white levels")
    if any(
        black >= white
        for black, white in zip(black_levels, white_levels, strict=True)
    ):
        raise ValueError("staged RAW frame black/white levels are invalid")
    if len(set(white_levels)) != 1:
        raise ValueError(
            "RawNIND staging currently requires one shared sensor white level"
        )

    provider_id = _decode_identity(pairs["provider_id_hex"], "provider id")
    provider_version = _decode_identity(
        pairs["provider_version_hex"],
        "provider version",
    )
    if bool(provider_id) != bool(provider_version):
        raise ValueError("staged RAW frame provider identity is incomplete")

    sample_path = Path(f"{manifest}.u16le")
    if (
        not sample_path.is_file()
        or sample_path.stat().st_size != expected_sample_bytes
    ):
        raise ValueError("staged RAW frame sample payload is incomplete")
    samples_sha256 = _sha256_file(sample_path)
    mosaic = np.memmap(
        sample_path,
        mode="r",
        dtype="<u2",
        shape=(height, width),
    )
    return StagedRawFrame(
        mosaic=mosaic,
        cfa=cfa,
        black_levels=black_levels,
        white_levels=white_levels,
        provider_id=provider_id or "shadow.raw-frame",
        provider_version=provider_version or SCHEMA,
        samples_sha256=samples_sha256,
    )
