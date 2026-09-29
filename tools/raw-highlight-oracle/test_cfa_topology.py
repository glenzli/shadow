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


topology = load_owner("cfa_topology")


class CfaTopologyContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="shadow-cfa-topology-test-")
        self.root = pathlib.Path(self.temporary.name)
        self.staging = self.root / "source.shadowrawi"
        self.payload = pathlib.Path(f"{self.staging}.u16le")
        samples = np.full((6, 6), 20, dtype="<u2")
        samples[2:5, 2:5] = 100
        samples[0, 0] = 100
        samples[0, 1] = 95
        samples.tofile(self.payload)
        fields = {
            "descriptor_contract": "active-camera-colour-response-20260822.1",
            "width": "6",
            "height": "6",
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
        line = "shadow-raw-frame-staging-20260822.1 " + " ".join(
            f"{key}={value}" for key, value in fields.items()
        )
        self.staging.write_text(line + "\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _generate(self, run_name: str = "synthetic") -> pathlib.Path:
        status = topology.main(
            [
                "--staging-manifest",
                str(self.staging),
                "--output-root",
                str(self.root / "topologies"),
                "--run-name",
                run_name,
                "--rows-per-chunk",
                "2",
            ]
        )
        self.assertEqual(status, 0)
        return self.root / "topologies" / run_name / "topology.json"

    def test_generation_marks_exact_sites_and_shared_three_by_three_core(self) -> None:
        manifest = self._generate()
        document = json.loads(manifest.read_text(encoding="utf-8"))
        self.assertEqual(document["schema"], topology.TOPOLOGY_SCHEMA)
        self.assertEqual(document["statistics"]["physical_white_sample_count"], 10)
        self.assertEqual(
            document["statistics"]["linear_response_terminal_sample_count"], 11
        )
        self.assertEqual(
            document["statistics"]["shared_physical_white_sample_count"], 1
        )
        physical, receipt = topology.read_topology_mask(
            manifest,
            "physical-white",
            "active",
            (6, 6),
            (0, 0, 6, 6),
        )
        shared, _ = topology.read_topology_mask(
            manifest,
            "shared-physical-white",
            "active",
            (6, 6),
            (0, 0, 6, 6),
        )
        self.assertEqual(int(np.count_nonzero(physical)), 10)
        self.assertEqual(int(np.count_nonzero(shared)), 1)
        self.assertTrue(bool(shared[3, 3]))
        self.assertEqual(receipt["selected_sample_count"], 10)
        serialized = json.dumps(document)
        self.assertNotIn(str(self.staging), serialized)
        self.assertNotIn(str(self.payload), serialized)

    def test_display_orientation_uses_rawframe_convention(self) -> None:
        values = np.arange(12).reshape(3, 4)
        np.testing.assert_array_equal(
            topology._oriented_mask(values, 5, "display"), np.rot90(values, 1)
        )
        np.testing.assert_array_equal(
            topology._oriented_mask(values, 6, "display"), np.rot90(values, -1)
        )

    def test_changed_payload_identity_is_rejected(self) -> None:
        manifest = self._generate("tamper")
        payload = manifest.parent / "topology.u8"
        contents = bytearray(payload.read_bytes())
        contents[0] ^= 1
        payload.write_bytes(contents)
        with self.assertRaisesRegex(ValueError, "identity changed"):
            topology.read_topology_mask(
                manifest,
                "physical-white",
                "active",
                (6, 6),
                (0, 0, 6, 6),
            )

    def test_run_directory_is_immutable(self) -> None:
        self._generate("once")
        status = topology.main(
            [
                "--staging-manifest",
                str(self.staging),
                "--output-root",
                str(self.root / "topologies"),
                "--run-name",
                "once",
            ]
        )
        self.assertEqual(status, 2)


if __name__ == "__main__":
    unittest.main()
