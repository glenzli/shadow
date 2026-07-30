from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import shutil
import tempfile
import unittest

import numpy as np

import foundation_artifact
import stripe_lifecycle
import tiling


SOURCE_PIXEL_CONTRACT_SHA256 = "f" * 64


def repeat_runner(tile: np.ndarray) -> np.ndarray:
    plane = tile[:, 0:1]
    sensor = np.repeat(np.repeat(plane, 2, axis=2), 2, axis=3)
    return np.ascontiguousarray(np.repeat(sensor, 3, axis=1))


def synthetic_packed(height: int, width: int) -> np.ndarray:
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    signal = (0.1 + yy * 0.0002 + xx * 0.0003).astype(np.float32)
    return np.stack(
        [signal, signal * 0.9, signal * 1.1, signal * 0.8],
        axis=0,
    )


def preprocessing_receipt(
    packed: np.ndarray,
) -> dict[str, object]:
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


def execution_identity() -> dict[str, object]:
    return {
        "engine": "contract-test",
        "runtime_version": "1",
        "requested_provider": "cpu",
        "active_providers": ["contract-test"],
        "platform": "test",
        "machine": "test",
    }


class RawNindFoundationArtifactContract(unittest.TestCase):
    def _produce(
        self,
        directory: Path,
    ) -> tuple[
        np.ndarray,
        tiling.FullImageRun,
        stripe_lifecycle.StripeRun,
        Path,
        foundation_artifact.RawNindFoundationContract,
    ]:
        packed = synthetic_packed(289, 291)
        source = directory / "source.raw"
        source.write_bytes(b"synthetic RAW identity")
        plan = stripe_lifecycle.plan_striped_image(289, 291)
        contract = foundation_artifact.make_contract(
            source,
            preprocessing_receipt(packed),
            plan,
            SOURCE_PIXEL_CONTRACT_SHA256,
            execution=execution_identity(),
        )
        destination = directory / "foundation.shadowrawf"
        sink = foundation_artifact.AtomicFoundationArtifactSink(
            destination,
            contract,
        )
        striped = stripe_lifecycle.run_striped_image(
            packed,
            repeat_runner,
            plan,
            sink,
        )
        reference = tiling.run_full_image(
            packed,
            repeat_runner,
            tiling.plan_full_image(289, 291),
        )
        return packed, reference, striped, destination, contract

    def test_atomic_round_trip_and_random_row_read(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            directory = Path(directory_name)
            _, reference, striped, destination, _ = self._produce(directory)
            self.assertTrue(destination.is_file())
            self.assertEqual(
                list(directory.glob(".*.partial-*")),
                [],
            )
            verification = foundation_artifact.verify_artifact(destination)
            self.assertEqual(
                verification.artifact_identity_sha256,
                striped.receipt.sink.artifact_identity_sha256,
            )
            self.assertEqual(
                verification.file_sha256,
                striped.receipt.sink.artifact_file_sha256,
            )
            self.assertEqual(
                verification.sequence_sha256,
                striped.receipt.sink.sequence_sha256,
            )
            with foundation_artifact.FoundationArtifactReader(
                destination
            ) as reader:
                rows = reader.read_rows(540, 30)
            np.testing.assert_array_equal(
                rows,
                reference.output[:, 540:570],
            )

    def test_payload_tampering_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            directory = Path(directory_name)
            _, _, _, destination, _ = self._produce(directory)
            verification = foundation_artifact.verify_artifact(destination)
            tampered = directory / "tampered.shadowrawf"
            shutil.copyfile(destination, tampered)
            with tampered.open("r+b") as stream:
                stream.seek(verification.stripes[0].offset + 31)
                original = stream.read(1)
                stream.seek(verification.stripes[0].offset + 31)
                stream.write(bytes([original[0] ^ 0x01]))
            with self.assertRaisesRegex(ValueError, "payload digest"):
                foundation_artifact.verify_artifact(tampered)

    def test_completed_partial_can_be_recovered_without_overwrite(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            directory = Path(directory_name)
            _, _, _, destination, _ = self._produce(directory)
            partial = directory / ".recovery.partial"
            recovered = directory / "recovered.shadowrawf"
            shutil.copyfile(destination, partial)
            receipt = foundation_artifact.recover_completed_artifact(
                partial,
                recovered,
            )
            self.assertTrue(receipt.recovered_from_partial)
            self.assertFalse(partial.exists())
            self.assertTrue(recovered.is_file())
            self.assertEqual(
                receipt.artifact_identity_sha256,
                foundation_artifact.verify_artifact(
                    destination
                ).artifact_identity_sha256,
            )
            duplicate = directory / ".duplicate.partial"
            shutil.copyfile(destination, duplicate)
            duplicate_receipt = (
                foundation_artifact.recover_completed_artifact(
                    duplicate,
                    destination,
                )
            )
            self.assertTrue(duplicate_receipt.recovered_from_partial)
            self.assertFalse(duplicate.exists())
            incomplete = directory / ".incomplete.partial"
            incomplete_destination = (
                directory / "incomplete.shadowrawf"
            )
            shutil.copyfile(destination, incomplete)
            with incomplete.open("r+b") as stream:
                stream.truncate(destination.stat().st_size - 17)
            with self.assertRaises(ValueError):
                foundation_artifact.recover_completed_artifact(
                    incomplete,
                    incomplete_destination,
                )
            self.assertTrue(incomplete.exists())
            self.assertFalse(incomplete_destination.exists())

    def test_existing_destination_is_never_overwritten(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            directory = Path(directory_name)
            _, _, _, destination, contract = self._produce(directory)
            before = foundation_artifact.verify_artifact(destination)
            sink = foundation_artifact.AtomicFoundationArtifactSink(
                destination,
                contract,
            )
            with self.assertRaisesRegex(ValueError, "already exists"):
                sink.begin(tuple(before.output_shape_sensor))
            after = foundation_artifact.verify_artifact(destination)
            self.assertEqual(before.file_sha256, after.file_sha256)
            self.assertEqual(
                list(directory.glob(".*.partial-*")),
                [],
            )

    def test_cancellation_removes_partial_and_publishes_nothing(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            directory = Path(directory_name)
            packed = synthetic_packed(289, 291)
            source = directory / "source.raw"
            source.write_bytes(b"synthetic RAW identity")
            plan = stripe_lifecycle.plan_striped_image(289, 291)
            contract = foundation_artifact.make_contract(
                source,
                preprocessing_receipt(packed),
                plan,
                SOURCE_PIXEL_CONTRACT_SHA256,
                execution=execution_identity(),
            )
            destination = directory / "cancelled.shadowrawf"
            sink = foundation_artifact.AtomicFoundationArtifactSink(
                destination,
                contract,
            )
            checks = 0

            def cancelled() -> bool:
                nonlocal checks
                checks += 1
                return checks >= 10

            with self.assertRaises(stripe_lifecycle.StripeCancelled):
                stripe_lifecycle.run_striped_image(
                    packed,
                    repeat_runner,
                    plan,
                    sink,
                    cancelled=cancelled,
                )
            self.assertFalse(destination.exists())
            self.assertEqual(
                list(directory.glob(".*.partial-*")),
                [],
            )

    def test_owned_partial_sink_writes_only_the_caller_path(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            directory = Path(directory_name)
            packed = synthetic_packed(289, 291)
            source = directory / "source.raw"
            source.write_bytes(b"synthetic RAW identity")
            plan = stripe_lifecycle.plan_striped_image(289, 291)
            contract = foundation_artifact.make_contract(
                source,
                preprocessing_receipt(packed),
                plan,
                SOURCE_PIXEL_CONTRACT_SHA256,
                execution=execution_identity(),
            )
            owned_partial = directory / ".application-owned.shadowrawf"
            sink = foundation_artifact.OwnedFoundationArtifactPartialSink(
                owned_partial,
                contract,
            )
            striped = stripe_lifecycle.run_striped_image(
                packed,
                repeat_runner,
                plan,
                sink,
            )

            self.assertTrue(owned_partial.is_file())
            self.assertEqual(
                [path for path in directory.iterdir() if path.name.startswith(".")],
                [owned_partial],
            )
            verification = foundation_artifact.verify_artifact(owned_partial)
            self.assertEqual(
                verification.file_sha256,
                striped.receipt.sink.artifact_file_sha256,
            )

    def test_owned_partial_sink_cancellation_removes_exact_path(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            directory = Path(directory_name)
            packed = synthetic_packed(289, 291)
            source = directory / "source.raw"
            source.write_bytes(b"synthetic RAW identity")
            plan = stripe_lifecycle.plan_striped_image(289, 291)
            contract = foundation_artifact.make_contract(
                source,
                preprocessing_receipt(packed),
                plan,
                SOURCE_PIXEL_CONTRACT_SHA256,
                execution=execution_identity(),
            )
            owned_partial = directory / ".cancelled-owned.shadowrawf"
            sink = foundation_artifact.OwnedFoundationArtifactPartialSink(
                owned_partial,
                contract,
            )
            checks = 0

            def cancelled() -> bool:
                nonlocal checks
                checks += 1
                return checks >= 10

            with self.assertRaises(stripe_lifecycle.StripeCancelled):
                stripe_lifecycle.run_striped_image(
                    packed,
                    repeat_runner,
                    plan,
                    sink,
                    cancelled=cancelled,
                )
            self.assertFalse(owned_partial.exists())

    def test_cache_key_covers_source_pixel_and_execution_identity(self) -> None:
        with tempfile.TemporaryDirectory(
            dir="/private/tmp"
        ) as directory_name:
            directory = Path(directory_name)
            packed = synthetic_packed(289, 291)
            source = directory / "source.raw"
            source.write_bytes(b"synthetic RAW identity")
            plan = stripe_lifecycle.plan_striped_image(289, 291)
            contract = foundation_artifact.make_contract(
                source,
                preprocessing_receipt(packed),
                plan,
                SOURCE_PIXEL_CONTRACT_SHA256,
                execution=execution_identity(),
            )
            shape = tuple(plan.tiling.output_shape_sensor)
            original = foundation_artifact.artifact_cache_key(
                contract,
                shape,
            )
            changed_source = replace(
                contract,
                source_sha256="0" * 64,
            )
            changed_execution = replace(
                contract,
                execution={
                    **contract.execution,
                    "runtime_version": "2",
                },
            )
            changed_pixel_contract = replace(
                contract,
                source_pixel_contract_sha256="e" * 64,
            )
            self.assertNotEqual(
                original,
                foundation_artifact.artifact_cache_key(
                    changed_source,
                    shape,
                ),
            )
            self.assertNotEqual(
                original,
                foundation_artifact.artifact_cache_key(
                    changed_execution,
                    shape,
                ),
            )
            self.assertNotEqual(
                original,
                foundation_artifact.artifact_cache_key(
                    changed_pixel_contract,
                    shape,
                ),
            )


if __name__ == "__main__":
    unittest.main()
