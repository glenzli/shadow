"""Bounded linear-image I/O for the RAW highlight oracle.

The owner deliberately supports only the interchange surfaces emitted by the
registered oracle adapters: linear RGB PFM and RGB TIFF.  TIFF inputs are first
normalised with a caller-selected ``tiffcp`` executable, then read strip by
strip so an objective crop does not require materialising a full-resolution
three-channel image in memory.
"""

from __future__ import annotations

import dataclasses
import hashlib
import pathlib
import shutil
import struct
import subprocess
from typing import BinaryIO

import numpy as np


@dataclasses.dataclass(frozen=True)
class LinearImage:
    pixels: np.ndarray
    full_width: int
    full_height: int
    crop_xywh: tuple[int, int, int, int]
    source_format: str
    receipt: dict[str, object]


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _bounded_crop(
    width: int, height: int, crop_xywh: tuple[int, int, int, int] | None
) -> tuple[int, int, int, int]:
    if crop_xywh is None:
        return (0, 0, width, height)
    x, y, crop_width, crop_height = crop_xywh
    if (
        x < 0
        or y < 0
        or crop_width <= 0
        or crop_height <= 0
        or x + crop_width > width
        or y + crop_height > height
    ):
        raise ValueError(
            f"crop {crop_xywh!r} is outside the {width}x{height} image"
        )
    return crop_xywh


def _read_non_comment_line(stream: BinaryIO) -> bytes:
    while True:
        line = stream.readline()
        if not line:
            raise ValueError("unexpected end of PFM header")
        stripped = line.strip()
        if stripped and not stripped.startswith(b"#"):
            return stripped


def read_pfm(
    path: pathlib.Path,
    crop_xywh: tuple[int, int, int, int] | None = None,
    orientation: str = "bottom-up",
) -> LinearImage:
    with path.open("rb") as stream:
        magic = _read_non_comment_line(stream)
        if magic != b"PF":
            raise ValueError("only three-channel RGB PFM is accepted")
        dimensions = _read_non_comment_line(stream).split()
        if len(dimensions) != 2:
            raise ValueError("invalid PFM dimensions")
        width, height = (int(value) for value in dimensions)
        scale = float(_read_non_comment_line(stream))
        data_offset = stream.tell()
    if width <= 0 or height <= 0 or scale == 0.0:
        raise ValueError("invalid PFM header values")
    expected = data_offset + width * height * 3 * 4
    if path.stat().st_size != expected:
        raise ValueError("PFM payload size does not match its header")
    dtype = np.dtype("<f4" if scale < 0.0 else ">f4")
    mapped = np.memmap(
        path, dtype=dtype, mode="r", offset=data_offset, shape=(height, width, 3)
    )
    if orientation == "bottom-up":
        top_down = mapped[::-1]
    elif orientation == "top-down":
        top_down = mapped
    else:
        raise ValueError("PFM orientation must be 'bottom-up' or 'top-down'")
    x, y, crop_width, crop_height = _bounded_crop(width, height, crop_xywh)
    pixels = np.asarray(top_down[y : y + crop_height, x : x + crop_width], dtype=np.float32)
    if abs(scale) != 1.0:
        pixels = pixels * abs(scale)
    return LinearImage(
        pixels=pixels,
        full_width=width,
        full_height=height,
        crop_xywh=(x, y, crop_width, crop_height),
        source_format="pfm-rgb-f32-linear",
        receipt={
            "source_sha256": sha256_file(path),
            "source_size_bytes": path.stat().st_size,
            "pfm_scale": scale,
            "pfm_file_row_orientation": orientation,
        },
    )


_TIFF_TYPE_SIZE = {
    1: 1,  # BYTE
    2: 1,  # ASCII
    3: 2,  # SHORT
    4: 4,  # LONG
    5: 8,  # RATIONAL
    11: 4,  # FLOAT
    12: 8,  # DOUBLE
}


def _unpack_tiff_values(
    stream: BinaryIO,
    endian: str,
    field_type: int,
    count: int,
    inline_or_offset: bytes,
) -> tuple[int | float, ...]:
    item_size = _TIFF_TYPE_SIZE.get(field_type)
    if item_size is None:
        raise ValueError(f"unsupported TIFF field type {field_type}")
    byte_count = item_size * count
    if byte_count <= 4:
        raw = inline_or_offset[:byte_count]
    else:
        offset = struct.unpack(endian + "I", inline_or_offset)[0]
        position = stream.tell()
        stream.seek(offset)
        raw = stream.read(byte_count)
        stream.seek(position)
        if len(raw) != byte_count:
            raise ValueError("truncated TIFF field")
    formats = {1: "B", 3: "H", 4: "I", 11: "f", 12: "d"}
    if field_type not in formats:
        raise ValueError(f"TIFF field type {field_type} is not numeric")
    return struct.unpack(endian + formats[field_type] * count, raw)


