#!/usr/bin/env python3
"""Derive factual clipped-site topology from byte-exact RawFrame staging."""

from __future__ import annotations

import argparse
import json
import pathlib
import resource
import sys
import time

import numpy as np

from normalized_mosaic import NormalizedMosaic, read_staging, sha256_file


TOPOLOGY_SCHEMA = "shadow.raw-highlight-cfa-topology.v1"
TOPOLOGY_VERSION = "20260825.1"
BIT_PHYSICAL_WHITE = np.uint8(1 << 0)
BIT_LINEAR_RESPONSE_TERMINAL = np.uint8(1 << 1)
BIT_SHARED_PHYSICAL_WHITE = np.uint8(1 << 2)
CHANNEL_BITS = {
    "R": np.uint8(1 << 3),
    "G": np.uint8(1 << 4),
    "B": np.uint8(1 << 5),
}
MASK_BITS = {
    "physical-white": int(BIT_PHYSICAL_WHITE),
    "linear-response-terminal": int(BIT_LINEAR_RESPONSE_TERMINAL),
    "shared-physical-white": int(BIT_SHARED_PHYSICAL_WHITE),
    "red-physical-white": int(CHANNEL_BITS["R"]),
    "green-physical-white": int(CHANNEL_BITS["G"]),
    "blue-physical-white": int(CHANNEL_BITS["B"]),
}
MAX_TOPOLOGY_BYTES = 512 * 1024 * 1024


