#!/usr/bin/env python3
"""Audit the official public RawNIND package as a Shadow foundation candidate."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import sys
import time

import numpy as np
import onnxruntime as ort

import foundation_artifact
import inference
import package_contract
import quality
import stripe_lifecycle
import tiling


def gate_failures(parity: inference.BackendParity) -> list[str]:
    failures: list[str] = []
    if parity.coreml is None:
        failures.append("CoreML execution provider is unavailable")
        return failures
    if parity.coreml_vs_cpu_max_abs is None or parity.coreml_vs_cpu_max_abs > 1e-5:
        failures.append("CoreML and CPU maximum absolute error exceeds 1e-5")
    if parity.coreml_vs_cpu_rmse is None or parity.coreml_vs_cpu_rmse > 1e-6:
        failures.append("CoreML and CPU RMSE exceeds 1e-6")
    return failures


def run(arguments: argparse.Namespace) -> int:
    started = time.perf_counter()
    receipt = package_contract.verify_package(arguments.package)
    extracted = package_contract.extract_verified_package(
        receipt,
        arguments.output_dir,
    )
    model_path = extracted / "model_bayer.onnx"
    parity, _ = inference.audit_backend_parity(
        model_path,
        arguments.warm_runs,
    )
    failures = gate_failures(parity)

    raw_tile = None
    if arguments.sample_raw is not None:
        requested_provider = "coreml" if parity.coreml is not None else "cpu"
        raw_tile = inference.audit_raw_tile(
            model_path,
            arguments.sample_raw,
            requested_provider,
        )

    pair_quality = None
    if arguments.quality_noisy is not None:
        requested_provider = "coreml" if parity.coreml is not None else "cpu"
        pair_quality = quality.evaluate_public_pair(
            model_path,
            arguments.quality_noisy,
            arguments.quality_ground_truth,
            requested_provider,
            arguments.quality_preview,
        )
        if not pair_quality.improved_on_probe:
            failures.append(
                "RawNIND did not improve MSE on the pinned public pair probe"
            )

    full_image_tiling = None
    if arguments.tiling_raw is not None:
        packed, _ = inference.load_raw_as_packed_bayer(arguments.tiling_raw)
        plan = tiling.plan_full_image(
            packed.shape[1],
            packed.shape[2],
            exact_halo_packed=arguments.tiling_exact_halo,
            blend_overlap_packed=arguments.tiling_blend_overlap,
            max_working_bytes=arguments.tiling_max_working_mib
            * 1024
            * 1024,
        )
        requested_provider = "coreml" if parity.coreml is not None else "cpu"
        session, session_load_ms = inference.create_session(
            model_path,
            requested_provider,
        )

        def run_tiling_tile(tile: np.ndarray) -> np.ndarray:
            return session.run(None, {"input": tile})[0]

        tiled = tiling.run_full_image(packed, run_tiling_tile, plan)
        full_image_tiling = {
            "source": str(arguments.tiling_raw.resolve()),
            "provider": requested_provider,
            "active_providers": session.get_providers(),
            "session_load_ms": session_load_ms,
            "receipt": tiling.as_json(tiled.receipt),
        }
        failures.extend(tiling.gate_failures(tiled.receipt))
        del tiled

    full_image_stripe_lifecycle = None
    artifact_verification = None
    stripe_failures: list[str] = []
    if arguments.stripe_raw is not None:
        packed, raw_metadata = inference.load_raw_as_packed_bayer(
            arguments.stripe_raw
        )
        plan = stripe_lifecycle.plan_striped_image(
            packed.shape[1],
            packed.shape[2],
            exact_halo_packed=arguments.tiling_exact_halo,
            blend_overlap_packed=arguments.tiling_blend_overlap,
            max_additional_working_bytes=(
                arguments.stripe_max_additional_working_mib
                * 1024
                * 1024
            ),
        )
        requested_provider = "coreml" if parity.coreml is not None else "cpu"
        session, session_load_ms = inference.create_session(
            model_path,
            requested_provider,
        )

        def run_stripe_tile(tile: np.ndarray) -> np.ndarray:
            return session.run(None, {"input": tile})[0]

        execution_identity = {
            "engine": "onnxruntime",
            "runtime_version": ort.__version__,
            "requested_provider": requested_provider,
            "active_providers": session.get_providers(),
            "platform": platform.platform(),
            "machine": platform.machine(),
        }
        if arguments.foundation_artifact is not None:
            artifact_contract = foundation_artifact.make_contract(
                arguments.stripe_raw,
                raw_metadata,
                plan,
                (
                    "e1998069001c14d01251cc3d6e2bc2aa66b807f3f17d246e7ee7270528302f7f"
                ),
                execution=execution_identity,
            )
            sink = foundation_artifact.AtomicFoundationArtifactSink(
                arguments.foundation_artifact,
                artifact_contract,
            )
        else:
            sink = stripe_lifecycle.DigestStripeSink()
        striped = stripe_lifecycle.run_striped_image(
            packed,
            run_stripe_tile,
            plan,
            sink,
        )
        stripe_failures = stripe_lifecycle.gate_failures(
            striped.receipt
        )
        if arguments.foundation_artifact is not None:
            artifact_verification = foundation_artifact.verify_artifact(
                arguments.foundation_artifact
            )
            if (
                artifact_verification.sequence_sha256
                != striped.receipt.sink.sequence_sha256
            ):
                stripe_failures.append(
                    "foundation artifact differs from the stripe producer"
                )
        full_image_stripe_lifecycle = {
            "source": str(arguments.stripe_raw.resolve()),
            "provider": requested_provider,
            "active_providers": session.get_providers(),
            "session_load_ms": session_load_ms,
            "gate_failures": stripe_failures,
            "receipt": stripe_lifecycle.as_json(striped.receipt),
            "artifact_verification": (
                foundation_artifact.as_json(artifact_verification)
                if artifact_verification is not None
                else None
            ),
        }
        failures.extend(stripe_failures)

    if artifact_verification is not None:
        next_gate = (
            "run a camera-diverse public paired-RAW benchmark, then implement "
            "the native verified artifact reader and Shadow cache registration"
            if pair_quality is not None
            else (
                "evaluate the pinned public noisy/clean pair and a "
                "camera-diverse public benchmark before native cache wiring"
            )
        )
    elif full_image_stripe_lifecycle is not None:
        next_gate = (
            "define a recoverable cached RAW-foundation artifact and run "
            "a camera-diverse public paired-RAW benchmark before production wiring"
            if pair_quality is not None
            else (
                "evaluate the pinned public noisy/clean pair, then define a "
                "recoverable cached RAW-foundation artifact and run a "
                "camera-diverse public benchmark before production wiring"
            )
        )
    elif full_image_tiling is not None:
        next_gate = (
            "run a camera-diverse public paired-RAW benchmark and replace "
            "the audit's full-frame accumulator with a bounded stripe "
            "lifecycle before defining the cached RAW-foundation artifact"
            if pair_quality is not None
            else (
                "evaluate the pinned public noisy/clean pair, then implement "
                "the bounded stripe lifecycle before production wiring"
            )
        )
    elif pair_quality is not None:
        next_gate = (
            "implement and validate full-image overlap/blending, then run a "
            "camera-diverse public paired-RAW benchmark before defining the "
            "cached RAW-foundation artifact"
        )
    else:
        next_gate = (
            "evaluate the pinned public noisy/clean pair, then implement "
            "full-image overlap/blending before production wiring"
        )

    report = {
        "schema": "shadow-rawnind-public-foundation-audit-v3",
        "status": (
            (
                "foundation_candidate_artifact_gate_passed"
                if artifact_verification is not None
                else (
                    "foundation_candidate_stripe_lifecycle_gate_passed"
                    if full_image_stripe_lifecycle is not None
                    else (
                        "foundation_candidate_full_image_gate_passed"
                        if full_image_tiling is not None
                        else (
                            "foundation_candidate_public_probe_gate_passed"
                            if pair_quality is not None
                            else "foundation_candidate_runtime_gate_passed"
                        )
                    )
                )
            )
            if not failures
            else (
                "foundation_candidate_artifact_gate_failed"
                if artifact_verification is not None
                else (
                    "foundation_candidate_stripe_lifecycle_gate_failed"
                    if full_image_stripe_lifecycle is not None
                    else (
                        "foundation_candidate_full_image_gate_failed"
                        if full_image_tiling is not None
                        else (
                            "foundation_candidate_public_probe_gate_failed"
                            if pair_quality is not None
                            else "foundation_candidate_runtime_gate_failed"
                        )
                    )
                )
            )
        ),
        "product_eligible": False,
        "foundation_candidate": not failures,
        "legacy_mosaic_node_compatible": False,
        "gate_failures": failures,
        "upstream": {
            "repository": package_contract.UPSTREAM_REPOSITORY,
            "revision": package_contract.UPSTREAM_REVISION,
            "release": package_contract.UPSTREAM_RELEASE,
            "training_repository": package_contract.TRAINING_REPOSITORY,
            "training_revision": package_contract.TRAINING_REVISION,
            "package_sha256": receipt.package_sha256,
            "members": receipt.member_sha256,
        },
        "licenses": {
            "model_and_training_code": "GPL-3.0",
            "model_card_training_data": "CC BY 4.0 / CC0 per image",
            "RawNIND_Dataverse_dataset": "CC-BY-SA-4.0",
            "shadow_compatibility": "compatible with GPL-3.0-or-later",
        },
        "contract": {
            "input": {
                "name": "input",
                "shape": [1, 4, 512, 512],
                "dtype": "float32",
                "channels": ["R", "G1", "G2", "B"],
                "normalization": "per-site-black-to-white-range",
                "white_balance": "none",
                "noise_conditioning": "none",
            },
            "output": {
                "name": "output",
                "shape": [1, 3, 1024, 1024],
                "dtype": "float32",
                "space": "linear-camera-rgb",
                "demosaiced": True,
                "scale_policy": "match-output-mean-to-input-mean",
            },
            "semantic_boundary": "raw-foundation-materialization",
        },
        "tiling": {
            "static_tile_edge_packed": inference.TILE_EDGE,
            "output_scale": inference.OUTPUT_SCALE,
            "theoretical_receptive_field_packed": (
                inference.MODEL_RECEPTIVE_FIELD_PACKED
            ),
            "conservative_exact_halo_packed": (
                inference.CONSERVATIVE_EXACT_HALO_PACKED
            ),
            "darktable_overlap_packed": inference.DARKTABLE_OVERLAP_PACKED,
            "published_demo_overlap_packed": inference.DEMO_OVERLAP_PACKED,
            "production_tiling_admitted": False,
            "bounded_stripe_lifecycle_admitted": (
                full_image_stripe_lifecycle is not None
                and not stripe_failures
            ),
            "durable_foundation_artifact_admitted": (
                artifact_verification is not None
                and not stripe_failures
            ),
        },
        "runtime": {
            "onnxruntime": ort.__version__,
            "available_providers": ort.get_available_providers(),
            "backend_parity": inference.as_json(parity),
        },
        "sample_raw_tile": (
            inference.as_json(raw_tile) if raw_tile is not None else None
        ),
        "public_pair_quality_probe": (
            quality.as_json(pair_quality) if pair_quality is not None else None
        ),
        "public_pair_quality_preview": (
            str(arguments.quality_preview.resolve())
            if arguments.quality_preview is not None
            else None
        ),
        "full_image_tiling": full_image_tiling,
        "full_image_stripe_lifecycle": full_image_stripe_lifecycle,
        "environment": {
            "python": platform.python_version(),
            "platform": platform.platform(),
            "machine": platform.machine(),
            "numpy": np.__version__,
            "tmpdir": os.environ.get("TMPDIR"),
        },
        "limitations": [
            (
                "one public noisy/clean crop is a bounded probe, not a "
                "camera-diverse quality benchmark"
                if pair_quality is not None
                else "no public noisy/clean quality probe was evaluated"
            ),
            "the Bayer graph jointly denoises and demosaics instead of returning a mosaic",
            "the official package is ONNX and Shadow production currently uses direct Core ML",
            (
                "the verified artifact is not yet registered with Shadow's "
                "cache or readable by the native render pipeline"
                if artifact_verification is not None
                else (
                    "the bounded stripe audit uses a digest-only atomic sink; "
                    "it does not yet define the durable cached foundation format"
                    if full_image_stripe_lifecycle is not None
                    else (
                        "the full-image audit uses a bounded in-memory accumulator, "
                        "not a production stripe writer with cancellation and recovery"
                        if full_image_tiling is not None
                        else (
                            "fixed-tile runtime parity does not admit full-image "
                            "overlap or per-tile gain policy"
                        )
                    )
                )
            ),
            (
                "the artifact deliberately stores lossless float32 reference "
                "pixels; compression and reduced precision are not yet admitted"
                if artifact_verification is not None
                else "no durable foundation artifact was evaluated"
            ),
            (
                "bounded memory currently costs two deterministic inference "
                "passes so one global gain can be applied without a full-frame "
                "or temporary unscaled output"
                if full_image_stripe_lifecycle is not None
                else "no bounded two-pass stripe lifecycle was evaluated"
            ),
            "the model is not bundled, selected by default, or exposed in UI",
        ],
        "next_gate": next_gate,
        "wall_time_seconds": time.perf_counter() - started,
    }
    report_path = arguments.output_dir.resolve() / "report.json"
    report_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if not failures else 1


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--package",
        type=Path,
        required=True,
        help="pinned rawdenoise-nind.dtmodel release asset",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="new external directory for verified extraction and report",
    )
    parser.add_argument(
        "--sample-raw",
        type=Path,
        help="optional one-file Bayer RAW tile admission sample",
    )
    parser.add_argument(
        "--quality-noisy",
        type=Path,
        help="official pinned RawNIND Canon 7D-1 ISO 12800 RAW",
    )
    parser.add_argument(
        "--quality-ground-truth",
        type=Path,
        help="official pinned RawNIND Canon 7D-1 ISO 100 ground-truth RAW",
    )
    parser.add_argument(
        "--quality-preview",
        type=Path,
        help="optional new external PPM: ground truth | noisy | denoised",
    )
    parser.add_argument(
        "--warm-runs",
        type=int,
        default=3,
        help="timed warm predictions per backend",
    )
    parser.add_argument(
        "--tiling-raw",
        type=Path,
        help="optional Bayer RAW for full-image seam and gain validation",
    )
    parser.add_argument(
        "--tiling-exact-halo",
        type=int,
        default=tiling.DEFAULT_EXACT_HALO_PACKED,
        help="trusted model halo in packed pixels",
    )
    parser.add_argument(
        "--tiling-blend-overlap",
        type=int,
        default=tiling.DEFAULT_BLEND_OVERLAP_PACKED,
        help="per-side packed overlap; must exceed the trusted halo",
    )
    parser.add_argument(
        "--tiling-max-working-mib",
        type=int,
        default=768,
        help="admission limit for the audit's in-memory full-image run",
    )
    parser.add_argument(
        "--stripe-raw",
        type=Path,
        help="optional Bayer RAW for bounded two-pass stripe validation",
    )
    parser.add_argument(
        "--stripe-max-additional-working-mib",
        type=int,
        default=256,
        help="admission limit excluding borrowed packed input and sink storage",
    )
    parser.add_argument(
        "--foundation-artifact",
        type=Path,
        help="optional new external .shadowrawf destination for stripe output",
    )
    arguments = parser.parse_args()
    if arguments.warm_runs < 0:
        parser.error("--warm-runs must be non-negative")
    if (arguments.quality_noisy is None) != (
        arguments.quality_ground_truth is None
    ):
        parser.error(
            "--quality-noisy and --quality-ground-truth must be provided together"
        )
    if arguments.quality_preview is not None and arguments.quality_noisy is None:
        parser.error("--quality-preview requires the paired quality inputs")
    if arguments.tiling_exact_halo < 0:
        parser.error("--tiling-exact-halo must be non-negative")
    if arguments.tiling_max_working_mib <= 0:
        parser.error("--tiling-max-working-mib must be positive")
    if arguments.stripe_max_additional_working_mib <= 0:
        parser.error(
            "--stripe-max-additional-working-mib must be positive"
        )
    if (
        arguments.foundation_artifact is not None
        and arguments.stripe_raw is None
    ):
        parser.error("--foundation-artifact requires --stripe-raw")
    return arguments


if __name__ == "__main__":
    try:
        sys.exit(run(parse_arguments()))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"RawNIND audit failed: {error}", file=sys.stderr)
        sys.exit(2)
