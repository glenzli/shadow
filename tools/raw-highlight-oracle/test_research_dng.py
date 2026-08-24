from __future__ import annotations

import importlib.util
import dataclasses
import pathlib
import struct
import sys
import tempfile
import unittest


OWNER_ROOT = pathlib.Path(__file__).resolve().parent


def load_owner(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


normalized = load_owner("shadow_normalized_mosaic", OWNER_ROOT / "normalized_mosaic.py")
sys.modules["normalized_mosaic"] = normalized
research = load_owner("shadow_research_dng", OWNER_ROOT / "research_dng.py")


class ResearchDngContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="shadow-research-dng-test-")
        self.root = pathlib.Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def staging(self, *, white: tuple[int, int, int, int] = (1023, 1023, 1023, 1023)):
        manifest = self.root / "fixture.shadowrawi"
        samples = tuple(range(16))
        payload = b"".join(struct.pack("<H", value) for value in samples)
        pathlib.Path(f"{manifest}.u16le").write_bytes(payload)
        identity = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
        matrix = ",".join(str(value) for value in identity)
        manifest.write_text(
            "shadow-raw-frame-staging-20260822.1 "
            "descriptor_contract=active-camera-colour-response-20260822.1 "
            "width=4 height=4 cfa=RGGB black=4,5,6,7 "
            f"white={','.join(str(value) for value in white)} "
            "linear_response=900,901,902,903 has_linear_response=1 orientation=0 "
            "bits_per_sample=10 as_shot_neutral=2,1,1,1.5 "
            f"camera_to_xyz_d50={matrix} xyz_to_camera_d65={matrix} "
            f"camera_to_linear_srgb_d65={matrix} "
            "pending_dng_opcode_bytes=0,0,0 "
            "provider_id_hex=736861646f772e74657374 "
            "provider_version_hex=312e30 sample_bytes=32\n",
            encoding="utf-8",
        )
        return normalized.read_staging(manifest)

    def test_dng_round_trips_exact_samples_and_private_descriptor(self) -> None:
        mosaic = self.staging()
        output = self.root / "fixture.dng"
        receipt = research.write_research_dng(mosaic, output)
        verification = research.verify_research_dng(mosaic, output)

        self.assertEqual(receipt.standard_white_projection, "exact")
        self.assertEqual(receipt.standard_neutral_projection, "exact")
        self.assertEqual(verification["sample_roundtrip"], "byte-identical")
        self.assertEqual(verification["metadata_roundtrip"], "exact-private-payload")
        parsed = research.parse_tiff(output)
        self.assertEqual(parsed.unsigned_values(256), (4,))
        self.assertEqual(parsed.unsigned_values(257), (4,))
        self.assertEqual(parsed.unsigned_values(50717), (1023,))
        self.assertEqual(research.extract_u16le_strip(parsed), mosaic.sample_path.read_bytes())

    def test_unequal_site_whites_are_preserved_privately_and_projected_conservatively(self) -> None:
        mosaic = self.staging(white=(1000, 1010, 1020, 1030))
        output = self.root / "unequal-white.dng"
        receipt = research.write_research_dng(mosaic, output)

        self.assertEqual(receipt.standard_white_level, 1000)
        self.assertEqual(receipt.standard_white_projection, "conservative-minimum")
        self.assertEqual(research.parse_tiff(output).unsigned_values(50717), (1000,))
        self.assertEqual(
            research._private_document(research.parse_tiff(output))["normalized_mosaic"]["white"],
            [1000, 1010, 1020, 1030],
        )

    def test_sample_corruption_fails_round_trip(self) -> None:
        mosaic = self.staging()
        output = self.root / "corrupted.dng"
        research.write_research_dng(mosaic, output)
        encoded = bytearray(output.read_bytes())
        encoded[-1] ^= 0x01
        output.write_bytes(encoded)

        with self.assertRaisesRegex(ValueError, "not byte-identical"):
            research.verify_research_dng(mosaic, output)

    def test_linear_srgb_camera_matrix_prevents_identity_calibration_fallback(self) -> None:
        mosaic = dataclasses.replace(
            self.staging(),
            xyz_to_camera_d65=None,
            camera_to_xyz_d50=None,
            camera_to_linear_srgb_d65=(
                1.2,
                -0.1,
                -0.1,
                -0.2,
                1.1,
                0.1,
                0.0,
                -0.1,
                1.1,
            ),
        )
        matrix, illuminant, projection = research._camera_matrix(mosaic)

        self.assertEqual(illuminant, 21)
        self.assertEqual(projection, "derived-from-camera-to-linear-srgb-d65")
        self.assertNotEqual(matrix, (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0))

    def test_changed_staging_field_set_fails_closed(self) -> None:
        mosaic = self.staging()
        text = mosaic.manifest_path.read_text(encoding="utf-8")
        mosaic.manifest_path.write_text(text.rstrip("\n") + " extra=1\n", encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "field set changed"):
            normalized.read_staging(mosaic.manifest_path)


if __name__ == "__main__":
    unittest.main()