def _read_tiff_directory(path: pathlib.Path) -> tuple[str, dict[int, tuple[int | float, ...]]]:
    with path.open("rb") as stream:
        byte_order = stream.read(2)
        endian = {b"II": "<", b"MM": ">"}.get(byte_order)
        if endian is None or struct.unpack(endian + "H", stream.read(2))[0] != 42:
            raise ValueError("only classic TIFF is accepted")
        first_ifd = struct.unpack(endian + "I", stream.read(4))[0]
        stream.seek(first_ifd)
        entry_count = struct.unpack(endian + "H", stream.read(2))[0]
        tags: dict[int, tuple[int | float, ...]] = {}
        for _ in range(entry_count):
            entry = stream.read(12)
            if len(entry) != 12:
                raise ValueError("truncated TIFF directory")
            tag, field_type, count = struct.unpack(endian + "HHI", entry[:8])
            if tag in {256, 257, 258, 259, 262, 273, 277, 278, 279, 284, 339}:
                tags[tag] = _unpack_tiff_values(
                    stream, endian, field_type, count, entry[8:]
                )
    return endian, tags


def _single_int(tags: dict[int, tuple[int | float, ...]], tag: int, default: int | None = None) -> int:
    values = tags.get(tag)
    if values is None:
        if default is None:
            raise ValueError(f"required TIFF tag {tag} is missing")
        return default
    if len(values) != 1:
        raise ValueError(f"TIFF tag {tag} must contain one value")
    return int(values[0])


def _executable_receipt(path: pathlib.Path) -> dict[str, object]:
    return {
        "resolved_path": str(path.resolve()),
        "size_bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def normalize_tiff(
    source: pathlib.Path,
    working_directory: pathlib.Path,
    tiffcp: str | None = None,
) -> tuple[pathlib.Path, dict[str, object]]:
    executable_name = tiffcp or "tiffcp"
    resolved = shutil.which(executable_name)
    if resolved is None:
        raise ValueError("tiffcp is required to normalize TIFF inputs")
    executable = pathlib.Path(resolved).resolve()
    working_directory.mkdir(parents=True, exist_ok=True)
    output = working_directory / f"{sha256_file(source)[:16]}.contiguous-uncompressed.tif"
    argv = [str(executable), "-c", "none", "-p", "contig", str(source), str(output)]
    completed = subprocess.run(argv, check=False, capture_output=True, text=True)
    if completed.returncode != 0:
        raise ValueError(f"tiffcp failed: {completed.stderr.strip()}")
    return output, {
        "executable": _executable_receipt(executable),
        "arguments": ["-c", "none", "-p", "contig", "<SOURCE>", "<NORMALIZED>"],
        "normalized_sha256": sha256_file(output),
        "normalized_size_bytes": output.stat().st_size,
        "stderr": completed.stderr.strip(),
    }


