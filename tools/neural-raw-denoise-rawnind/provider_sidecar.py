#!/usr/bin/env python3
"""Executable reference sidecar for Shadow's RawNIND RAW foundation provider."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import sys
from typing import NoReturn

import numpy as np
import onnxruntime as ort

import foundation_artifact
import inference
import package_contract
import stripe_lifecycle


MODEL_RECEIPT_PREFIX = "shadow-rawnind-foundation-model-v1"
PLAN_RECEIPT_PREFIX = "shadow-rawnind-foundation-plan-v1"
FOUNDATION_RECEIPT_PREFIX = "shadow-rawnind-foundation-v1"
EXPECTED_MANIFEST_SHA256 = (
    "d3d56084216df66ed4aba057700b0c157c2539b78374f137e0a6eb8c5d3f181f"
)
EXPECTED_RUNTIME_VERSION = "1.24.4"
EXPECTED_MODEL_ID = "darktable-ai/rawnind-public-bayer"
EXPECTED_MODEL_REVISION = (
    "release-5.6.0@5454d7aa6d89a67054fd4a83343b09e69acaf76a"
)
EXPECTED_ARTIFACT_SET_BLAKE3 = (
    "51bfabe88964e78ac74007e7b499e61ed07dc9dba51c28f3d0d495becb611d94"
)
EXPECTED_PREPROCESSING_VERSION = "rawnind-bayer-foundation-20260730.1"
EXPECTED_GRAPH_MEMBER = "rawdenoise-nind/model_bayer.onnx"
EXPECTED_SOURCE_PIXEL_CONTRACT_SHA256 = (
    "e1998069001c14d01251cc3d6e2bc2aa66b807f3f17d246e7ee7270528302f7f"
)

_cancelled = False


def _request_cancellation(
    unused_signum: int,
    unused_frame: object,
) -> None:
    global _cancelled
    _cancelled = True


def _is_cancelled() -> bool:
    return _cancelled


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _validate_lower_sha256(value: str, label: str) -> str:
    if (
        len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise ValueError(f"{label} must be a lowercase SHA-256")
    return value


def verify_manifest(path: Path) -> dict[str, object]:
    resolved = path.resolve()
    if not resolved.is_file():
        raise ValueError("RawNIND model manifest does not exist")
    payload = resolved.read_bytes()
    if hashlib.sha256(payload).hexdigest() != EXPECTED_MANIFEST_SHA256:
        raise ValueError("RawNIND model manifest bytes changed")
    manifest = json.loads(payload)
    if not isinstance(manifest, dict):
        raise ValueError("RawNIND model manifest must be an object")
    if (
        manifest.get("schema_version") != 1
        or manifest.get("model_id") != EXPECTED_MODEL_ID
        or manifest.get("exact_revision") != EXPECTED_MODEL_REVISION
        or manifest.get("capabilities") != ["raw_foundation_denoise"]
        or manifest.get("format") != "onnx"
        or manifest.get("opset") != 20
        or manifest.get("preprocessing_version")
        != EXPECTED_PREPROCESSING_VERSION
    ):
        raise ValueError("RawNIND model manifest identity changed")

    artifact_set = manifest.get("artifact_set")
    if (
        not isinstance(artifact_set, dict)
        or artifact_set.get("inventory_blake3")
        != EXPECTED_ARTIFACT_SET_BLAKE3
    ):
        raise ValueError("RawNIND artifact-set identity changed")
    artifacts = artifact_set.get("artifacts")
    expected_artifacts = [
        {
            "relative_path": "rawdenoise-nind.dtmodel",
            "role": "auxiliary",
            "byte_len": 57_700_134,
            "sha256": package_contract.PACKAGE_SHA256,
        },
        {
            "relative_path": EXPECTED_GRAPH_MEMBER,
            "role": "model_definition",
            "byte_len": package_contract.PACKAGE_MEMBERS[
                EXPECTED_GRAPH_MEMBER
            ][0],
            "sha256": package_contract.PACKAGE_MEMBERS[
                EXPECTED_GRAPH_MEMBER
            ][1],
        },
    ]
    if artifacts != expected_artifacts:
        raise ValueError("RawNIND model artifact inventory changed")

    targets = manifest.get("execution_targets")
    if targets != [
        {
            "kind": "cpu",
            "minimum_runtime_version": "onnxruntime-1.24.4",
            "precisions": ["float32"],
        }
    ]:
        raise ValueError("RawNIND execution target changed")
    return manifest


def verify_model(
    model_package: Path,
    model_graph: Path,
    manifest_path: Path,
) -> ort.InferenceSession:
    verify_manifest(manifest_path)
    package_receipt = package_contract.verify_package(model_package)
    graph = model_graph.resolve()
    if not graph.is_file():
        raise ValueError("RawNIND Bayer graph does not exist")
    expected_size, expected_sha256 = package_contract.PACKAGE_MEMBERS[
        EXPECTED_GRAPH_MEMBER
    ]
    if graph.stat().st_size != expected_size:
        raise ValueError("RawNIND Bayer graph size changed")
    if _sha256_file(graph) != expected_sha256:
        raise ValueError("RawNIND Bayer graph identity changed")
    if (
        package_receipt.member_sha256.get(EXPECTED_GRAPH_MEMBER)
        != expected_sha256
    ):
        raise ValueError("RawNIND package and extracted graph disagree")
    if ort.__version__ != EXPECTED_RUNTIME_VERSION:
        raise ValueError(
            "RawNIND reference sidecar requires ONNX Runtime "
            f"{EXPECTED_RUNTIME_VERSION}, got {ort.__version__}"
        )
    session, unused_load_ms = inference.create_session(graph, "cpu")
    if session.get_providers() != ["CPUExecutionProvider"]:
        raise ValueError("RawNIND reference sidecar did not bind CPU execution")
    return session


def model_receipt() -> str:
    graph_sha256 = package_contract.PACKAGE_MEMBERS[
        EXPECTED_GRAPH_MEMBER
    ][1]
    return (
        f"{MODEL_RECEIPT_PREFIX} "
        f"package_sha256={package_contract.PACKAGE_SHA256} "
        f"graph_sha256={graph_sha256} "
        f"runtime_version={ort.__version__}"
    )


def _execution_identity(
    session: ort.InferenceSession,
) -> dict[str, object]:
    return {
        "engine": "onnxruntime",
        "runtime_version": ort.__version__,
        "requested_provider": "cpu",
        "active_providers": session.get_providers(),
        "platform": platform.platform(),
        "machine": platform.machine(),
    }


def _prepare_foundation(
    session: ort.InferenceSession,
    source: Path,
    source_pixel_contract_sha256: str,
) -> tuple[
    np.ndarray,
    stripe_lifecycle.StripePlan,
    foundation_artifact.RawNindFoundationContract,
]:
    packed, raw_preprocessing = inference.load_raw_as_packed_bayer(source)
    plan = stripe_lifecycle.plan_striped_image(
        packed.shape[1],
        packed.shape[2],
    )
    contract = foundation_artifact.make_contract(
        source,
        raw_preprocessing,
        plan,
        source_pixel_contract_sha256,
        execution=_execution_identity(session),
    )
    return packed, plan, contract


def _validate_source_and_pixel_contract(
    input_raw: Path,
    source_pixel_contract_sha256: str,
) -> Path:
    source_pixel_contract_sha256 = _validate_lower_sha256(
        source_pixel_contract_sha256,
        "source pixel contract",
    )
    if (
        source_pixel_contract_sha256
        != EXPECTED_SOURCE_PIXEL_CONTRACT_SHA256
    ):
        raise ValueError("RawNIND source pixel contract changed")
    source = input_raw.resolve()
    if not source.is_file():
        raise ValueError("RawNIND source RAW does not exist")
    return source


def plan_foundation(
    *,
    model_package: Path,
    model_graph: Path,
    manifest_path: Path,
    input_raw: Path,
    source_pixel_contract_sha256: str,
) -> str:
    source = _validate_source_and_pixel_contract(
        input_raw,
        source_pixel_contract_sha256,
    )
    session = verify_model(model_package, model_graph, manifest_path)
    if _is_cancelled():
        raise stripe_lifecycle.StripeCancelled(
            "RawNIND sidecar was cancelled before RAW planning"
        )
    unused_packed, plan, contract = _prepare_foundation(
        session,
        source,
        source_pixel_contract_sha256,
    )
    cache_key = foundation_artifact.artifact_cache_key(
        contract,
        tuple(plan.tiling.output_shape_sensor),
    )
    _, height, width = plan.tiling.output_shape_sensor
    return (
        f"{PLAN_RECEIPT_PREFIX} "
        f"cache_key_sha256={cache_key} "
        f"source_sha256={contract.source_sha256} "
        f"source_size_bytes={contract.source_size_bytes} "
        "source_pixel_contract_sha256="
        f"{contract.source_pixel_contract_sha256} "
        f"width={width} height={height} "
        f"runtime_version={ort.__version__}"
    )


def run_foundation(
    *,
    model_package: Path,
    model_graph: Path,
    manifest_path: Path,
    input_raw: Path,
    output_foundation: Path,
    source_pixel_contract_sha256: str,
) -> str:
    source = _validate_source_and_pixel_contract(
        input_raw,
        source_pixel_contract_sha256,
    )
    output = output_foundation.resolve()
    if output.exists():
        raise ValueError("RawNIND output partial already exists")
    if not output.parent.is_dir():
        raise ValueError("RawNIND output partial parent does not exist")

    created_output = False
    try:
        session = verify_model(model_package, model_graph, manifest_path)
        if _is_cancelled():
            raise stripe_lifecycle.StripeCancelled(
                "RawNIND sidecar was cancelled before RAW decoding"
            )
        packed, plan, contract = _prepare_foundation(
            session,
            source,
            source_pixel_contract_sha256,
        )
        sink = foundation_artifact.OwnedFoundationArtifactPartialSink(
            output,
            contract,
        )

        def run_tile(tile: object) -> object:
            return session.run(None, {"input": tile})[0]

        created_output = True
        striped = stripe_lifecycle.run_striped_image(
            packed,
            run_tile,
            plan,
            sink,
            cancelled=_is_cancelled,
        )
        failures = stripe_lifecycle.gate_failures(striped.receipt)
        if failures:
            raise ValueError(
                "RawNIND foundation quality gate failed: "
                + "; ".join(failures)
            )
        verification = foundation_artifact.verify_artifact(output)
        sink_receipt = striped.receipt.sink
        if (
            sink_receipt.artifact_cache_key_sha256
            != verification.cache_key_sha256
            or sink_receipt.artifact_identity_sha256
            != verification.artifact_identity_sha256
            or sink_receipt.artifact_file_sha256
            != verification.file_sha256
            or sink_receipt.artifact_file_bytes != verification.file_bytes
        ):
            raise ValueError("RawNIND artifact receipt changed after sealing")
        _, height, width = verification.output_shape_sensor
        return (
            f"{FOUNDATION_RECEIPT_PREFIX} "
            f"cache_key_sha256={verification.cache_key_sha256} "
            "artifact_identity_sha256="
            f"{verification.artifact_identity_sha256} "
            f"file_sha256={verification.file_sha256} "
            f"width={width} height={height} "
            f"runtime_version={ort.__version__}"
        )
    except BaseException:
        if created_output:
            output.unlink(missing_ok=True)
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Verify or execute Shadow's pinned RawNIND RAW foundation model"
        )
    )
    parser.add_argument("--model-package", required=True, type=Path)
    parser.add_argument("--model-graph", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--input-raw", type=Path)
    parser.add_argument("--output-foundation", type=Path)
    parser.add_argument("--source-pixel-contract-sha256")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--verify-model", action="store_true")
    mode.add_argument("--plan", action="store_true")
    mode.add_argument("--run", action="store_true")
    return parser


def _fail(message: str) -> NoReturn:
    print(f"RawNIND foundation sidecar failed: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    global _cancelled
    _cancelled = False
    signal.signal(signal.SIGINT, _request_cancellation)
    signal.signal(signal.SIGTERM, _request_cancellation)
    arguments = build_parser().parse_args()
    run_arguments = (
        arguments.input_raw,
        arguments.output_foundation,
        arguments.source_pixel_contract_sha256,
    )
    try:
        if arguments.verify_model:
            if any(value is not None for value in run_arguments):
                raise ValueError(
                    "--verify-model does not accept run-only arguments"
                )
            verify_model(
                arguments.model_package,
                arguments.model_graph,
                arguments.manifest,
            )
            print(model_receipt())
            return 0
        if arguments.plan:
            if (
                arguments.input_raw is None
                or arguments.source_pixel_contract_sha256 is None
                or arguments.output_foundation is not None
            ):
                raise ValueError(
                    "--plan requires --input-raw and "
                    "--source-pixel-contract-sha256, and does not accept "
                    "--output-foundation"
                )
            receipt = plan_foundation(
                model_package=arguments.model_package,
                model_graph=arguments.model_graph,
                manifest_path=arguments.manifest,
                input_raw=arguments.input_raw,
                source_pixel_contract_sha256=(
                    arguments.source_pixel_contract_sha256
                ),
            )
            print(receipt)
            return 0
        if any(value is None for value in run_arguments):
            raise ValueError(
                "--run requires --input-raw, --output-foundation, and "
                "--source-pixel-contract-sha256"
            )
        receipt = run_foundation(
            model_package=arguments.model_package,
            model_graph=arguments.model_graph,
            manifest_path=arguments.manifest,
            input_raw=arguments.input_raw,
            output_foundation=arguments.output_foundation,
            source_pixel_contract_sha256=(
                arguments.source_pixel_contract_sha256
            ),
        )
        print(receipt)
        return 0
    except (
        OSError,
        RuntimeError,
        ValueError,
        stripe_lifecycle.StripeCancelled,
    ) as error:
        _fail(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
