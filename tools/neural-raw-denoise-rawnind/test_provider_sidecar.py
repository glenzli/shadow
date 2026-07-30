from __future__ import annotations

import contextlib
import hashlib
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np

import foundation_artifact
import provider_sidecar


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = (
    REPOSITORY_ROOT
    / "apps/desktop/providers/rawnind-foundation/model-manifest.json"
)


class FakeSession:
    def get_providers(self) -> list[str]:
        return ["CPUExecutionProvider"]

    def run(
        self,
        unused_outputs: object,
        inputs: dict[str, np.ndarray],
    ) -> list[np.ndarray]:
        tile = inputs["input"]
        plane = tile[:, 0:1]
        sensor = np.repeat(np.repeat(plane, 2, axis=2), 2, axis=3)
        return [
            np.ascontiguousarray(np.repeat(sensor, 3, axis=1))
        ]


def synthetic_preprocessing(packed: np.ndarray) -> dict[str, object]:
    return {
        "sensor_shape": [packed.shape[1] * 2, packed.shape[2] * 2],
        "packed_shape": list(packed.shape),
        "raw_pattern": [[0, 1], [3, 2]],
        "source_raw_pattern": [[0, 1], [3, 2]],
        "force_rggb_crop_sensor": [0, 0],
        "color_description": "RGBG",
        "white_level": 16383.0,
        "black_level_per_channel": [512.0, 512.0, 512.0, 512.0],
    }


def receipt_field(receipt: str, name: str) -> str:
    prefix = f"{name}="
    return next(
        field.removeprefix(prefix)
        for field in receipt.split()
        if field.startswith(prefix)
    )


