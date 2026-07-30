from __future__ import annotations

import unittest

import numpy as np

import tiling


def repeat_runner(tile: np.ndarray) -> np.ndarray:
    plane = tile[:, 0:1]
    sensor = np.repeat(np.repeat(plane, 2, axis=2), 2, axis=3)
    return np.ascontiguousarray(np.repeat(sensor, 3, axis=1))


class RawNindFullImageTilingContract(unittest.TestCase):
    def test_default_plan_preserves_pooling_phase_and_memory_budget(self) -> None:
        plan = tiling.plan_full_image(1731, 2601)
        self.assertEqual(plan.step_packed, 288)
        self.assertEqual(plan.step_packed % plan.pool_alignment_packed, 0)
        self.assertEqual(plan.blend_width_packed, 24)
        self.assertEqual((plan.grid_rows, plan.grid_columns), (7, 10))
        self.assertLessEqual(
            plan.estimated_working_bytes,
            plan.max_working_bytes,
        )

    def test_overlap_must_exceed_the_exact_halo(self) -> None:
        with self.assertRaisesRegex(ValueError, "must exceed"):
            tiling.plan_full_image(
                512,
                512,
                exact_halo_packed=100,
                blend_overlap_packed=100,
            )

    def test_step_must_preserve_pooling_phase(self) -> None:
        with self.assertRaisesRegex(ValueError, "pooling phase"):
            tiling.plan_full_image(
                512,
                512,
                blend_overlap_packed=110,
            )

    def test_memory_budget_is_an_admission_gate(self) -> None:
        with self.assertRaisesRegex(ValueError, "working-memory budget"):
            tiling.plan_full_image(
                1731,
                2601,
                max_working_bytes=1024,
            )

    def test_direct_reflection_matches_the_full_padding_reference(self) -> None:
        height, width = 289, 291
        packed = np.arange(
            4 * height * width,
            dtype=np.float32,
        ).reshape(4, height, width)
        plan = tiling.plan_image_geometry(height, width)
        overlap = plan.blend_overlap_packed
        padded = np.pad(
            packed,
            (
                (0, 0),
                (overlap, plan.pad_after_packed[0]),
                (overlap, plan.pad_after_packed[1]),
            ),
            mode="reflect",
        )
        for row in range(plan.grid_rows):
            for column in range(plan.grid_columns):
                placement = tiling.tile_placement(plan, row, column)
                expected = padded[
                    :,
                    placement.packed_y : (
                        placement.packed_y + plan.tile_edge_packed
                    ),
                    placement.packed_x : (
                        placement.packed_x + plan.tile_edge_packed
                    ),
                ][None]
                actual = tiling.extract_tile(packed, plan, placement)
                np.testing.assert_array_equal(actual, expected)

    def test_odd_image_is_covered_without_coordinate_or_blend_seams(self) -> None:
        height, width = 613, 719
        yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
        signal = (0.1 + yy * 0.0002 + xx * 0.0003).astype(np.float32)
        packed = np.stack(
            [signal, signal * 0.9, signal * 1.1, signal * 0.8],
            axis=0,
        )
        plan = tiling.plan_full_image(height, width)
        result = tiling.run_full_image(packed, repeat_runner, plan)
        expected = np.repeat(np.repeat(signal, 2, axis=0), 2, axis=1)
        expected = np.stack([expected, expected, expected], axis=0)
        expected *= np.float32(
            np.mean(packed, dtype=np.float64)
            / np.mean(expected, dtype=np.float64)
        )
        np.testing.assert_allclose(result.output, expected, atol=2e-7)
        self.assertLessEqual(
            result.receipt.coverage_max_abs_error,
            2e-7,
        )
        self.assertLess(
            result.receipt.trusted_overlap_max_abs,
            1e-7,
        )
        self.assertEqual(tiling.gate_failures(result.receipt), [])


if __name__ == "__main__":
    unittest.main()
