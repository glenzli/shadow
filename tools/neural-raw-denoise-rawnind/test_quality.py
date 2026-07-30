from __future__ import annotations

from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np

import public_pair
import quality


class RawNindQualityContract(unittest.TestCase):
    def test_canonical_demosaic_preserves_known_bayer_sites(self) -> None:
        packed = np.stack(
            [
                np.full((4, 5), 0.1, dtype=np.float32),
                np.full((4, 5), 0.2, dtype=np.float32),
                np.full((4, 5), 0.3, dtype=np.float32),
                np.full((4, 5), 0.4, dtype=np.float32),
            ]
        )
        rgb = quality.canonical_bilinear_demosaic(packed)
        self.assertEqual(rgb.shape, (3, 8, 10))
        np.testing.assert_array_equal(rgb[0, 0::2, 0::2], packed[0])
        np.testing.assert_array_equal(rgb[1, 0::2, 1::2], packed[1])
        np.testing.assert_array_equal(rgb[1, 1::2, 0::2], packed[2])
        np.testing.assert_array_equal(rgb[2, 1::2, 1::2], packed[3])

    def test_alignment_reports_reference_offset_for_target_coordinates(self) -> None:
        yy, xx = np.mgrid[0:256, 0:256].astype(np.float32)
        reference_luma = (
            np.sin(xx * 0.073)
            + np.cos(yy * 0.051)
            + 0.3 * np.sin((xx + yy) * 0.11)
        )
        reference = np.stack(
            [reference_luma, reference_luma * 0.8, reference_luma * 1.2]
        )
        expected_shift = (7, -5)
        target = np.zeros_like(reference)
        reference_view, target_view = quality.aligned_views(
            reference,
            target,
            expected_shift,
        )
        target_view[...] = reference_view
        actual_shift, _ = quality.estimate_alignment(reference, target)
        self.assertEqual(actual_shift, expected_shift)

    def test_error_metrics_improve_for_closer_candidate(self) -> None:
        reference = np.full((3, 8, 8), 0.5, dtype=np.float32)
        mask = np.ones((8, 8), dtype=bool)
        farther = quality.error_metrics(
            reference,
            np.full_like(reference, 0.7),
            mask,
        )
        closer = quality.error_metrics(
            reference,
            np.full_like(reference, 0.55),
            mask,
        )
        self.assertLess(closer.mse, farther.mse)
        self.assertGreater(closer.psnr_db, farther.psnr_db)

    def test_preview_is_a_single_horizontal_ppm(self) -> None:
        image = np.full((3, 4, 5), 0.25, dtype=np.float32)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "probe.ppm"
            quality.write_comparison_ppm(image, image, image, path)
            with path.open("rb") as stream:
                self.assertEqual(stream.readline(), b"P6\n")
                self.assertEqual(stream.readline(), b"15 4\n")
                self.assertEqual(stream.readline(), b"255\n")
                self.assertEqual(len(stream.read()), 15 * 4 * 3)

    def test_public_pair_evaluation_accepts_manifest_identities(self) -> None:
        expected_noisy = public_pair.PublicRawFile(
            role="noisy",
            dataverse_file_id=1,
            persistent_id="doi:test/noisy",
            filename="noisy.dng",
            size_bytes=10,
            md5="0" * 32,
        )
        expected_ground_truth = public_pair.PublicRawFile(
            role="ground_truth",
            dataverse_file_id=2,
            persistent_id="doi:test/ground-truth",
            filename="ground-truth.dng",
            size_bytes=10,
            md5="1" * 32,
        )
        with mock.patch(
            "quality.public_pair.verify_public_raw",
            side_effect=[
                mock.sentinel.verified_noisy,
                RuntimeError("verified expected identities"),
            ],
        ) as verify:
            with self.assertRaisesRegex(
                RuntimeError,
                "verified expected identities",
            ):
                quality.evaluate_public_pair(
                    Path("model.onnx"),
                    Path("noisy.dng"),
                    Path("ground-truth.dng"),
                    "cpu",
                    expected_noisy=expected_noisy,
                    expected_ground_truth=expected_ground_truth,
                )
        self.assertEqual(
            verify.call_args_list,
            [
                mock.call(Path("noisy.dng"), expected_noisy),
                mock.call(
                    Path("ground-truth.dng"),
                    expected_ground_truth,
                ),
            ],
        )


if __name__ == "__main__":
    unittest.main()
