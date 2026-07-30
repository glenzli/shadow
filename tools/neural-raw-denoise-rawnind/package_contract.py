#!/usr/bin/env python3
"""Verify and extract the pinned public darktable RawNIND model package."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import shutil
from typing import BinaryIO
from zipfile import ZipFile


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]

UPSTREAM_REPOSITORY = "https://github.com/darktable-org/darktable-ai"
UPSTREAM_REVISION = "5454d7aa6d89a67054fd4a83343b09e69acaf76a"
UPSTREAM_RELEASE = "release-5.6.0"
TRAINING_REPOSITORY = "https://github.com/trougnouf/rawnind_jddc"
TRAINING_REVISION = "4d455aa8ada69214eafa6a91ac0b2e011cf9dcb7"

PACKAGE_SHA256 = "d71b5f1e727c85a359e6f74dca9e2016c9d8fc3e2f7ac3e9b347d80ceca969af"
PACKAGE_MEMBERS = {
    "rawdenoise-nind/config.json": (
        1_641,
        "b4b9d94d6f8bb7c401ae36972e3feb4371f8471b5a8708159a5615654033761c",
    ),
    "rawdenoise-nind/model_bayer.onnx": (
        31_056_425,
        "da27509dab6a2915da67e988acd86cf71f9d5bbc8d1aa0ed32933578a887b901",
    ),
    "rawdenoise-nind/model_linear.onnx": (
        31_053_823,
        "df957efadcc152c007d5d3b0917bdff9e41c0d4a0efe56584ef30b36393cd181",
    ),
}


@dataclass(frozen=True)
class PackageReceipt:
    package_path: Path
    package_sha256: str
    config: dict[str, object]
    member_sha256: dict[str, str]


def _sha256_stream(stream: BinaryIO) -> str:
    digest = hashlib.sha256()
    while chunk := stream.read(1024 * 1024):
        digest.update(chunk)
    return digest.hexdigest()


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return _sha256_stream(stream)


def _validate_config(config: object) -> dict[str, object]:
    if not isinstance(config, dict):
        raise ValueError("RawNIND config must be a JSON object")
    required_top_level = {
        "id": "rawdenoise-nind",
        "task": "rawdenoise",
        "arch": "utnet2",
        "backend": "onnx",
        "version": "1.0",
        "tiling": True,
        "coreml_format": "mlprogram",
    }
    for key, expected in required_top_level.items():
        if config.get(key) != expected:
            raise ValueError(
                f"RawNIND config mismatch for {key}: "
                f"expected {expected!r}, got {config.get(key)!r}"
            )

    attributes = config.get("attributes")
    if not isinstance(attributes, dict):
        raise ValueError("RawNIND config is missing attributes")
    if attributes.get("input_sizes") != [512]:
        raise ValueError("RawNIND package must declare one static 512-pixel tile")
    expected_bayer = {
        "input_kind": "bayer_v1",
        "bayer_orientation": "force_rggb",
        "edge_pad": "mirror_cropped",
        "wb_norm": "none",
        "output_scale": "match_gain",
    }
    if attributes.get("model_bayer") != expected_bayer:
        raise ValueError("RawNIND Bayer preprocessing contract changed")

    model_card = config.get("model_card")
    if not isinstance(model_card, dict):
        raise ValueError("RawNIND config is missing its model card")
    if model_card.get("license") != "GPL-3.0":
        raise ValueError("RawNIND model license changed")
    if model_card.get("training_data_license") != (
        "CC BY 4.0 / CC0 (per-image, Wikimedia Commons)"
    ):
        raise ValueError("RawNIND training-data license declaration changed")
    return config


def verify_package(package_path: Path) -> PackageReceipt:
    package_path = package_path.resolve()
    if not package_path.is_file():
        raise ValueError("RawNIND package does not exist")

    package_sha256 = sha256_file(package_path)
    if package_sha256 != PACKAGE_SHA256:
        raise ValueError(
            "RawNIND release-asset identity mismatch: "
            f"expected {PACKAGE_SHA256}, got {package_sha256}"
        )

    member_sha256: dict[str, str] = {}
    with ZipFile(package_path) as archive:
        entries = archive.infolist()
        names = [entry.filename for entry in entries]
        if len(names) != len(set(names)):
            raise ValueError("RawNIND package contains duplicate archive members")
        if set(names) != set(PACKAGE_MEMBERS):
            raise ValueError("RawNIND package member set changed")

        for entry in entries:
            expected_size, expected_sha256 = PACKAGE_MEMBERS[entry.filename]
            if entry.is_dir() or entry.file_size != expected_size:
                raise ValueError(
                    f"RawNIND member size mismatch: {entry.filename}"
                )
            unix_mode = (entry.external_attr >> 16) & 0o170000
            if unix_mode not in (0, 0o100000):
                raise ValueError(
                    f"RawNIND member is not a regular file: {entry.filename}"
                )
            with archive.open(entry, "r") as stream:
                actual_sha256 = _sha256_stream(stream)
            if actual_sha256 != expected_sha256:
                raise ValueError(
                    f"RawNIND member identity mismatch: {entry.filename}"
                )
            member_sha256[entry.filename] = actual_sha256

        with archive.open("rawdenoise-nind/config.json", "r") as stream:
            config = _validate_config(json.load(stream))

    return PackageReceipt(
        package_path=package_path,
        package_sha256=package_sha256,
        config=config,
        member_sha256=member_sha256,
    )


def path_is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
    except ValueError:
        return False
    return True


def validate_new_output_directory(path: Path) -> Path:
    resolved = path.resolve()
    if path_is_within(resolved, REPOSITORY_ROOT):
        raise ValueError("RawNIND model payload must stay outside the repository")
    if resolved.exists():
        raise ValueError("RawNIND output directory must not already exist")
    return resolved


def extract_verified_package(
    receipt: PackageReceipt,
    output_directory: Path,
) -> Path:
    output_directory = validate_new_output_directory(output_directory)
    output_directory.mkdir(parents=True)
    with ZipFile(receipt.package_path) as archive:
        for member_name in PACKAGE_MEMBERS:
            target = output_directory / member_name
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(member_name, "r") as source:
                with target.open("wb") as destination:
                    shutil.copyfileobj(source, destination, length=1024 * 1024)
    return output_directory / "rawdenoise-nind"
