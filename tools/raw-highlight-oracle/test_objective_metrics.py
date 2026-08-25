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
topology = load_owner("cfa_topology")
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

    def test_factual_empty_core_preserves_unclipped_control_exterior(self) -> None:
        region = json.loads(self.region_path.read_text(encoding="utf-8"))
        masks = objective.build_masks(
            (48, 64),
            region,
            np.zeros((48, 64), dtype=bool),
        )
        self.assertEqual(int(np.count_nonzero(masks["clipped_core"])), 0)
        self.assertEqual(int(np.count_nonzero(masks["boundary_band"])), 0)
        self.assertGreater(int(np.count_nonzero(masks["reliable_exterior"])), 0)

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
        self.assertEqual(
            candidate["clipped_core_source"]["mode"], "manual-region-rectangles"
        )
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

    def test_analysis_projects_factual_cfa_topology_into_registered_crop(self) -> None:
        staging = self.root / "source.shadowrawi"
        samples = np.full((48, 64), 20, dtype="<u2")
        samples[16:32, 24:40] = 100
        sample_path = pathlib.Path(f"{staging}.u16le")
        samples.tofile(sample_path)
        fields = {
            "descriptor_contract": "active-camera-colour-response-20260822.1",
            "width": "64",
            "height": "48",
            "cfa": "RGGB",
            "black": "10,10,10,10",
            "white": "100,100,100,100",
            "linear_response": "90,90,90,90",
            "has_linear_response": "1",
            "orientation": "0",
            "bits_per_sample": "16",
            "as_shot_neutral": "1,1,1,1",
            "camera_to_xyz_d50": "-",
            "xyz_to_camera_d65": "-",
            "camera_to_linear_srgb_d65": "-",
            "pending_dng_opcode_bytes": "0,0,0",
            "provider_id_hex": "74657374",
            "provider_version_hex": "31",
            "sample_bytes": str(samples.nbytes),
        }
        staging.write_text(
            "shadow-raw-frame-staging-20260822.1 "
            + " ".join(f"{key}={value}" for key, value in fields.items())
            + "\n",
            encoding="utf-8",
        )
        topology_root = self.root / "topology"
        self.assertEqual(
            topology.main(
                [
                    "--staging-manifest",
                    str(staging),
                    "--output-root",
                    str(topology_root),
                    "--run-name",
                    "synthetic",
                ]
            ),
            0,
        )
        output_root = self.root / "topology-analyses"
        self.assertEqual(
            objective.main(
                [
                    "--reference",
                    str(self.reference_path),
                    "--candidate",
                    f"synthetic={self.candidate_path}",
                    "--candidate-transfer",
                    "synthetic=linear",
                    "--region-spec",
                    str(self.region_path),
                    "--topology-manifest",
                    str(topology_root / "synthetic" / "topology.json"),
                    "--topology-image-space",
                    "active",
                    "--output-root",
                    str(output_root),
                    "--run-name",
                    "synthetic-topology-analysis",
                    "--normalization",
                    "exterior-rgb",
                    "--max-shift",
                    "0",
                ]
            ),
            0,
        )
        document = json.loads(
            (output_root / "synthetic-topology-analysis" / "analysis.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(document["analysis_version"], "20260825.2")
        self.assertEqual(document["topology"]["selected_sample_count"], 256)
        source = document["candidates"][0]["clipped_core_source"]
        self.assertEqual(source["mode"], "cfa-topology")
        self.assertEqual(source["selected_pixel_count"], 256)

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