def read_tiff(
    path: pathlib.Path,
    working_directory: pathlib.Path,
    crop_xywh: tuple[int, int, int, int] | None = None,
    tiffcp: str | None = None,
) -> LinearImage:
    normalized, conversion = normalize_tiff(path, working_directory, tiffcp)
    endian, tags = _read_tiff_directory(normalized)
    width = _single_int(tags, 256)
    height = _single_int(tags, 257)
    bits = tuple(int(value) for value in tags.get(258, ()))
    samples = _single_int(tags, 277)
    sample_format = tuple(int(value) for value in tags.get(339, (1,) * samples))
    if (
        _single_int(tags, 259) != 1
        or _single_int(tags, 262) != 2
        or _single_int(tags, 284, 1) != 1
        or samples < 3
        or len(set(bits)) != 1
        or len(set(sample_format)) != 1
    ):
        raise ValueError("TIFF must be uncompressed contiguous RGB with uniform samples")
    bit_depth = bits[0]
    sample_kind = sample_format[0]
    if (bit_depth, sample_kind) == (16, 1):
        dtype = np.dtype(endian + "u2")
        divisor = 65535.0
    elif (bit_depth, sample_kind) == (32, 3):
        dtype = np.dtype(endian + "f4")
        divisor = 1.0
    else:
        raise ValueError(
            f"unsupported TIFF sample representation bits={bit_depth} format={sample_kind}"
        )
    offsets = tuple(int(value) for value in tags.get(273, ()))
    byte_counts = tuple(int(value) for value in tags.get(279, ()))
    if not offsets or len(offsets) != len(byte_counts):
        raise ValueError("TIFF strip tables are missing or inconsistent")
    rows_per_strip = _single_int(tags, 278, height)
    x, y, crop_width, crop_height = _bounded_crop(width, height, crop_xywh)
    pixels = np.empty((crop_height, crop_width, 3), dtype=np.float32)
    covered = np.zeros(crop_height, dtype=bool)
    bytes_per_sample = bit_depth // 8
    with normalized.open("rb") as stream:
        for strip_index, (offset, byte_count) in enumerate(zip(offsets, byte_counts)):
            strip_y = strip_index * rows_per_strip
            strip_rows = min(rows_per_strip, height - strip_y)
            overlap_y0 = max(y, strip_y)
            overlap_y1 = min(y + crop_height, strip_y + strip_rows)
            if overlap_y0 >= overlap_y1:
                continue
            expected = strip_rows * width * samples * bytes_per_sample
            if byte_count < expected:
                raise ValueError("TIFF strip is shorter than its declared rows")
            stream.seek(offset)
            raw = stream.read(expected)
            if len(raw) != expected:
                raise ValueError("truncated TIFF strip")
            strip = np.frombuffer(raw, dtype=dtype).reshape(strip_rows, width, samples)
            source_y0 = overlap_y0 - strip_y
            source_y1 = overlap_y1 - strip_y
            target_y0 = overlap_y0 - y
            target_y1 = overlap_y1 - y
            pixels[target_y0:target_y1] = (
                strip[source_y0:source_y1, x : x + crop_width, :3].astype(np.float32)
                / divisor
            )
            covered[target_y0:target_y1] = True
    if not bool(np.all(covered)):
        raise ValueError("TIFF strips did not cover the requested crop")
    return LinearImage(
        pixels=pixels,
        full_width=width,
        full_height=height,
        crop_xywh=(x, y, crop_width, crop_height),
        source_format=f"tiff-rgb-{bit_depth}-sample-format-{sample_kind}",
        receipt={
            "source_sha256": sha256_file(path),
            "source_size_bytes": path.stat().st_size,
            "conversion": conversion,
        },
    )


def read_linear_image(
    path: pathlib.Path,
    working_directory: pathlib.Path,
    crop_xywh: tuple[int, int, int, int] | None = None,
    tiffcp: str | None = None,
    pfm_orientation: str = "bottom-up",
) -> LinearImage:
    suffix = path.suffix.lower()
    if suffix == ".pfm":
        return read_pfm(path, crop_xywh, pfm_orientation)
    if suffix in {".tif", ".tiff"}:
        return read_tiff(path, working_directory, crop_xywh, tiffcp)
    raise ValueError(f"unsupported linear image extension: {path.suffix}")


def linear_image_dimensions(
    path: pathlib.Path,
    working_directory: pathlib.Path,
    tiffcp: str | None = None,
) -> tuple[int, int]:
    suffix = path.suffix.lower()
    if suffix == ".pfm":
        with path.open("rb") as stream:
            if _read_non_comment_line(stream) != b"PF":
                raise ValueError("only three-channel RGB PFM is accepted")
            dimensions = _read_non_comment_line(stream).split()
        if len(dimensions) != 2:
            raise ValueError("invalid PFM dimensions")
        return int(dimensions[0]), int(dimensions[1])
    if suffix in {".tif", ".tiff"}:
        normalized, _ = normalize_tiff(path, working_directory, tiffcp)
        _, tags = _read_tiff_directory(normalized)
        return _single_int(tags, 256), _single_int(tags, 257)
    raise ValueError(f"unsupported linear image extension: {path.suffix}")


def write_pfm(path: pathlib.Path, pixels: np.ndarray) -> None:
    array = np.asarray(pixels, dtype=np.float32)
    if array.ndim != 3 or array.shape[2] != 3:
        raise ValueError("PFM output must have shape HxWx3")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as stream:
        stream.write(f"PF\n{array.shape[1]} {array.shape[0]}\n-1.0\n".encode("ascii"))
        stream.write(np.ascontiguousarray(array[::-1], dtype="<f4").tobytes())


def write_pgm(path: pathlib.Path, mask: np.ndarray) -> None:
    array = np.asarray(mask, dtype=bool)
    if array.ndim != 2:
        raise ValueError("PGM mask must have shape HxW")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as stream:
        stream.write(f"P5\n{array.shape[1]} {array.shape[0]}\n255\n".encode("ascii"))
        stream.write(np.where(array, 255, 0).astype(np.uint8).tobytes())