def _identity(path: pathlib.Path) -> dict[str, object]:
    return {
        "basename": path.name,
        "size_bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def _external_output(path: pathlib.Path) -> pathlib.Path:
    resolved = path.expanduser().resolve()
    repository_root = pathlib.Path(__file__).resolve().parents[2]
    try:
        resolved.relative_to(repository_root)
    except ValueError:
        return resolved
    raise ValueError("CFA topology output must remain outside the Shadow repository")


def _peak_rss_bytes() -> int:
    value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return int(value if sys.platform == "darwin" else value * 1024)


def _box3_counts(mask: np.ndarray) -> np.ndarray:
    height, width = mask.shape
    padded = np.pad(np.asarray(mask, dtype=np.uint8), 1)
    result = np.zeros((height, width), dtype=np.uint8)
    for dy in range(3):
        for dx in range(3):
            result += padded[dy : dy + height, dx : dx + width]
    return result


def _parent_receipt(
    parent_manifest: pathlib.Path,
    staging_manifest: pathlib.Path,
    sample_path: pathlib.Path,
) -> dict[str, object]:
    document = json.loads(parent_manifest.read_text(encoding="utf-8"))
    required = {sha256_file(staging_manifest), sha256_file(sample_path)}
    matches: list[dict[str, object]] = []
    for adapter in document.get("adapters", []):
        hashes = {
            str(artifact.get("sha256"))
            for artifact in adapter.get("artifacts", [])
            if artifact.get("sha256")
        }
        if required <= hashes:
            matches.append(adapter)
    if len(matches) != 1:
        raise ValueError(
            "parent oracle manifest must contain the exact staging manifest and sample payload"
        )
    adapter = matches[0]
    return {
        "manifest": _identity(parent_manifest),
        "schema": document.get("schema"),
        "run_name": document.get("run_name"),
        "adapter_id": adapter.get("id"),
    }


def _site_channel_indices(cfa: str) -> np.ndarray:
    return np.asarray(["RGB".index(colour) for colour in cfa], dtype=np.uint8)


def _chunk_site_indices(first_y: int, last_y: int, width: int) -> np.ndarray:
    rows = np.arange(first_y, last_y, dtype=np.uint32)[:, None]
    columns = np.arange(width, dtype=np.uint32)[None, :]
    return ((rows & 1) * 2 + (columns & 1)).astype(np.uint8)


def build_topology(
    mosaic: NormalizedMosaic,
    payload_path: pathlib.Path,
    *,
    rows_per_chunk: int,
) -> dict[str, object]:
    if rows_per_chunk < 2 or rows_per_chunk > 4096:
        raise ValueError("rows per chunk must be between 2 and 4096")
    expected = mosaic.width * mosaic.height
    if expected <= 0 or expected > MAX_TOPOLOGY_BYTES:
        raise ValueError("CFA topology dimensions exceed the bounded payload contract")
    samples = np.memmap(
        mosaic.sample_path,
        dtype="<u2",
        mode="r",
        shape=(mosaic.height, mosaic.width),
    )
    output = np.memmap(
        payload_path,
        dtype=np.uint8,
        mode="w+",
        shape=(mosaic.height, mosaic.width),
    )
    site_channels = _site_channel_indices(mosaic.cfa)
    white = np.asarray(mosaic.white, dtype=np.uint16)
    linear_response = np.asarray(mosaic.linear_response, dtype=np.uint16)
    physical_by_site = np.zeros(4, dtype=np.uint64)
    linear_by_site = np.zeros(4, dtype=np.uint64)
    shared_count = 0

    for first_y in range(0, mosaic.height, rows_per_chunk):
        last_y = min(mosaic.height, first_y + rows_per_chunk)
        halo_first = max(0, first_y - 1)
        halo_last = min(mosaic.height, last_y + 1)
        halo_samples = np.asarray(samples[halo_first:halo_last])
        halo_sites = _chunk_site_indices(halo_first, halo_last, mosaic.width)
        physical_halo = halo_samples >= white[halo_sites]
        if mosaic.has_linear_response:
            linear_halo = halo_samples >= linear_response[halo_sites]
        else:
            linear_halo = np.zeros_like(physical_halo)
        core_offset = first_y - halo_first
        core_rows = last_y - first_y
        core_slice = slice(core_offset, core_offset + core_rows)
        core_sites = halo_sites[core_slice]
        core_physical = physical_halo[core_slice]
        core_linear = linear_halo[core_slice]
        flags = np.zeros((core_rows, mosaic.width), dtype=np.uint8)
        flags[core_physical] |= BIT_PHYSICAL_WHITE
        flags[core_linear] |= BIT_LINEAR_RESPONSE_TERMINAL

        shared = np.ones((core_rows, mosaic.width), dtype=bool)
        for channel, colour in enumerate("RGB"):
            channel_sites = site_channels[halo_sites] == channel
            observed = _box3_counts(channel_sites)[core_slice]
            clipped = _box3_counts(channel_sites & physical_halo)[core_slice]
            shared &= (observed > 0) & (clipped == observed)
            channel_physical = core_physical & (site_channels[core_sites] == channel)
            flags[channel_physical] |= CHANNEL_BITS[colour]
        flags[shared] |= BIT_SHARED_PHYSICAL_WHITE
        output[first_y:last_y] = flags
        shared_count += int(np.count_nonzero(shared))
        for site in range(4):
            selected = core_sites == site
            physical_by_site[site] += int(np.count_nonzero(core_physical & selected))
            linear_by_site[site] += int(np.count_nonzero(core_linear & selected))
    output.flush()
    del output
    del samples
    physical_by_channel = {
        colour: int(
            sum(
                physical_by_site[site]
                for site, site_colour in enumerate(mosaic.cfa)
                if site_colour == colour
            )
        )
        for colour in "RGB"
    }
    return {
        "physical_white_sample_count": int(np.sum(physical_by_site)),
        "linear_response_terminal_sample_count": int(np.sum(linear_by_site)),
        "shared_physical_white_sample_count": shared_count,
        "physical_white_by_cfa_site": [int(value) for value in physical_by_site],
        "linear_response_terminal_by_cfa_site": [int(value) for value in linear_by_site],
        "physical_white_by_channel": physical_by_channel,
    }


def generate(args: argparse.Namespace) -> int:
    started = time.monotonic()
    manifest_path = args.staging_manifest.expanduser().resolve()
    output_root = _external_output(args.output_root)
    run_directory = output_root / args.run_name
    run_directory.mkdir(parents=True, exist_ok=False)
    mosaic = read_staging(manifest_path)
    payload_path = run_directory / "topology.u8"
    statistics = build_topology(
        mosaic,
        payload_path,
        rows_per_chunk=args.rows_per_chunk,
    )
    source_receipt: dict[str, object] = {
        "staging_manifest": _identity(mosaic.manifest_path),
        "sample_payload": _identity(mosaic.sample_path),
        "descriptor": {
            "contract": mosaic.metadata()["descriptor_contract"],
            "black_levels": list(mosaic.black),
            "physical_white_levels": list(mosaic.white),
            "linear_response_limits": list(mosaic.linear_response),
            "has_linear_response_limits": mosaic.has_linear_response,
        },
    }
    document: dict[str, object] = {
        "schema": TOPOLOGY_SCHEMA,
        "topology_version": TOPOLOGY_VERSION,
        "run_name": args.run_name,
        "coordinate_space": {
            "plane": "active-raw-sensor",
            "storage": "row-major-unoriented-active",
            "width": mosaic.width,
            "height": mosaic.height,
            "orientation": mosaic.orientation,
            "cfa_2x2": mosaic.cfa,
        },
        "source": source_receipt,
        "bitfield": {
            "artifact": _identity(payload_path),
            "encoding": "u8-row-major-unoriented-active-v1",
            "bits": {
                "physical-white": 0,
                "linear-response-terminal": 1,
                "shared-physical-white-3x3": 2,
                "red-physical-white": 3,
                "green-physical-white": 4,
                "blue-physical-white": 5,
            },
        },
        "statistics": statistics,
        "resource_usage": {
            "elapsed_ms": round((time.monotonic() - started) * 1000.0, 3),
            "peak_rss_bytes": _peak_rss_bytes(),
            "rows_per_chunk": args.rows_per_chunk,
        },
    }
    if args.parent_manifest is not None:
        parent = args.parent_manifest.expanduser().resolve()
        source_receipt["parent_oracle"] = _parent_receipt(
            parent,
            mosaic.manifest_path,
            mosaic.sample_path,
        )
    topology_manifest = run_directory / "topology.json"
    topology_manifest.write_text(
        json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"topology.run_directory={run_directory}")
    print(f"topology.manifest={topology_manifest}")
    print(f"topology.physical_white_samples={statistics['physical_white_sample_count']}")
    return 0


def _oriented_mask(mask: np.ndarray, orientation: int, image_space: str) -> np.ndarray:
    if image_space == "active":
        return mask
    if image_space != "display":
        raise ValueError(f"unsupported topology image space: {image_space}")
    if orientation in {0, 1}:
        return mask
    if orientation == 3:
        return np.rot90(mask, 2)
    if orientation == 5:
        return np.rot90(mask, 1)
    if orientation == 6:
        return np.rot90(mask, -1)
    raise ValueError(f"unsupported RawFrame display orientation: {orientation}")


def read_topology_mask(
    manifest_path: pathlib.Path,
    mask_name: str,
    image_space: str,
    expected_dimensions: tuple[int, int],
    crop_xywh: tuple[int, int, int, int],
    reference_geometry_crop_xywh: tuple[int, int, int, int] | None = None,
) -> tuple[np.ndarray, dict[str, object]]:
    path = manifest_path.expanduser().resolve()
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema") != TOPOLOGY_SCHEMA:
        raise ValueError(f"topology manifest must use {TOPOLOGY_SCHEMA}")
    if mask_name not in MASK_BITS:
        raise ValueError(f"unsupported topology mask: {mask_name}")
    coordinate = document["coordinate_space"]
    width = int(coordinate["width"])
    height = int(coordinate["height"])
    if width <= 0 or height <= 0 or width * height > MAX_TOPOLOGY_BYTES:
        raise ValueError("topology dimensions exceed the bounded payload contract")
    artifact = document["bitfield"]["artifact"]
    payload = path.parent / str(artifact["basename"])
    if (
        not payload.is_file()
        or payload.stat().st_size != width * height
        or payload.stat().st_size != artifact.get("size_bytes")
        or sha256_file(payload) != artifact.get("sha256")
    ):
        raise ValueError("topology bitfield identity changed")
    bitfield = np.memmap(payload, dtype=np.uint8, mode="r", shape=(height, width))
    selected = (bitfield & np.uint8(MASK_BITS[mask_name])) != 0
    oriented = _oriented_mask(selected, int(coordinate["orientation"]), image_space)
    expected_width, expected_height = expected_dimensions
    source_height, source_width = oriented.shape
    if reference_geometry_crop_xywh is None:
        geometry_crop = (0, 0, source_width, source_height)
        geometry_method = "identity-v1"
    else:
        geometry_crop = reference_geometry_crop_xywh
        geometry_method = "exact-integer-crop-v1"
    geometry_x, geometry_y, geometry_width, geometry_height = geometry_crop
    if (
        geometry_x < 0
        or geometry_y < 0
        or geometry_width <= 0
        or geometry_height <= 0
        or geometry_x + geometry_width > source_width
        or geometry_y + geometry_height > source_height
    ):
        raise ValueError("topology reference geometry crop is outside the oriented mask")
    matched = oriented[
        geometry_y : geometry_y + geometry_height,
        geometry_x : geometry_x + geometry_width,
    ]
    if matched.shape != (expected_height, expected_width):
        raise ValueError(
            "topology and reference dimensions differ; declare an exact integer crop instead of resampling"
        )
    x, y, crop_width, crop_height = crop_xywh
    cropped = np.asarray(matched[y : y + crop_height, x : x + crop_width], dtype=bool)
    if cropped.shape != (crop_height, crop_width):
        raise ValueError("topology crop is incomplete")
    receipt = {
        "manifest": _identity(path),
        "payload": _identity(payload),
        "mask": mask_name,
        "image_space": image_space,
        "source_dimensions": [width, height],
        "matched_dimensions": [expected_width, expected_height],
        "reference_geometry": {
            "method": geometry_method,
            "source_crop_xywh": list(geometry_crop),
            "output_dimensions": [expected_width, expected_height],
            "resampling": False,
        },
        "crop_xywh": list(crop_xywh),
        "selected_sample_count": int(np.count_nonzero(cropped)),
    }
    return cropped, receipt


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--staging-manifest", type=pathlib.Path, required=True)
    result.add_argument("--parent-manifest", type=pathlib.Path)
    result.add_argument("--output-root", type=pathlib.Path, required=True)
    result.add_argument("--run-name", required=True)
    result.add_argument("--rows-per-chunk", type=int, default=256)
    return result


def main(argv: list[str] | None = None) -> int:
    try:
        return generate(parser().parse_args(argv))
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
