#!/usr/bin/env python3
"""Encode and verify a lossless research DNG from Shadow RawFrame staging.

The DNG's uncompressed CFA strip is byte-identical to the active uint16 little-
endian staging plane.  DNGPrivateData carries Shadow's exact four-site descriptor
because standard DNG WhiteLevel cannot represent a distinct value per CFA site.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import math
import os
import pathlib
import struct
import subprocess
import sys
import uuid
from collections.abc import Sequence
from fractions import Fraction

from normalized_mosaic import NormalizedMosaic, read_staging


RESEARCH_DNG_SCHEMA = "shadow.raw-highlight-research-dng.v1"
WRITER_VERSION = "20260825.1"
PRIVATE_MAGIC = b"Shadow\x00" + RESEARCH_DNG_SCHEMA.encode("ascii") + b"\x00"

TYPE_BYTE = 1
TYPE_ASCII = 2
TYPE_SHORT = 3
TYPE_LONG = 4
TYPE_RATIONAL = 5
TYPE_SRATIONAL = 10
TYPE_SIZES = {
    TYPE_BYTE: 1,
    TYPE_ASCII: 1,
    TYPE_SHORT: 2,
    TYPE_LONG: 4,
    TYPE_RATIONAL: 8,
    TYPE_SRATIONAL: 8,
}


@dataclasses.dataclass(frozen=True)
class IfdValue:
    tag: int
    field_type: int
    count: int
    data: bytes


@dataclasses.dataclass(frozen=True)
class ResearchDngReceipt:
    output_path: pathlib.Path
    width: int
    height: int
    sample_bytes: int
    sample_sha256: str
    dng_sha256: str
    standard_white_level: int
    standard_white_projection: str
    standard_neutral_projection: str
    calibration_projection: str


@dataclasses.dataclass(frozen=True)
class ParsedTiff:
    path: pathlib.Path
    endian: str
    entries: dict[int, tuple[int, int, bytes]]

    def unsigned_values(self, tag: int) -> tuple[int, ...]:
        field_type, count, data = self.entries[tag]
        formats = {TYPE_BYTE: "B", TYPE_SHORT: "H", TYPE_LONG: "I"}
        if field_type not in formats:
            raise ValueError(f"TIFF tag {tag} is not an unsigned integer field")
        return struct.unpack(self.endian + formats[field_type] * count, data)

    def bytes_value(self, tag: int) -> bytes:
        field_type, _count, data = self.entries[tag]
        if field_type not in {TYPE_BYTE, TYPE_ASCII}:
            raise ValueError(f"TIFF tag {tag} is not a byte field")
        return data


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _fraction(value: float, *, signed: bool) -> tuple[int, int]:
    if not math.isfinite(value):
        raise ValueError("DNG rational value is not finite")
    fraction = Fraction(value).limit_denominator(1_000_000)
    minimum = -(2**31) if signed else 0
    maximum = 2**31 - 1 if signed else 2**32 - 1
    if not minimum <= fraction.numerator <= maximum or not 1 <= fraction.denominator <= maximum:
        raise ValueError("DNG rational value is outside classic TIFF bounds")
    return fraction.numerator, fraction.denominator


def _value(tag: int, field_type: int, values: Sequence[int] | bytes | str) -> IfdValue:
    if field_type == TYPE_ASCII:
        data = values.encode("ascii") + b"\x00" if isinstance(values, str) else bytes(values)
        if not data.endswith(b"\x00"):
            data += b"\x00"
        return IfdValue(tag, field_type, len(data), data)
    if isinstance(values, str):
        raise TypeError("non-ASCII TIFF values cannot be strings")
    if isinstance(values, bytes):
        return IfdValue(tag, field_type, len(values), values)
    formats = {TYPE_BYTE: "B", TYPE_SHORT: "H", TYPE_LONG: "I"}
    if field_type not in formats:
        raise ValueError("unsupported integer TIFF field type")
    data = struct.pack("<" + formats[field_type] * len(values), *values)
    return IfdValue(tag, field_type, len(values), data)


def _rational_value(tag: int, values: Sequence[float], *, signed: bool) -> IfdValue:
    pairs = [_fraction(value, signed=signed) for value in values]
    code = "i" if signed else "I"
    data = b"".join(struct.pack("<" + code * 2, numerator, denominator) for numerator, denominator in pairs)
    return IfdValue(tag, TYPE_SRATIONAL if signed else TYPE_RATIONAL, len(pairs), data)


def _inverse_3x3(matrix: Sequence[float]) -> tuple[float, ...]:
    a, b, c, d, e, f, g, h, i = matrix
    determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if not math.isfinite(determinant) or abs(determinant) < 1e-12:
        raise ValueError("camera-to-XYZ matrix is singular")
    return (
        (e * i - f * h) / determinant,
        (c * h - b * i) / determinant,
        (b * f - c * e) / determinant,
        (f * g - d * i) / determinant,
        (a * i - c * g) / determinant,
        (c * d - a * f) / determinant,
        (d * h - e * g) / determinant,
        (b * g - a * h) / determinant,
        (a * e - b * d) / determinant,
    )


def _multiply_3x3(left: Sequence[float], right: Sequence[float]) -> tuple[float, ...]:
    return tuple(
        sum(left[row * 3 + inner] * right[inner * 3 + column] for inner in range(3))
        for row in range(3)
        for column in range(3)
    )


def _orientation(libraw_orientation: int) -> int:
    mapping = {0: 1, 3: 3, 5: 8, 6: 6}
    if libraw_orientation not in mapping:
        raise ValueError(f"unsupported LibRaw orientation: {libraw_orientation}")
    return mapping[libraw_orientation]


def _camera_matrix(mosaic: NormalizedMosaic) -> tuple[tuple[float, ...], int, str]:
    if mosaic.xyz_to_camera_d65 is not None:
        return mosaic.xyz_to_camera_d65, 21, "xyz-to-camera-d65-exact"
    if mosaic.camera_to_xyz_d50 is not None:
        return _inverse_3x3(mosaic.camera_to_xyz_d50), 23, "inverse-camera-to-xyz-d50"
    if mosaic.camera_to_linear_srgb_d65 is not None:
        linear_srgb_to_xyz_d65 = (
            0.4124564,
            0.3575761,
            0.1804375,
            0.2126729,
            0.7151522,
            0.0721750,
            0.0193339,
            0.1191920,
            0.9503041,
        )
        camera_to_xyz_d65 = _multiply_3x3(
            linear_srgb_to_xyz_d65, mosaic.camera_to_linear_srgb_d65
        )
        return (
            _inverse_3x3(camera_to_xyz_d65),
            21,
            "derived-from-camera-to-linear-srgb-d65",
        )
    return (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0), 21, "identity-fallback"


def _private_payload(mosaic: NormalizedMosaic) -> bytes:
    document = {
        "schema": RESEARCH_DNG_SCHEMA,
        "writer_version": WRITER_VERSION,
        "normalized_mosaic": mosaic.metadata(),
        "pending_opcodes_applied": False,
        "geometry": "active-area-only-unoriented",
    }
    return PRIVATE_MAGIC + json.dumps(
        document, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def _ifd_values(mosaic: NormalizedMosaic, strip_offset: int) -> tuple[list[IfdValue], dict[str, object]]:
    colour_codes = {"R": 0, "G": 1, "B": 2}
    standard_white = min(mosaic.white)
    white_projection = "exact" if len(set(mosaic.white)) == 1 else "conservative-minimum"
    neutral = mosaic.colour_neutral()
    green_values = [mosaic.as_shot_neutral[index] for index, colour in enumerate(mosaic.cfa) if colour == "G"]
    neutral_projection = "exact" if len(set(green_values)) <= 1 else "green-site-average"
    matrix, illuminant, calibration_projection = _camera_matrix(mosaic)
    model = "Shadow normalized RawFrame"
    software = f"Shadow RAW oracle {WRITER_VERSION}"
    values = [
        _value(254, TYPE_LONG, (0,)),
        _value(256, TYPE_LONG, (mosaic.width,)),
        _value(257, TYPE_LONG, (mosaic.height,)),
        _value(258, TYPE_SHORT, (16,)),
        _value(259, TYPE_SHORT, (1,)),
        _value(262, TYPE_SHORT, (32803,)),
        _value(273, TYPE_LONG, (strip_offset,)),
        _value(274, TYPE_SHORT, (_orientation(mosaic.orientation),)),
        _value(277, TYPE_SHORT, (1,)),
        _value(278, TYPE_LONG, (mosaic.height,)),
        _value(279, TYPE_LONG, (mosaic.sample_bytes,)),
        _value(284, TYPE_SHORT, (1,)),
        _value(305, TYPE_ASCII, software),
        _value(33421, TYPE_SHORT, (2, 2)),
        _value(33422, TYPE_BYTE, bytes(colour_codes[colour] for colour in mosaic.cfa)),
        _value(50706, TYPE_BYTE, bytes((1, 4, 0, 0))),
        _value(50707, TYPE_BYTE, bytes((1, 1, 0, 0))),
        _value(50708, TYPE_ASCII, model),
        _value(50710, TYPE_BYTE, bytes((0, 1, 2))),
        _value(50711, TYPE_SHORT, (1,)),
        _value(50713, TYPE_SHORT, (2, 2)),
        _rational_value(50714, mosaic.black, signed=False),
        _value(50717, TYPE_LONG, (standard_white,)),
        _rational_value(50718, (1.0, 1.0), signed=False),
        _value(50719, TYPE_LONG, (0, 0)),
        _value(50720, TYPE_LONG, (mosaic.width, mosaic.height)),
        _rational_value(50721, matrix, signed=True),
        _rational_value(50728, neutral, signed=False),
        _value(50740, TYPE_BYTE, _private_payload(mosaic)),
        _value(50778, TYPE_SHORT, (illuminant,)),
        _value(50829, TYPE_LONG, (0, 0, mosaic.height, mosaic.width)),
    ]
    return sorted(values, key=lambda item: item.tag), {
        "standard_white_level": standard_white,
        "standard_white_projection": white_projection,
        "standard_neutral_projection": neutral_projection,
        "calibration_projection": calibration_projection,
    }


def _layout_ifd(values: Sequence[IfdValue]) -> tuple[bytes, int]:
    ifd_size = 2 + len(values) * 12 + 4
    extra_offset = 8 + ifd_size
    extra = bytearray()
    encoded_entries = bytearray()
    for value in values:
        expected = TYPE_SIZES[value.field_type] * value.count
        if len(value.data) != expected:
            raise ValueError(f"TIFF tag {value.tag} has inconsistent payload length")
        encoded_entries.extend(struct.pack("<HHI", value.tag, value.field_type, value.count))
        if len(value.data) <= 4:
            encoded_entries.extend(value.data.ljust(4, b"\x00"))
        else:
            while (extra_offset + len(extra)) % 4:
                extra.append(0)
            encoded_entries.extend(struct.pack("<I", extra_offset + len(extra)))
            extra.extend(value.data)
    while (extra_offset + len(extra)) % 4:
        extra.append(0)
    ifd = struct.pack("<H", len(values)) + bytes(encoded_entries) + struct.pack("<I", 0)
    return ifd + bytes(extra), extra_offset + len(extra)


def write_research_dng(mosaic: NormalizedMosaic, output_path: pathlib.Path) -> ResearchDngReceipt:
    output = output_path.expanduser().resolve(strict=False)
    if output.exists():
        raise ValueError(f"research DNG destination already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    provisional, projection = _ifd_values(mosaic, 0)
    _layout, strip_offset = _layout_ifd(provisional)
    values, projection = _ifd_values(mosaic, strip_offset)
    ifd, confirmed_strip_offset = _layout_ifd(values)
    if confirmed_strip_offset != strip_offset:
        raise ValueError("research DNG strip layout did not converge")
    temporary = output.with_name(f"{output.name}.partial-{uuid.uuid4().hex}")
    try:
        with temporary.open("xb") as destination, mosaic.sample_path.open("rb") as samples:
            destination.write(b"II" + struct.pack("<HI", 42, 8))
            destination.write(ifd)
            copied = 0
            for chunk in iter(lambda: samples.read(1024 * 1024), b""):
                destination.write(chunk)
                copied += len(chunk)
            destination.flush()
            os.fsync(destination.fileno())
        if copied != mosaic.sample_bytes:
            raise ValueError("research DNG source sample payload changed during encoding")
        temporary.replace(output)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise
    return ResearchDngReceipt(
        output_path=output,
        width=mosaic.width,
        height=mosaic.height,
        sample_bytes=mosaic.sample_bytes,
        sample_sha256=mosaic.sample_sha256,
        dng_sha256=sha256_file(output),
        standard_white_level=int(projection["standard_white_level"]),
        standard_white_projection=str(projection["standard_white_projection"]),
        standard_neutral_projection=str(projection["standard_neutral_projection"]),
        calibration_projection=str(projection["calibration_projection"]),
    )


def parse_tiff(path: pathlib.Path) -> ParsedTiff:
    resolved = path.expanduser().resolve(strict=False)
    with resolved.open("rb") as stream:
        header = stream.read(8)
        if len(header) != 8 or header[:2] not in {b"II", b"MM"}:
            raise ValueError("TIFF byte order is unsupported")
        endian = "<" if header[:2] == b"II" else ">"
        magic, ifd_offset = struct.unpack(endian + "HI", header[2:])
        if magic != 42:
            raise ValueError("classic TIFF magic is missing")
        stream.seek(ifd_offset)
        count_bytes = stream.read(2)
        if len(count_bytes) != 2:
            raise ValueError("TIFF IFD is incomplete")
        count = struct.unpack(endian + "H", count_bytes)[0]
        if count == 0 or count > 4096:
            raise ValueError("TIFF IFD entry count is invalid")
        entries: dict[int, tuple[int, int, bytes]] = {}
        for _ in range(count):
            encoded = stream.read(12)
            if len(encoded) != 12:
                raise ValueError("TIFF IFD entry is incomplete")
            tag, field_type, value_count = struct.unpack(endian + "HHI", encoded[:8])
            if field_type not in TYPE_SIZES:
                continue
            byte_count = TYPE_SIZES[field_type] * value_count
            if byte_count <= 4:
                data = encoded[8 : 8 + byte_count]
            else:
                value_offset = struct.unpack(endian + "I", encoded[8:])[0]
                position = stream.tell()
                stream.seek(value_offset)
                data = stream.read(byte_count)
                stream.seek(position)
                if len(data) != byte_count:
                    raise ValueError(f"TIFF tag {tag} payload is incomplete")
            entries[tag] = (field_type, value_count, data)
    return ParsedTiff(resolved, endian, entries)


def _private_document(parsed: ParsedTiff) -> dict[str, object]:
    payload = parsed.bytes_value(50740)
    if not payload.startswith(PRIVATE_MAGIC):
        raise ValueError("research DNG private descriptor magic is missing")
    return json.loads(payload[len(PRIVATE_MAGIC) :].decode("utf-8"))


def extract_u16le_strip(parsed: ParsedTiff) -> bytes:
    required = {256, 257, 258, 259, 273, 277, 279}
    if not required.issubset(parsed.entries):
        raise ValueError("TIFF pixel layout tags are incomplete")
    bits = parsed.unsigned_values(258)
    compression = parsed.unsigned_values(259)
    samples_per_pixel = parsed.unsigned_values(277)
    if bits != (16,) or compression != (1,) or samples_per_pixel != (1,):
        raise ValueError("TIFF is not an uncompressed single-channel uint16 plane")
    offsets = parsed.unsigned_values(273)
    counts = parsed.unsigned_values(279)
    if len(offsets) != len(counts) or not offsets:
        raise ValueError("TIFF strip table is invalid")
    chunks: list[bytes] = []
    with parsed.path.open("rb") as stream:
        for offset, count in zip(offsets, counts, strict=True):
            stream.seek(offset)
            chunk = stream.read(count)
            if len(chunk) != count:
                raise ValueError("TIFF sample strip is incomplete")
            chunks.append(chunk)
    encoded = b"".join(chunks)
    if parsed.endian == ">":
        if len(encoded) % 2:
            raise ValueError("TIFF uint16 strip byte count is odd")
        swapped = bytearray(len(encoded))
        swapped[0::2] = encoded[1::2]
        swapped[1::2] = encoded[0::2]
        return bytes(swapped)
    return encoded


def verify_research_dng(mosaic: NormalizedMosaic, dng_path: pathlib.Path) -> dict[str, object]:
    parsed = parse_tiff(dng_path)
    width = parsed.unsigned_values(256)[0]
    height = parsed.unsigned_values(257)[0]
    if (width, height) != (mosaic.width, mosaic.height):
        raise ValueError("research DNG dimensions differ from normalized mosaic")
    samples = extract_u16le_strip(parsed)
    sample_sha256 = hashlib.sha256(samples).hexdigest()
    if len(samples) != mosaic.sample_bytes or sample_sha256 != mosaic.sample_sha256:
        raise ValueError("research DNG CFA strip is not byte-identical to normalized mosaic")
    private = _private_document(parsed)
    if private.get("normalized_mosaic") != mosaic.metadata():
        raise ValueError("research DNG private descriptor is not an exact metadata round trip")
    return {
        "sample_roundtrip": "byte-identical",
        "metadata_roundtrip": "exact-private-payload",
        "sample_bytes": len(samples),
        "sample_sha256": sample_sha256,
    }


def _run(command: Sequence[str], log_path: pathlib.Path, *, timeout: float) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=timeout,
        env={**os.environ, "LC_ALL": "C", "TZ": "UTC"},
    )
    log_path.write_text(
        f"exit_code={completed.returncode}\n--- stdout ---\n{completed.stdout}\n--- stderr ---\n{completed.stderr}",
        encoding="utf-8",
    )
    if completed.returncode != 0:
        raise ValueError(f"external DNG verifier failed: {command[0]}")
    return completed


def _libraw_unprocessed_verify(
    unprocessed_raw: pathlib.Path,
    dng_path: pathlib.Path,
    output_directory: pathlib.Path,
    mosaic: NormalizedMosaic,
    timeout: float,
) -> dict[str, object]:
    output = pathlib.Path(f"{dng_path}.tiff")
    _run(
        (
            str(unprocessed_raw),
            "-q",
            "-T",
            str(dng_path),
        ),
        output_directory / "libraw-unprocessed.log",
        timeout=timeout,
    )
    parsed = parse_tiff(output)
    width = parsed.unsigned_values(256)[0]
    height = parsed.unsigned_values(257)[0]
    samples = extract_u16le_strip(parsed)
    digest = hashlib.sha256(samples).hexdigest()
    if (width, height) != (mosaic.width, mosaic.height):
        raise ValueError("LibRaw unprocessed dimensions differ from normalized mosaic")
    if len(samples) != mosaic.sample_bytes or digest != mosaic.sample_sha256:
        raise ValueError("LibRaw unprocessed CFA is not byte-identical to normalized mosaic")
    return {"status": "byte-identical", "sample_sha256": digest}


def from_raw(args: argparse.Namespace) -> int:
    source = pathlib.Path(args.input).expanduser().resolve(strict=True)
    output_directory = pathlib.Path(args.output_directory).expanduser().resolve(strict=False)
    output_directory.mkdir(parents=True, exist_ok=True)
    manifest = output_directory / "source.shadowrawi"
    nonce = "00000000-0000-0000-0000-000000000001"
    helper = pathlib.Path(args.decode_helper).expanduser().resolve(strict=True)
    _run(
        (str(helper), "raw-frame-staging", str(source), str(manifest), nonce),
        output_directory / "shadow-staging.log",
        timeout=args.timeout_seconds,
    )
    mosaic = read_staging(manifest)
    dng_path = output_directory / "normalized.dng"
    receipt = write_research_dng(mosaic, dng_path)
    self_verification = verify_research_dng(mosaic, dng_path)

    raw_identify_status = "not-requested"
    if args.raw_identify:
        executable = pathlib.Path(args.raw_identify).expanduser().resolve(strict=True)
        _run(
            (str(executable), "-v", "-w", str(dng_path)),
            output_directory / "raw-identify.log",
            timeout=args.timeout_seconds,
        )
        raw_identify_status = "recognized"
    libraw_status = "not-requested"
    if args.unprocessed_raw:
        executable = pathlib.Path(args.unprocessed_raw).expanduser().resolve(strict=True)
        libraw = _libraw_unprocessed_verify(
            executable, dng_path, output_directory, mosaic, args.timeout_seconds
        )
        libraw_status = str(libraw["status"])

    print("normalized_dng.status=ok")
    print(f"normalized_dng.width={mosaic.width}")
    print(f"normalized_dng.height={mosaic.height}")
    print(f"normalized_dng.cfa={mosaic.cfa}")
    print(f"normalized_dng.sample_bytes={mosaic.sample_bytes}")
    print(f"normalized_dng.sample_sha256={mosaic.sample_sha256}")
    print(f"normalized_dng.dng_sha256={receipt.dng_sha256}")
    print(f"normalized_dng.sample_roundtrip={self_verification['sample_roundtrip']}")
    print(f"normalized_dng.metadata_roundtrip={self_verification['metadata_roundtrip']}")
    print(f"normalized_dng.standard_white_projection={receipt.standard_white_projection}")
    print(f"normalized_dng.standard_neutral_projection={receipt.standard_neutral_projection}")
    print(f"normalized_dng.calibration_projection={receipt.calibration_projection}")
    print(f"normalized_dng.raw_identify={raw_identify_status}")
    print(f"normalized_dng.libraw_unprocessed_roundtrip={libraw_status}")
    return 0


def build(args: argparse.Namespace) -> int:
    mosaic = read_staging(pathlib.Path(args.staging_manifest))
    receipt = write_research_dng(mosaic, pathlib.Path(args.output))
    verification = verify_research_dng(mosaic, receipt.output_path)
    print(json.dumps({**dataclasses.asdict(receipt), **verification}, default=str, sort_keys=True))
    return 0


def verify(args: argparse.Namespace) -> int:
    mosaic = read_staging(pathlib.Path(args.staging_manifest))
    print(json.dumps(verify_research_dng(mosaic, pathlib.Path(args.dng)), sort_keys=True))
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Build a byte-exact Shadow research DNG")
    commands = result.add_subparsers(dest="command", required=True)
    build_parser = commands.add_parser("build")
    build_parser.add_argument("--staging-manifest", required=True)
    build_parser.add_argument("--output", required=True)
    build_parser.set_defaults(handler=build)
    verify_parser = commands.add_parser("verify")
    verify_parser.add_argument("--staging-manifest", required=True)
    verify_parser.add_argument("--dng", required=True)
    verify_parser.set_defaults(handler=verify)
    raw_parser = commands.add_parser("from-raw")
    raw_parser.add_argument("--decode-helper", required=True)
    raw_parser.add_argument("--input", required=True)
    raw_parser.add_argument("--output-directory", required=True)
    raw_parser.add_argument("--raw-identify")
    raw_parser.add_argument("--unprocessed-raw")
    raw_parser.add_argument("--timeout-seconds", type=float, default=600.0)
    raw_parser.set_defaults(handler=from_raw)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    if getattr(args, "timeout_seconds", 1.0) <= 0:
        print("error: timeout must be positive", file=sys.stderr)
        return 2
    try:
        return int(args.handler(args))
    except (OSError, ValueError, json.JSONDecodeError, subprocess.TimeoutExpired) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