class RawNindProviderSidecarContract(unittest.TestCase):
    def setUp(self) -> None:
        provider_sidecar._cancelled = False

    def test_checked_in_manifest_is_exact(self) -> None:
        manifest = provider_sidecar.verify_manifest(MANIFEST)
        self.assertEqual(
            manifest["model_id"],
            provider_sidecar.EXPECTED_MODEL_ID,
        )

    def test_changed_manifest_bytes_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            changed = Path(directory_name) / "manifest.json"
            changed.write_bytes(MANIFEST.read_bytes() + b"\n")
            with self.assertRaisesRegex(ValueError, "bytes changed"):
                provider_sidecar.verify_manifest(changed)

    def test_model_receipt_matches_rust_protocol(self) -> None:
        self.assertEqual(
            provider_sidecar.model_receipt(),
            (
                "shadow-rawnind-foundation-model-v1 "
                "package_sha256="
                "d71b5f1e727c85a359e6f74dca9e2016c9d8fc3e2f7ac3e9b347d80ceca969af "
                "graph_sha256="
                "da27509dab6a2915da67e988acd86cf71f9d5bbc8d1aa0ed32933578a887b901 "
                "runtime_version=1.24.4"
            ),
        )

    def test_run_writes_one_verified_application_owned_partial(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            directory = Path(directory_name)
            source = directory / "source.raw"
            source.write_bytes(b"synthetic RAW identity")
            output = directory / ".provider-owned.shadowrawf"
            yy, xx = np.mgrid[0:513, 0:513].astype(np.float32)
            signal = 0.1 + yy * 0.0002 + xx * 0.0003
            packed = np.ascontiguousarray(
                np.repeat(signal[None], 4, axis=0).astype(np.float32)
            )
            with (
                mock.patch.object(
                    provider_sidecar,
                    "verify_model",
                    return_value=FakeSession(),
                ),
                mock.patch.object(
                    provider_sidecar.inference,
                    "load_raw_as_packed_bayer",
                    return_value=(
                        packed,
                        synthetic_preprocessing(packed),
                    ),
                ),
            ):
                receipt = provider_sidecar.run_foundation(
                    model_package=directory / "unused.dtmodel",
                    model_graph=directory / "unused.onnx",
                    manifest_path=MANIFEST,
                    input_raw=source,
                    output_foundation=output,
                    source_pixel_contract_sha256=(
                        provider_sidecar.EXPECTED_SOURCE_PIXEL_CONTRACT_SHA256
                    ),
                )
            self.assertTrue(output.is_file())
            self.assertEqual(
                list(directory.glob("*.shadowrawf")),
                [output],
            )
            verification = foundation_artifact.verify_artifact(output)
            self.assertEqual(
                receipt,
                (
                    "shadow-rawnind-foundation-v1 "
                    f"cache_key_sha256={verification.cache_key_sha256} "
                    "artifact_identity_sha256="
                    f"{verification.artifact_identity_sha256} "
                    f"file_sha256={verification.file_sha256} "
                    "width=1026 height=1026 runtime_version=1.24.4"
                ),
            )

    def test_plan_computes_the_exact_cache_key_without_tile_inference(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            directory = Path(directory_name)
            source = directory / "source.raw"
            source_bytes = b"synthetic RAW identity"
            source.write_bytes(source_bytes)
            packed = np.ones((4, 513, 513), dtype=np.float32)

            class PlanOnlySession(FakeSession):
                def run(
                    self,
                    unused_outputs: object,
                    inputs: dict[str, np.ndarray],
                ) -> list[np.ndarray]:
                    raise AssertionError("planning must not run a model tile")

            with (
                mock.patch.object(
                    provider_sidecar,
                    "verify_model",
                    return_value=PlanOnlySession(),
                ),
                mock.patch.object(
                    provider_sidecar.inference,
                    "load_raw_as_packed_bayer",
                    return_value=(
                        packed,
                        synthetic_preprocessing(packed),
                    ),
                ),
            ):
                receipt = provider_sidecar.plan_foundation(
                    model_package=directory / "unused.dtmodel",
                    model_graph=directory / "unused.onnx",
                    manifest_path=MANIFEST,
                    input_raw=source,
                    source_pixel_contract_sha256=(
                        provider_sidecar.EXPECTED_SOURCE_PIXEL_CONTRACT_SHA256
                    ),
                )

            self.assertTrue(
                receipt.startswith(
                    "shadow-rawnind-foundation-plan-v1 "
                )
            )
            self.assertEqual(
                receipt_field(receipt, "source_sha256"),
                hashlib.sha256(source_bytes).hexdigest(),
            )
            self.assertEqual(receipt_field(receipt, "width"), "1026")
            self.assertEqual(receipt_field(receipt, "height"), "1026")
            self.assertEqual(
                receipt_field(
                    receipt,
                    "source_pixel_contract_sha256",
                ),
                provider_sidecar.EXPECTED_SOURCE_PIXEL_CONTRACT_SHA256,
            )

    def test_run_failure_removes_owned_partial(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            directory = Path(directory_name)
            source = directory / "source.raw"
            source.write_bytes(b"synthetic RAW identity")
            output = directory / ".failed.shadowrawf"
            packed = np.ones((4, 289, 291), dtype=np.float32)

            class FailingSession(FakeSession):
                def run(
                    self,
                    unused_outputs: object,
                    inputs: dict[str, np.ndarray],
                ) -> list[np.ndarray]:
                    raise RuntimeError("inference failed")

            with (
                mock.patch.object(
                    provider_sidecar,
                    "verify_model",
                    return_value=FailingSession(),
                ),
                mock.patch.object(
                    provider_sidecar.inference,
                    "load_raw_as_packed_bayer",
                    return_value=(
                        packed,
                        synthetic_preprocessing(packed),
                    ),
                ),
            ):
                with self.assertRaisesRegex(
                    RuntimeError,
                    "inference failed",
                ):
                    provider_sidecar.run_foundation(
                        model_package=directory / "unused.dtmodel",
                        model_graph=directory / "unused.onnx",
                        manifest_path=MANIFEST,
                        input_raw=source,
                        output_foundation=output,
                        source_pixel_contract_sha256=(
                            provider_sidecar.EXPECTED_SOURCE_PIXEL_CONTRACT_SHA256
                        ),
                    )
            self.assertFalse(output.exists())

    def test_run_rejects_a_substituted_source_pixel_contract(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            directory = Path(directory_name)
            source = directory / "source.raw"
            source.write_bytes(b"synthetic RAW identity")
            with self.assertRaisesRegex(
                ValueError,
                "source pixel contract changed",
            ):
                provider_sidecar.run_foundation(
                    model_package=directory / "unused.dtmodel",
                    model_graph=directory / "unused.onnx",
                    manifest_path=MANIFEST,
                    input_raw=source,
                    output_foundation=directory / ".output.shadowrawf",
                    source_pixel_contract_sha256="0" * 64,
                )

    def test_main_rejects_run_only_arguments_in_verify_mode(self) -> None:
        arguments = [
            "provider_sidecar.py",
            "--model-package",
            "package",
            "--model-graph",
            "graph",
            "--manifest",
            "manifest",
            "--input-raw",
            "raw",
            "--verify-model",
        ]
        stderr = io.StringIO()
        with (
            mock.patch("sys.argv", arguments),
            contextlib.redirect_stderr(stderr),
            self.assertRaises(SystemExit) as raised,
        ):
            provider_sidecar.main()
        self.assertEqual(raised.exception.code, 1)
        self.assertIn("run-only arguments", stderr.getvalue())

    @unittest.skipUnless(
        os.environ.get("SHADOW_TEST_RAWNIND_PACKAGE")
        and os.environ.get("SHADOW_TEST_RAWNIND_GRAPH"),
        "external RawNIND package and graph were not supplied",
    )
    def test_real_public_model_preflight(self) -> None:
        session = provider_sidecar.verify_model(
            Path(os.environ["SHADOW_TEST_RAWNIND_PACKAGE"]),
            Path(os.environ["SHADOW_TEST_RAWNIND_GRAPH"]),
            MANIFEST,
        )
        self.assertEqual(
            session.get_providers(),
            ["CPUExecutionProvider"],
        )

    @unittest.skipUnless(
        os.environ.get("SHADOW_TEST_RAWNIND_PACKAGE")
        and os.environ.get("SHADOW_TEST_RAWNIND_GRAPH")
        and os.environ.get("SHADOW_TEST_RAWNIND_RAW"),
        "external RawNIND package, graph, and public RAW were not supplied",
    )
    def test_real_public_model_materializes_verified_foundation(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            output = Path(directory_name) / ".public.shadowrawf"
            plan_receipt = provider_sidecar.plan_foundation(
                model_package=Path(
                    os.environ["SHADOW_TEST_RAWNIND_PACKAGE"]
                ),
                model_graph=Path(
                    os.environ["SHADOW_TEST_RAWNIND_GRAPH"]
                ),
                manifest_path=MANIFEST,
                input_raw=Path(os.environ["SHADOW_TEST_RAWNIND_RAW"]),
                source_pixel_contract_sha256=(
                    provider_sidecar.EXPECTED_SOURCE_PIXEL_CONTRACT_SHA256
                ),
            )
            receipt = provider_sidecar.run_foundation(
                model_package=Path(
                    os.environ["SHADOW_TEST_RAWNIND_PACKAGE"]
                ),
                model_graph=Path(
                    os.environ["SHADOW_TEST_RAWNIND_GRAPH"]
                ),
                manifest_path=MANIFEST,
                input_raw=Path(os.environ["SHADOW_TEST_RAWNIND_RAW"]),
                output_foundation=output,
                source_pixel_contract_sha256=(
                    provider_sidecar.EXPECTED_SOURCE_PIXEL_CONTRACT_SHA256
                ),
            )
            verification = foundation_artifact.verify_artifact(output)
            self.assertIn(
                f"file_sha256={verification.file_sha256}",
                receipt,
            )
            self.assertEqual(
                receipt_field(plan_receipt, "cache_key_sha256"),
                receipt_field(receipt, "cache_key_sha256"),
            )
            self.assertEqual(
                verification.output_shape_sensor,
                [3, 2600, 3908],
            )


if __name__ == "__main__":
    unittest.main()
