from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

import numpy as np


OWNER_ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(OWNER_ROOT))


def load_owner(name: str):
    specification = importlib.util.spec_from_file_location(name, OWNER_ROOT / f"{name}.py")
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


linear = load_owner("linear_image")
objective = load_owner("objective_metrics")


class ObjectiveMetricsContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="shadow-objective-test-")
        self.root = pathlib.Path(self.temporary.name)
        height, width = 48, 64
        y, x = np.mgrid[0:height, 0:width].astype(np.float32)
        base = 0.1 + 0.5 * x / width + 0.2 * y / height
        self.reference = np.stack((base * 1.05, base, base * 0.95), axis=2)
        self.reference[16:32, 24:40] = np.array([1.2, 1.15, 1.1], dtype=np.float32)
        self.candidate = self.reference * 0.5
        self.candidate[16:32, 24:40, 0] += 0.08
        self.reference_path = self.root / "reference.pfm"
        self.candidate_path = self.root / "candidate.pfm"
        linear.write_pfm(self.reference_path, self.reference)
        linear.write_pfm(self.candidate_path, self.candidate)
        self.region_path = self.root / "region.json"
        self.region_path.write_text(
            json.dumps(
                {
                    "schema": objective.REGION_SCHEMA,
                    "id": "synthetic-clipped-core",
                    "crop_normalized": [0.0, 0.0, 1.0, 1.0],
                    "clipped_core_rectangles": [[0.375, 0.333, 0.25, 0.334]],
                    "reliable_exterior_rectangles": [
                        [0.0, 0.0, 1.0, 0.2],
                        [0.0, 0.8, 1.0, 0.2]
                    ],
                    "boundary_radius_pixels": 3
                }
            ),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_pfm_round_trip_preserves_extended_linear_values(self) -> None:
        image = linear.read_linear_image(
            self.reference_path, self.root / "working", (20, 12, 24, 24)
        )
        self.assertEqual(image.source_format, "pfm-rgb-f32-linear")
        self.assertEqual(image.pixels.shape, (24, 24, 3))
        np.testing.assert_allclose(image.pixels, self.reference[12:36, 20:44])
        self.assertGreater(float(np.max(image.pixels)), 1.0)
        vkdt_dialect = linear.read_pfm(
            self.reference_path, orientation="top-down"
        )
        np.testing.assert_allclose(vkdt_dialect.pixels, self.reference[::-1])
        self.assertEqual(
            vkdt_dialect.receipt["pfm_file_row_orientation"], "top-down"
        )

    def test_registration_recovers_integer_translation(self) -> None:
        shifted = np.zeros_like(self.reference)
        shifted[2:, :-3] = self.reference[:-2, 3:]
        registered_reference, registered_candidate, receipt = objective.register_translation(
            self.reference, shifted, 4
        )
        self.assertEqual(receipt["dx_pixels"], 3)
        self.assertEqual(receipt["dy_pixels"], -2)
        np.testing.assert_allclose(registered_reference, registered_candidate, atol=1.0e-6)

    def test_analysis_emits_region_metrics_masks_and_resource_receipt(self) -> None:
        output_root = self.root / "analyses"
        status = objective.main(
            [
                "--reference",
                str(self.reference_path),
                "--candidate",
                f"synthetic={self.candidate_path}",
                "--candidate-transfer",
                "synthetic=linear",
                "--region-spec",
                str(self.region_path),
                "--output-root",
                str(output_root),
                "--run-name",
                "synthetic-analysis",
                "--normalization",
                "exterior-rgb",
                "--max-shift",
                "0",
            ]
        )
        self.assertEqual(status, 0)
        analysis_path = output_root / "synthetic-analysis" / "analysis.json"
        document = json.loads(analysis_path.read_text(encoding="utf-8"))
        self.assertEqual(document["schema"], objective.ANALYSIS_SCHEMA)
        self.assertEqual(document["linear_contract"]["transfer"], "linear")
        candidate = document["candidates"][0]
        for scale in candidate["exposure_white_normalization"]["candidate_scale_rgb"]:
            self.assertAlmostEqual(scale, 2.0, places=5)
        self.assertGreater(
            candidate["metrics"]["false_colour_exterior"]["exterior_pixel_count"], 0
        )
        self.assertIn("boundary_continuity", candidate["metrics"])
        self.assertEqual(len(candidate["artifacts"]), 8)
        for artifact in candidate["artifacts"]:
            self.assertTrue(
                (output_root / "synthetic-analysis" / artifact["path"]).is_file()
            )
        serialized = json.dumps(document)
        self.assertNotIn(str(self.reference_path), serialized)
        self.assertNotIn(str(self.candidate_path), serialized)
        self.assertGreater(document["resource_usage"]["analysis_peak_rss_bytes"], 0)

    def test_analysis_directory_is_immutable(self) -> None:
        output_root = self.root / "immutable"
        arguments = [
            "--reference",
            str(self.reference_path),
            "--candidate",
            f"synthetic={self.candidate_path}",
            "--candidate-transfer",
            "synthetic=linear",
            "--region-spec",
            str(self.region_path),
            "--output-root",
            str(output_root),
            "--run-name",
            "once",
            "--normalization",
            "exterior-rgb",
            "--max-shift",
            "0",
        ]
        self.assertEqual(objective.main(arguments), 0)
        self.assertEqual(objective.main(arguments), 2)


if __name__ == "__main__":
    unittest.main()
