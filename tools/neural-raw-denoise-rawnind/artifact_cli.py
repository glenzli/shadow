#!/usr/bin/env python3
"""Inspect and recover external Shadow RAW foundation artifacts."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import numpy as np

import foundation_artifact


def run(arguments: argparse.Namespace) -> int:
    if arguments.command == "verify":
        receipt = foundation_artifact.verify_artifact(arguments.artifact)
        print(
            json.dumps(
                foundation_artifact.as_json(receipt),
                indent=2,
                sort_keys=True,
            )
        )
        return 0
    if arguments.command == "read-probe":
        with foundation_artifact.FoundationArtifactReader(
            arguments.artifact
        ) as reader:
            rows = reader.read_rows(arguments.y_start, arguments.rows)
            verification = reader.verification
        report = {
            "artifact_identity_sha256": (
                verification.artifact_identity_sha256
            ),
            "finite": bool(np.isfinite(rows).all()),
            "max": float(np.max(rows)),
            "mean": float(np.mean(rows, dtype=np.float64)),
            "min": float(np.min(rows)),
            "shape": list(rows.shape),
            "y_start": arguments.y_start,
        }
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    if arguments.command == "recover":
        receipt = foundation_artifact.recover_completed_artifact(
            arguments.partial,
            arguments.destination,
        )
        print(
            json.dumps(
                foundation_artifact.as_json(receipt),
                indent=2,
                sort_keys=True,
            )
        )
        return 0
    raise AssertionError("unreachable artifact command")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)

    verify = commands.add_parser("verify")
    verify.add_argument("artifact", type=Path)

    read_probe = commands.add_parser("read-probe")
    read_probe.add_argument("artifact", type=Path)
    read_probe.add_argument("--y-start", type=int, required=True)
    read_probe.add_argument("--rows", type=int, required=True)

    recover = commands.add_parser("recover")
    recover.add_argument("partial", type=Path)
    recover.add_argument("destination", type=Path)
    return parser.parse_args()


if __name__ == "__main__":
    try:
        sys.exit(run(parse_arguments()))
    except (OSError, ValueError) as error:
        print(f"foundation artifact command failed: {error}", file=sys.stderr)
        sys.exit(2)
