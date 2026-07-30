from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import dataset_manifest


METADATA_PATH = Path(
    "/private/tmp/shadow-rawnind-dataverse-metadata.json"
)
DESCRIPTOR_PATH = Path("/private/tmp/shadow-rawnind-dataset.yaml")


@unittest.skipUnless(
    METADATA_PATH.is_file() and DESCRIPTOR_PATH.is_file(),
    "official RawNIND metadata fixtures are external",
)
class RawNindOfficialDatasetManifestContract(unittest.TestCase):
    def test_pilot_is_verified_against_official_holdout_and_inventory(self) -> None:
        manifest = dataset_manifest.build_pilot_manifest(
            METADATA_PATH,
            DESCRIPTOR_PATH,
        )
        dataset_manifest.validate_manifest(manifest)
        self.assertEqual(manifest["selection_contract"]["pair_count"], 10)
        self.assertEqual(manifest["selection_contract"]["raw_file_count"], 20)
        self.assertEqual(
            manifest["selection_contract"]["camera_makes"],
            ["Canon", "Panasonic", "Sony"],
        )
        self.assertEqual(
            len(manifest["selection_contract"]["cameras"]),
            6,
        )
        self.assertEqual(manifest["download"]["total_bytes"], 691_917_871)
        self.assertEqual(
            {pair["scene"] for pair in manifest["pairs"]},
            {selection.scene for selection in dataset_manifest.PILOT_SELECTIONS},
        )

    def test_manifest_identity_rejects_mutation(self) -> None:
        manifest = dataset_manifest.build_pilot_manifest(
            METADATA_PATH,
            DESCRIPTOR_PATH,
        )
        changed = deepcopy(manifest)
        changed["pairs"][0]["noisy_iso"] = 1
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            dataset_manifest.validate_manifest(changed)

    def test_writer_refuses_to_replace_a_manifest(self) -> None:
        manifest = dataset_manifest.build_pilot_manifest(
            METADATA_PATH,
            DESCRIPTOR_PATH,
        )
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "manifest.json"
            dataset_manifest.write_manifest(manifest, output)
            written = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(
                written["manifest_sha256"],
                manifest["manifest_sha256"],
            )
            with self.assertRaisesRegex(ValueError, "must not already exist"):
                dataset_manifest.write_manifest(manifest, output)


class RawNindDatasetManifestContract(unittest.TestCase):
    def test_descriptor_digest_is_required_before_split_parsing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "dataset.yaml"
            path.write_text(
                "Bayer:\n"
                "  invented:\n"
                "    unknown_sensor: false\n"
                "    test_reserve: true\n",
                encoding="utf-8",
            )
            self.assertNotEqual(
                hashlib.md5(
                    path.read_bytes(),
                    usedforsecurity=False,
                ).hexdigest(),
                dataset_manifest.DATASET_DESCRIPTOR.md5,
            )
            with self.assertRaisesRegex(ValueError, "size mismatch"):
                dataset_manifest.parse_pinned_bayer_descriptor(path)


if __name__ == "__main__":
    unittest.main()
