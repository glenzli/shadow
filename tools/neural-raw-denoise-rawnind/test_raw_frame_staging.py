from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

import numpy as np

import raw_frame_staging


class RawFrameStagingContract(unittest.TestCase):
    def test_strict_manifest_loads_little_endian_active_plane(self) -> None:
        with tempfile.TemporaryDirectory(dir="/private/tmp") as directory_name:
            manifest = Path(directory_name) / "frame.shadowrawi"
            samples = np.arange(16, dtype="<u2").reshape(4, 4)
            sample_path = Path(f"{manifest}.u16le")
            sample_path.write_bytes(samples.tobytes())
            manifest.write_text(
                "shadow-raw-frame-staging-20260806.1 "
                "descriptor_contract=active-camera-colour-20260806.1 "
                "width=4 height=4 cfa=GRBG "
                "black=64,65,66,67 "
                "white=16383,16383,16383,16383 "
                "orientation=0 bits_per_sample=14 "
                "as_shot_neutral=2,1,1.5,1 "
                "camera_to_xyz_d50=- "
                "xyz_to_camera_d65=1,0,0,0,1,0,0,0,1 "
                "camera_to_linear_srgb_d65=1,0,0,0,1,0,0,0,1 "
                "pending_dng_opcode_bytes=0,0,0 "
                "provider_id_hex=736861646f772e74657374 "
                "provider_version_hex=312e30 "
                "sample_bytes=32\n",
                encoding="utf-8",
            )
            frame = raw_frame_staging.load(manifest)
            np.testing.assert_array_equal(frame.mosaic, samples)
            self.assertEqual(frame.cfa, "GRBG")
            self.assertEqual(frame.provider_id, "shadow.test")
            self.assertEqual(frame.provider_version, "1.0")
            self.assertEqual(len(frame.samples_sha256), 64)

    def test_incomplete_payload_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(dir="/private/tmp") as directory_name:
            manifest = Path(directory_name) / "frame.shadowrawi"
            Path(f"{manifest}.u16le").write_bytes(b"\0" * 30)
            manifest.write_text(
                "shadow-raw-frame-staging-20260806.1 "
                "descriptor_contract=active-camera-colour-20260806.1 "
                "width=4 height=4 cfa=RGGB "
                "black=0,0,0,0 white=1,1,1,1 "
                "orientation=0 bits_per_sample=14 "
                "as_shot_neutral=2,1,1.5,1 "
                "camera_to_xyz_d50=- "
                "xyz_to_camera_d65=1,0,0,0,1,0,0,0,1 "
                "camera_to_linear_srgb_d65=1,0,0,0,1,0,0,0,1 "
                "pending_dng_opcode_bytes=0,0,0 "
                "provider_id_hex=- provider_version_hex=- "
                "sample_bytes=32\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "incomplete"):
                raw_frame_staging.load(manifest)


if __name__ == "__main__":
    unittest.main()
