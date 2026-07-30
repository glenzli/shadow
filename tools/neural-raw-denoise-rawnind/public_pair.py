#!/usr/bin/env python3
"""Identity contract for one small, official RawNIND noisy/clean pair."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
from pathlib import Path
import sys
from typing import BinaryIO


DATASET_DOI = "doi:10.14428/DVN/DEQCIM"
DATASET_URL = "https://doi.org/10.14428/DVN/DEQCIM"
DATASET_LICENSE = "CC-BY-SA-4.0"


@dataclass(frozen=True)
class PublicRawFile:
    role: str
    dataverse_file_id: int
    persistent_id: str
    filename: str
    size_bytes: int
    md5: str

    @property
    def download_url(self) -> str:
        return (
            "https://dataverse.uclouvain.be/api/access/datafile/"
            f"{self.dataverse_file_id}"
        )


@dataclass(frozen=True)
class VerifiedPublicRaw:
    role: str
    path: str
    dataverse_file_id: int
    persistent_id: str
    filename: str
    size_bytes: int
    official_md5: str
    sha256: str


GROUND_TRUTH = PublicRawFile(
    role="ground_truth",
    dataverse_file_id=28_815,
    persistent_id="doi:10.14428/DVN/DEQCIM/YVRECS",
    filename=(
        "Bayer_7D-1_GT_ISO100_"
        "sha1=22aef4a5b4038e241082741117827f364ce6a5ac.cr2"
    ),
    size_bytes=23_170_147,
    md5="2bb12d6d5fa67af285a736b8f5dfea90",
)

NOISY = PublicRawFile(
    role="noisy",
    dataverse_file_id=28_663,
    persistent_id="doi:10.14428/DVN/DEQCIM/51R7S3",
    filename=(
        "Bayer_7D-1_ISO12800_"
        "sha1=5322313f9650a7b94028d69567c1f6c66bac8765.cr2"
    ),
    size_bytes=33_639_891,
    md5="47b2c25f22f44c1d26fd2a55ffd87931",
)


def _hashes(stream: BinaryIO) -> tuple[str, str]:
    md5 = hashlib.md5(usedforsecurity=False)
    sha256 = hashlib.sha256()
    while chunk := stream.read(1024 * 1024):
        md5.update(chunk)
        sha256.update(chunk)
    return md5.hexdigest(), sha256.hexdigest()


def verify_public_raw(path: Path, expected: PublicRawFile) -> VerifiedPublicRaw:
    resolved = path.resolve()
    if not resolved.is_file():
        raise ValueError(f"RawNIND {expected.role} file does not exist")
    size = resolved.stat().st_size
    if size != expected.size_bytes:
        raise ValueError(
            f"RawNIND {expected.role} size mismatch: "
            f"expected {expected.size_bytes}, got {size}"
        )
    with resolved.open("rb") as stream:
        actual_md5, sha256 = _hashes(stream)
    if actual_md5 != expected.md5:
        raise ValueError(
            f"RawNIND {expected.role} official MD5 identity mismatch"
        )
    return VerifiedPublicRaw(
        role=expected.role,
        path=str(resolved),
        dataverse_file_id=expected.dataverse_file_id,
        persistent_id=expected.persistent_id,
        filename=expected.filename,
        size_bytes=size,
        official_md5=actual_md5,
        sha256=sha256,
    )


def assemble_download_parts(
    parts: list[Path],
    output: Path,
    expected: PublicRawFile,
) -> VerifiedPublicRaw:
    """Assemble ordered HTTP ranges into a new file and verify before admission."""

    if not parts:
        raise ValueError("RawNIND range assembly requires at least one part")
    resolved_output = output.resolve()
    temporary = resolved_output.with_name(resolved_output.name + ".partial")
    if resolved_output.exists() or temporary.exists():
        raise ValueError("RawNIND range-assembly output must not already exist")
    resolved_output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with temporary.open("xb") as destination:
            for part in parts:
                with part.resolve().open("rb") as source:
                    while chunk := source.read(1024 * 1024):
                        destination.write(chunk)
        receipt = verify_public_raw(temporary, expected)
        temporary.replace(resolved_output)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    return VerifiedPublicRaw(
        role=receipt.role,
        path=str(resolved_output),
        dataverse_file_id=receipt.dataverse_file_id,
        persistent_id=receipt.persistent_id,
        filename=receipt.filename,
        size_bytes=receipt.size_bytes,
        official_md5=receipt.official_md5,
        sha256=receipt.sha256,
    )


def as_json(value: object) -> object:
    if hasattr(value, "__dataclass_fields__"):
        return asdict(value)
    raise TypeError(f"cannot serialize {type(value)!r}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=("noisy", "ground-truth"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("parts", type=Path, nargs="+")
    return parser.parse_args()


if __name__ == "__main__":
    try:
        arguments = parse_arguments()
        expected = NOISY if arguments.role == "noisy" else GROUND_TRUTH
        assembled = assemble_download_parts(
            arguments.parts,
            arguments.output,
            expected,
        )
        print(asdict(assembled))
    except (OSError, ValueError) as error:
        print(f"RawNIND pair assembly failed: {error}", file=sys.stderr)
        sys.exit(2)
