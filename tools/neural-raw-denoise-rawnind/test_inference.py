from __future__ import annotations

import unittest

import numpy as np

import inference


class RawNindInferenceContract(unittest.TestCase):
    def test_gbrg_is_physically_cropped_to_rggb_geometry(self) -> None:
        pattern = np.array([[3, 2], [0, 1]], dtype=np.uint8)
        row, column, forced = inference.force_rggb_geometry(pattern, "RGBG")
        self.assertEqual((row, column), (1, 0))
        np.testing.assert_array_equal(
            forced,
            np.array([[0, 1], [3, 2]], dtype=np.uint8),
        )

    def test_representative_input_matches_static_graph_contract(self) -> None:
        value = inference.representative_packed_bayer()
        self.assertEqual(value.shape, (1, 4, 512, 512))
        self.assertEqual(value.dtype, np.float32)
        self.assertGreaterEqual(float(value.min()), 0.0)
        self.assertLessEqual(float(value.max()), 1.0)

    def test_match_gain_restores_input_mean(self) -> None:
        anchor = np.full((1, 4, 16, 16), 0.25, dtype=np.float32)
        output = np.full((1, 3, 32, 32), -8.0, dtype=np.float32)
        matched = inference.match_gain(output, anchor)
        self.assertAlmostEqual(float(matched.mean()), 0.25, places=6)

    def test_match_gain_rejects_degenerate_output(self) -> None:
        anchor = np.full((1, 4, 16, 16), 0.25, dtype=np.float32)
        output = np.zeros((1, 3, 32, 32), dtype=np.float32)
        with self.assertRaisesRegex(ValueError, "cannot be gain matched"):
            inference.match_gain(output, anchor)

    def test_architecture_requires_more_context_than_published_overlap(self) -> None:
        self.assertEqual(inference.MODEL_RECEPTIVE_FIELD_PACKED, 200)
        self.assertEqual(inference.CONSERVATIVE_EXACT_HALO_PACKED, 100)
        self.assertLess(
            inference.DARKTABLE_OVERLAP_PACKED,
            inference.CONSERVATIVE_EXACT_HALO_PACKED,
        )
        self.assertLess(
            inference.DEMO_OVERLAP_PACKED,
            inference.CONSERVATIVE_EXACT_HALO_PACKED,
        )


if __name__ == "__main__":
    unittest.main()
