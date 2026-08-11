#!/usr/bin/env python3
"""Validate the RawNIND execution-migration baseline without running a model."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

import foundation_artifact
import inference
import provider_sidecar
import raw_frame_staging
import stripe_lifecycle
import tiling


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
REPOSITORY_ROOT = SCRIPT_DIRECTORY.parents[1]
DEFAULT_BASELINE = (
    REPOSITORY_ROOT
    / "apps"
    / "desktop"
    / "providers"
    / "rawnind-foundation"
    / "execution-baseline.json"
)
MODEL_MANIFEST = DEFAULT_BASELINE.with_name("model-manifest.json")
BASELINE_SCHEMA = "shadow.rawnind.execution-baseline@20260811.1"
VALIDATION_SCHEMA = "shadow.rawnind.execution-baseline-validation@20260811.1"

TOP_LEVEL_FIELDS = {
    "schema",
    "status",
    "legacy_route",
    "target_route",
    "model",
    "input",
    "adapter_semantics",
    "output",
    "ownership",
    "transport",
    "routing_policy",
    "parity_and_performance_gates",
}


def _load_object(path: Path, label: str) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"{label} is not readable JSON") from error
    if not isinstance(payload, dict):
        raise ValueError(f"{label} must be a JSON object")
    return payload


def _canonical_bytes(payload: dict[str, Any]) -> bytes:
    return json.dumps(
        payload,
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _artifact_digest(manifest: dict[str, Any], role: str) -> str:
    artifact_set = manifest.get("artifact_set")
    if not isinstance(artifact_set, dict):
        raise ValueError("model manifest artifact_set changed")
    artifacts = artifact_set.get("artifacts")
    if not isinstance(artifacts, list):
        raise ValueError("model manifest artifacts changed")
    matches = [
        artifact.get("sha256")
        for artifact in artifacts
        if isinstance(artifact, dict) and artifact.get("role") == role
    ]
    if len(matches) != 1 or not isinstance(matches[0], str):
        raise ValueError(f"model manifest {role} artifact changed")
    return matches[0]


def _expected_model(manifest: dict[str, Any]) -> dict[str, Any]:
    execution_targets = manifest.get("execution_targets")
    if not isinstance(execution_targets, list) or len(execution_targets) != 1:
        raise ValueError("model manifest execution target changed")
    target = execution_targets[0]
    if not isinstance(target, dict):
        raise ValueError("model manifest execution target changed")
    artifact_set = manifest.get("artifact_set")
    if not isinstance(artifact_set, dict):
        raise ValueError("model manifest artifact set changed")
    return {
        "artifact_set_blake3": artifact_set.get("inventory_blake3"),
        "exact_revision": manifest.get("exact_revision"),
        "execution_provider": "CPUExecutionProvider",
        "format": manifest.get("format"),
        "graph_sha256": _artifact_digest(manifest, "model_definition"),
        "id": manifest.get("model_id"),
        "manifest_sha256": _sha256_file(MODEL_MANIFEST),
        "opset": manifest.get("opset"),
        "package_sha256": _artifact_digest(manifest, "auxiliary"),
        "precision": target.get("precisions", [None])[0],
        "runtime": target.get("minimum_runtime_version"),
    }


def _expected_adapter_semantics() -> dict[str, Any]:
    plan = tiling.plan_image_geometry(1024, 1536)
    stripe_plan = stripe_lifecycle.plan_striped_image(1024, 1536)
    return {
        "input": {
            "channel_order": ["R", "G1", "G2", "B"],
            "dtype": "float32",
            "name": "input",
            "normalization": "per-cfa-site-black-to-white-range-clipped",
            "shape": [
                1,
                inference.INPUT_CHANNELS,
                inference.TILE_EDGE,
                inference.TILE_EDGE,
            ],
            "white_balance": "none",
        },
        "output": {
            "channel_order": ["R", "G", "B"],
            "dtype": "float32",
            "name": "output",
            "shape": [
                1,
                inference.OUTPUT_CHANNELS,
                inference.TILE_EDGE * inference.OUTPUT_SCALE,
                inference.TILE_EDGE * inference.OUTPUT_SCALE,
            ],
            "space": "linear-camera-rgb",
        },
        "tiling": {
            "blend_overlap_packed": plan.blend_overlap_packed,
            "blend_width_packed": plan.blend_width_packed,
            "exact_halo_packed": plan.exact_halo_packed,
            "padding": "numpy-reflect-direct-index",
            "pool_alignment_packed": plan.pool_alignment_packed,
            "step_packed": plan.step_packed,
            "tile_edge_packed": plan.tile_edge_packed,
        },
        "lifecycle": {
            "inference_passes": stripe_plan.inference_passes,
            "max_additional_working_bytes": (
                stripe_plan.max_additional_working_bytes
            ),
            "producer": "rawnind-bayer-two-pass-stripe-v1",
            "scale_policy": "one-global-output-mean-to-input-mean",
            "source_extraction": stripe_plan.source_extraction,
            "stripe_format": stripe_lifecycle.STRIPE_SEQUENCE_FORMAT,
        },
    }


def _validate_migration_policy(baseline: dict[str, Any]) -> None:
    if baseline["legacy_route"] != {
        "adapter_revision": "rawnind-foundation-sidecar-protocol-20260731.2",
        "provider_id": "shadow.rawnind.foundation-sidecar",
    }:
        raise ValueError("execution baseline legacy route changed")
    if baseline["target_route"] != {
        "artifact_lease_protocol": "infer-runtime.artifact-lease@20260811.1",
        "contract": "infer.raw.foundation@20260811.1",
        "endpoint": "POST /infer/v1/raw/foundations",
        "intent": "raw.materialize_foundation",
    }:
        raise ValueError("execution baseline target route changed")

    if baseline["ownership"] != {
        "infer_runtime": [
            "exact-build-and-model-discovery-without-download",
            "rawnind-adapter-semantics",
            "onnxruntime-session-and-provider-lifecycle",
            "resource-admission-and-queue",
            "execution-progress-cancellation-errors-and-provenance",
        ],
        "shadow": [
            "raw-container-decode-and-provider-neutral-staging",
            "recipe-source-revision-and-stale-result-arbitration",
            "cache-lookup-index-verification-publication-and-recovery",
            "preview-detail-export-and-user-interface",
        ],
    }:
        raise ValueError("execution baseline ownership changed")

    transport = baseline["transport"]
    if transport != {
        "input_handle": "read-only-stable-open-object",
        "input_integrity": (
            "consumer-stability-promise-plus-runtime-identity-size-and-"
            "actual-read-digest"
        ),
        "linux_macos": "uds-scm-rights",
        "no_arbitrary_paths": True,
        "no_per_tile_public_rpc": True,
        "no_second_full_input_or_output": True,
        "output_exclusivity": (
            "cooperative-owner-only-not-adversarial-same-uid"
        ),
        "output_handle": "empty-cooperative-exclusive-writable-open-object",
        "lease_binding": "one-shot-app-job-generation-ttl",
        "windows": "owner-only-named-pipe-duplicated-handle",
    }:
        raise ValueError("execution baseline transport safety changed")

    routing = baseline["routing_policy"]
    if not isinstance(routing, dict) or routing != {
        "cache_hit_before_runtime": True,
        "cache_hit_loads_onnxruntime": False,
        "cache_hit_may_contact_runtime": False,
        "legacy_and_infer_route_identities_are_distinct": True,
        "silent_legacy_fallback": False,
    }:
        raise ValueError("execution baseline routing policy changed")

    gates = baseline["parity_and_performance_gates"]
    if not isinstance(gates, dict):
        raise ValueError("execution baseline gates changed")
    if gates.get("artifact_parity") != {
        "fallback": (
            "exact-stripe-and-pixel-semantics-with-new-artifact-revision"
        ),
        "preferred": "byte-identical-shadowrawf",
    }:
        raise ValueError("execution baseline artifact parity gate changed")
    if gates.get("cpu_tile_throughput_decrease_percent_max") != 3.0:
        raise ValueError("execution baseline CPU throughput gate changed")
    if gates.get("extra_private_dirty_bytes_max") != 64 * 1024 * 1024:
        raise ValueError("execution baseline memory gate changed")
    if gates.get("soak") != {"duration_hours": 24, "orphan_leases_max": 0}:
        raise ValueError("execution baseline soak gate changed")
    if gates.get("cancellation") != {
        "additional_stop_latency_ms_max": 250,
        "percentile": 95,
        "rule": "one-tile-time-plus-additional-latency",
    }:
        raise ValueError("execution baseline cancellation gate changed")
    if gates.get("warm_end_to_end_overhead") != {
        "p50_relative_percent_max": 3.0,
        "p95_additional_latency_ms_max": 500,
        "p95_relative_percent_max": 5.0,
        "p95_rule": "relative-or-absolute-whichever-is-looser",
    }:
        raise ValueError("execution baseline warm latency gate changed")


def validate_baseline(path: Path = DEFAULT_BASELINE) -> dict[str, Any]:
    baseline = _load_object(path, "execution baseline")
    if set(baseline) != TOP_LEVEL_FIELDS:
        raise ValueError("execution baseline top-level fields changed")
    if baseline["schema"] != BASELINE_SCHEMA:
        raise ValueError("execution baseline schema changed")
    if baseline["status"] != "phase-0-candidate":
        raise ValueError("execution baseline status changed")

    manifest = _load_object(MODEL_MANIFEST, "model manifest")
    if baseline["model"] != _expected_model(manifest):
        raise ValueError("execution baseline model contract drifted")
    if baseline["input"] != {
        "descriptor_contract": "active-camera-colour-20260806.1",
        "payload_dtype": "uint16-little-endian",
        "payload_suffix": ".u16le",
        "source_pixel_contract_sha256": (
            provider_sidecar.EXPECTED_SOURCE_PIXEL_CONTRACT_SHA256
        ),
        "staging_schema": raw_frame_staging.SCHEMA,
    }:
        raise ValueError("execution baseline RawFrame input contract drifted")
    if baseline["adapter_semantics"] != _expected_adapter_semantics():
        raise ValueError("execution baseline adapter semantics drifted")
    if baseline["output"] != {
        "artifact_schema": foundation_artifact.ARTIFACT_SCHEMA,
        "cache_key_schema": foundation_artifact.CACHE_KEY_SCHEMA,
        "header_schema": foundation_artifact.HEADER_SCHEMA,
        "implementation_revision": foundation_artifact.IMPLEMENTATION_REVISION,
        "publication_owner": "shadow",
        "verification_owner": "shadow",
    }:
        raise ValueError("execution baseline output contract drifted")
    _validate_migration_policy(baseline)
    return baseline


def validation_receipt(
    baseline: dict[str, Any],
    baseline_path: Path = DEFAULT_BASELINE,
) -> dict[str, Any]:
    return {
        "schema": VALIDATION_SCHEMA,
        "baseline_schema": baseline["schema"],
        "baseline_canonical_json_sha256": _sha256_bytes(
            _canonical_bytes(baseline)
        ),
        "baseline_file_sha256": _sha256_file(baseline_path),
        "result": "validated",
        "checks": [
            "model-manifest",
            "raw-frame-staging",
            "adapter-semantics",
            "foundation-artifact",
            "transport-and-routing-policy",
            "parity-and-performance-gates",
        ],
        "contains_payload_paths": False,
    }


def write_external_receipt(path: Path, receipt: dict[str, Any]) -> None:
    destination = path.resolve()
    try:
        destination.relative_to(REPOSITORY_ROOT)
    except ValueError:
        pass
    else:
        raise ValueError("validation receipt must stay outside the repository")
    destination.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(receipt, indent=2, sort_keys=True) + "\n"
    with destination.open("x", encoding="utf-8") as stream:
        stream.write(payload)


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument("--receipt", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = _arguments()
    baseline = validate_baseline(arguments.baseline)
    receipt = validation_receipt(baseline, arguments.baseline)
    if arguments.receipt is not None:
        write_external_receipt(arguments.receipt, receipt)
    print(json.dumps(receipt, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
