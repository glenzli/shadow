from __future__ import annotations

import os
from pathlib import Path
import tempfile
import unittest

import package_contract


class RawNindPackageContract(unittest.TestCase):
    def test_pinned_release_package_is_verified_when_available(self) -> None:
        package = os.environ.get("SHADOW_TEST_RAWNIND_PACKAGE")
        if package is None:
            self.skipTest("set SHADOW_TEST_RAWNIND_PACKAGE for the external release asset")
        receipt = package_contract.verify_package(Path(package))
        self.assertEqual(receipt.package_sha256, package_contract.PACKAGE_SHA256)
        self.assertEqual(set(receipt.member_sha256), set(package_contract.PACKAGE_MEMBERS))

    def test_output_payload_must_stay_outside_the_repository(self) -> None:
        with self.assertRaisesRegex(ValueError, "outside the repository"):
            package_contract.validate_new_output_directory(
                package_contract.REPOSITORY_ROOT / "generated-raw-model"
            )

    def test_existing_output_directory_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "must not already exist"):
                package_contract.validate_new_output_directory(Path(directory))

    def test_manifest_contract_is_exact(self) -> None:
        config = {
            "id": "rawdenoise-nind",
            "task": "rawdenoise",
            "arch": "utnet2",
            "backend": "onnx",
            "version": "1.0",
            "tiling": True,
            "coreml_format": "mlprogram",
            "attributes": {
                "input_sizes": [512],
                "model_bayer": {
                    "input_kind": "bayer_v1",
                    "bayer_orientation": "force_rggb",
                    "edge_pad": "mirror_cropped",
                    "wb_norm": "none",
                    "output_scale": "match_gain",
                },
            },
            "model_card": {
                "license": "GPL-3.0",
                "training_data_license": (
                    "CC BY 4.0 / CC0 (per-image, Wikimedia Commons)"
                ),
            },
        }
        package_contract._validate_config(config)
        config["attributes"]["model_bayer"]["wb_norm"] = "daylight"
        with self.assertRaisesRegex(ValueError, "contract changed"):
            package_contract._validate_config(config)


if __name__ == "__main__":
    unittest.main()
