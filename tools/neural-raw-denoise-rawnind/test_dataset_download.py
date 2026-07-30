from __future__ import annotations

import hashlib
from pathlib import Path
import tempfile
import unittest

import dataset_download
import public_pair


class RawNindDatasetDownloadContract(unittest.TestCase):
    def _identity(self, payload: bytes) -> public_pair.PublicRawFile:
        return public_pair.PublicRawFile(
            role="noisy",
            dataverse_file_id=42,
            persistent_id="doi:test/noisy",
            filename="pinned-noisy.dng",
            size_bytes=len(payload),
            md5=hashlib.md5(
                payload,
                usedforsecurity=False,
            ).hexdigest(),
        )

    def test_download_is_verified_and_published_without_overwrite(self) -> None:
        payload = b"official public raw fixture"
        identity = self._identity(payload)
        calls: list[list[str]] = []

        def fake_curl(command: list[str], *, check: bool) -> None:
            self.assertTrue(check)
            calls.append(command)
            output = Path(command[command.index("--output") + 1])
            output.write_bytes(payload)

        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory)
            first = dataset_download.download_one(
                identity,
                destination,
                curl_runner=fake_curl,
            )
            self.assertEqual(first.official_md5, identity.md5)
            self.assertEqual(len(calls), 1)
            self.assertFalse(
                (destination / f".{identity.filename}.partial").exists()
            )
            second = dataset_download.download_one(
                identity,
                destination,
                curl_runner=fake_curl,
            )
            self.assertEqual(second.sha256, first.sha256)
            self.assertEqual(len(calls), 1)

    def test_unverified_download_is_removed_before_retry(self) -> None:
        identity = self._identity(b"expected payload")

        def fake_curl(command: list[str], *, check: bool) -> None:
            self.assertTrue(check)
            output = Path(command[command.index("--output") + 1])
            output.write_bytes(b"wrong")

        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory)
            with self.assertRaisesRegex(ValueError, "size mismatch"):
                dataset_download.download_one(
                    identity,
                    destination,
                    curl_runner=fake_curl,
                )
            self.assertFalse((destination / identity.filename).exists())
            self.assertFalse(
                (destination / f".{identity.filename}.partial").exists()
            )

    def test_existing_corrupt_destination_fails_without_network(self) -> None:
        identity = self._identity(b"expected payload")

        def unexpected_curl(command: list[str], *, check: bool) -> None:
            self.fail(f"curl must not run: {command}, check={check}")

        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory)
            (destination / identity.filename).write_bytes(b"wrong")
            with self.assertRaisesRegex(ValueError, "size mismatch"):
                dataset_download.download_one(
                    identity,
                    destination,
                    curl_runner=unexpected_curl,
                )

    def test_parallelism_is_bounded(self) -> None:
        with self.assertRaisesRegex(ValueError, "between 1 and 8"):
            dataset_download.download_manifest(
                {
                    "manifest_sha256": "unused",
                    "pairs": [],
                },
                Path("/tmp/unused"),
                workers=9,
            )

    def test_range_download_has_exact_absolute_bounds(self) -> None:
        payload = b"0123456789abcdef"
        identity = self._identity(payload)
        observed: list[str] = []

        def fake_curl(command: list[str], *, check: bool) -> None:
            self.assertTrue(check)
            range_text = command[command.index("--range") + 1]
            observed.append(range_text)
            start, end = (int(value) for value in range_text.split("-"))
            output = Path(command[command.index("--output") + 1])
            output.write_bytes(payload[start : end + 1])

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "segment.part"
            result = dataset_download.download_segment(
                identity,
                4,
                9,
                path,
                curl_runner=fake_curl,
            )
            self.assertEqual(result.read_bytes(), payload[4:10])
            self.assertEqual(observed, ["4-9"])
            second = dataset_download.download_segment(
                identity,
                4,
                9,
                path,
                curl_runner=fake_curl,
            )
            self.assertEqual(second, result)
            self.assertEqual(observed, ["4-9"])


if __name__ == "__main__":
    unittest.main()
