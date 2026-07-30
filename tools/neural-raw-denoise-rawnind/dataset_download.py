#!/usr/bin/env python3
"""Resume, verify, and atomically publish the pinned RawNIND pilot files."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict
from functools import partial
import json
import os
from pathlib import Path
import subprocess
from typing import Any, Callable

import dataset_manifest
import public_pair


SCHEMA = "shadow-rawnind-evaluation-download-receipt-v1"
CurlRunner = Callable[..., subprocess.CompletedProcess[Any]]


def _public_raw(value: dict[str, Any]) -> public_pair.PublicRawFile:
    required = {
        "role",
        "dataverse_file_id",
        "persistent_id",
        "filename",
        "size_bytes",
        "md5",
    }
    if set(value) != required:
        raise ValueError("RawNIND manifest RAW identity schema mismatch")
    identity = public_pair.PublicRawFile(**value)
    if Path(identity.filename).name != identity.filename:
        raise ValueError("RawNIND manifest filename is not a basename")
    return identity


def selected_files(
    manifest: dict[str, Any],
) -> list[public_pair.PublicRawFile]:
    dataset_manifest.validate_manifest(manifest)
    selected: list[public_pair.PublicRawFile] = []
    seen: set[str] = set()
    for pair in manifest["pairs"]:
        for key in ("ground_truth", "noisy"):
            identity = _public_raw(pair[key])
            if identity.filename in seen:
                raise ValueError("RawNIND manifest reuses a payload")
            seen.add(identity.filename)
            selected.append(identity)
    return selected


def download_one(
    expected: public_pair.PublicRawFile,
    destination: Path,
    *,
    curl_runner: CurlRunner = subprocess.run,
) -> public_pair.VerifiedPublicRaw:
    resolved_destination = destination.resolve()
    resolved_destination.mkdir(parents=True, exist_ok=True)
    final_path = resolved_destination / expected.filename
    partial_path = resolved_destination / f".{expected.filename}.partial"
    if final_path.exists():
        if partial_path.exists():
            raise ValueError("verified RAW and owned partial both exist")
        return public_pair.verify_public_raw(final_path, expected)

    command = [
        "curl",
        "--fail",
        "--location",
        "--silent",
        "--show-error",
        "--retry",
        "5",
        "--retry-all-errors",
        "--connect-timeout",
        "30",
        "--continue-at",
        "-",
        "--output",
        str(partial_path),
        expected.download_url,
    ]
    curl_runner(command, check=True)
    try:
        public_pair.verify_public_raw(partial_path, expected)
    except ValueError:
        partial_path.unlink(missing_ok=True)
        raise
    try:
        os.link(partial_path, final_path)
    except FileExistsError:
        verified = public_pair.verify_public_raw(final_path, expected)
    else:
        verified = public_pair.verify_public_raw(final_path, expected)
    finally:
        partial_path.unlink(missing_ok=True)
    return verified


def download_manifest(
    manifest: dict[str, Any],
    destination: Path,
    *,
    curl_runner: CurlRunner = subprocess.run,
    workers: int = 4,
) -> list[public_pair.VerifiedPublicRaw]:
    if workers < 1 or workers > 8:
        raise ValueError("RawNIND download workers must be between 1 and 8")
    operation = partial(
        download_one,
        destination=destination,
        curl_runner=curl_runner,
    )
    with ThreadPoolExecutor(max_workers=workers) as executor:
        return list(executor.map(operation, selected_files(manifest)))


def _segment_paths(
    destination: Path,
    expected: public_pair.PublicRawFile,
    segment_bytes: int,
) -> list[tuple[int, int, Path]]:
    root = destination.resolve() / ".segments" / expected.filename
    root.mkdir(parents=True, exist_ok=True)
    return [
        (
            start,
            min(start + segment_bytes, expected.size_bytes) - 1,
            root / f"{start:012d}.part",
        )
        for start in range(0, expected.size_bytes, segment_bytes)
    ]


def download_segment(
    expected: public_pair.PublicRawFile,
    start: int,
    end: int,
    path: Path,
    *,
    curl_runner: CurlRunner = subprocess.run,
) -> Path:
    if start < 0 or end < start or end >= expected.size_bytes:
        raise ValueError("RawNIND download segment range is invalid")
    expected_bytes = end - start + 1
    if path.exists():
        if path.stat().st_size == expected_bytes:
            return path
        path.unlink()
    partial = path.with_suffix(path.suffix + ".download")
    partial.unlink(missing_ok=True)
    command = [
        "curl",
        "--fail",
        "--location",
        "--silent",
        "--show-error",
        "--retry",
        "5",
        "--retry-all-errors",
        "--connect-timeout",
        "30",
        "--range",
        f"{start}-{end}",
        "--output",
        str(partial),
        expected.download_url,
    ]
    try:
        curl_runner(command, check=True)
        if partial.stat().st_size != expected_bytes:
            raise ValueError("RawNIND HTTP range length mismatch")
        os.link(partial, path)
    except FileExistsError:
        if path.stat().st_size != expected_bytes:
            raise ValueError("RawNIND published segment length mismatch")
    except BaseException:
        partial.unlink(missing_ok=True)
        raise
    finally:
        partial.unlink(missing_ok=True)
    return path


def _assemble_segments(
    expected: public_pair.PublicRawFile,
    destination: Path,
    segments: list[tuple[int, int, Path]],
) -> public_pair.VerifiedPublicRaw:
    final_path = destination.resolve() / expected.filename
    old_direct_partial = final_path.with_name(f".{expected.filename}.partial")
    old_direct_partial.unlink(missing_ok=True)
    assembly_partial = final_path.with_name(final_path.name + ".partial")
    assembly_partial.unlink(missing_ok=True)
    if final_path.exists():
        return public_pair.verify_public_raw(final_path, expected)
    ordered_paths = [path for _, _, path in segments]
    verified = public_pair.assemble_download_parts(
        ordered_paths,
        final_path,
        expected,
    )
    for path in ordered_paths:
        path.unlink(missing_ok=True)
    segment_directory = ordered_paths[0].parent
    segment_directory.rmdir()
    try:
        segment_directory.parent.rmdir()
    except OSError:
        pass
    return verified


def download_segmented_manifest(
    manifest: dict[str, Any],
    destination: Path,
    *,
    curl_runner: CurlRunner = subprocess.run,
    workers: int = 16,
    segment_bytes: int = 1024 * 1024,
) -> list[public_pair.VerifiedPublicRaw]:
    if workers < 1 or workers > 32:
        raise ValueError("RawNIND range workers must be between 1 and 32")
    if segment_bytes < 64 * 1024 or segment_bytes > 16 * 1024 * 1024:
        raise ValueError("RawNIND segment size must be between 64 KiB and 16 MiB")
    resolved_destination = destination.resolve()
    resolved_destination.mkdir(parents=True, exist_ok=True)
    identities = selected_files(manifest)
    pending: list[
        tuple[public_pair.PublicRawFile, int, int, Path]
    ] = []
    segments_by_filename: dict[str, list[tuple[int, int, Path]]] = {}
    for identity in identities:
        final_path = resolved_destination / identity.filename
        if final_path.exists():
            public_pair.verify_public_raw(final_path, identity)
            segments_by_filename[identity.filename] = []
            continue
        segments = _segment_paths(
            resolved_destination,
            identity,
            segment_bytes,
        )
        segments_by_filename[identity.filename] = segments
        pending.extend(
            (identity, start, end, path)
            for start, end, path in segments
            if not path.exists() or path.stat().st_size != end - start + 1
        )

    def run_segment(
        task: tuple[public_pair.PublicRawFile, int, int, Path],
    ) -> Path:
        identity, start, end, path = task
        return download_segment(
            identity,
            start,
            end,
            path,
            curl_runner=curl_runner,
        )

    with ThreadPoolExecutor(max_workers=workers) as executor:
        list(executor.map(run_segment, pending))

    return [
        (
            public_pair.verify_public_raw(
                resolved_destination / identity.filename,
                identity,
            )
            if not segments_by_filename[identity.filename]
            else _assemble_segments(
                identity,
                resolved_destination,
                segments_by_filename[identity.filename],
            )
        )
        for identity in identities
    ]


def write_receipt(
    manifest: dict[str, Any],
    files: list[public_pair.VerifiedPublicRaw],
    path: Path,
) -> dict[str, Any]:
    value = {
        "schema": SCHEMA,
        "manifest_sha256": manifest["manifest_sha256"],
        "file_count": len(files),
        "total_bytes": sum(file.size_bytes for file in files),
        "files": [asdict(file) for file in files],
    }
    resolved = path.resolve()
    if resolved.exists():
        raise ValueError("RawNIND download receipt must not already exist")
    resolved.parent.mkdir(parents=True, exist_ok=True)
    with resolved.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
    return value


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=16)
    parser.add_argument("--segment-mib", type=int, default=1)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    manifest = dataset_manifest.load_manifest(arguments.manifest)
    files = download_segmented_manifest(
        manifest,
        arguments.destination,
        workers=arguments.workers,
        segment_bytes=arguments.segment_mib * 1024 * 1024,
    )
    receipt = write_receipt(manifest, files, arguments.receipt)
    print(json.dumps(receipt, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"error: {error}")
        raise SystemExit(2) from error
