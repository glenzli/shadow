from __future__ import annotations

import unittest

import numpy as np

import stripe_lifecycle
import tiling


def repeat_runner(tile: np.ndarray) -> np.ndarray:
    plane = tile[:, 0:1]
    sensor = np.repeat(np.repeat(plane, 2, axis=2), 2, axis=3)
    return np.ascontiguousarray(np.repeat(sensor, 3, axis=1))


def synthetic_packed(height: int, width: int) -> np.ndarray:
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    signal = (0.1 + yy * 0.0002 + xx * 0.0003).astype(np.float32)
    return np.stack(
        [signal, signal * 0.9, signal * 1.1, signal * 0.8],
        axis=0,
    )


def publication() -> stripe_lifecycle.StripePublication:
    return stripe_lifecycle.StripePublication(
        producer="rawnind-bayer-two-pass-stripe-v1",
        global_input_mean=0.25,
        first_pass_raw_output_mean=2.0,
        second_pass_raw_output_mean=2.0,
        replay_relative_mean_delta=0.0,
        global_gain=0.125,
        output_mean=0.25,
    )


class RawNindStripeLifecycleContract(unittest.TestCase):
    def test_canon_plan_bounds_the_accumulator_without_full_padding(self) -> None:
        plan = stripe_lifecycle.plan_striped_image(1731, 2601)
        self.assertEqual(plan.max_buffer_rows_sensor, 824)
        self.assertEqual(plan.max_emitted_stripe_rows_sensor, 576)
        self.assertEqual(plan.inference_passes, 2)
        self.assertEqual(
            plan.source_extraction,
            "direct-reflect-from-borrowed-packed-input",
        )
        self.assertLess(
            plan.estimated_additional_working_bytes,
            plan.tiling.estimated_working_bytes,
        )
        self.assertLessEqual(
            plan.estimated_additional_working_bytes,
            plan.max_additional_working_bytes,
        )

    def test_additional_memory_budget_is_an_admission_gate(self) -> None:
        with self.assertRaisesRegex(ValueError, "additional working-memory"):
            stripe_lifecycle.plan_striped_image(
                1731,
                2601,
                max_additional_working_bytes=1024,
            )

    def test_stripes_match_the_full_frame_reference(self) -> None:
        height, width = 289, 291
        packed = synthetic_packed(height, width)
        full_plan = tiling.plan_full_image(height, width)
        reference = tiling.run_full_image(
            packed,
            repeat_runner,
            full_plan,
        )
        stripe_plan = stripe_lifecycle.plan_striped_image(height, width)
        sink = stripe_lifecycle.ArrayStripeSink()
        striped = stripe_lifecycle.run_striped_image(
            packed,
            repeat_runner,
            stripe_plan,
            sink,
        )
        self.assertIsNotNone(sink.output)
        np.testing.assert_array_equal(sink.output, reference.output)
        self.assertAlmostEqual(
            striped.receipt.global_gain,
            reference.receipt.global_gain,
            places=7,
        )
        self.assertEqual(sink.state, "committed")
        self.assertEqual(
            striped.receipt.total_tile_inferences,
            2 * stripe_plan.tiling.tile_count,
        )
        self.assertEqual(
            stripe_lifecycle.gate_failures(striped.receipt),
            [],
        )

    def test_cancellation_aborts_the_unpublished_sink(self) -> None:
        packed = synthetic_packed(289, 291)
        plan = stripe_lifecycle.plan_striped_image(289, 291)
        writes = 0

        class TrackingSink(stripe_lifecycle.DigestStripeSink):
            def write_stripe(
                self,
                y_start: int,
                stripe: np.ndarray,
            ) -> None:
                nonlocal writes
                super().write_stripe(y_start, stripe)
                writes += 1

        sink = TrackingSink()
        checks = 0

        def cancelled() -> bool:
            nonlocal checks
            checks += 1
            return checks >= 10

        with self.assertRaises(stripe_lifecycle.StripeCancelled):
            stripe_lifecycle.run_striped_image(
                packed,
                repeat_runner,
                plan,
                sink,
                cancelled=cancelled,
            )
        self.assertEqual(writes, 1)
        self.assertEqual(sink.state, "aborted")

    def test_sink_rejects_incomplete_publication(self) -> None:
        sink = stripe_lifecycle.DigestStripeSink()
        sink.begin((3, 8, 12))
        sink.write_stripe(
            0,
            np.zeros((3, 4, 12), dtype=np.float32),
        )
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            sink.commit(publication())
        sink.abort()
        self.assertEqual(sink.state, "aborted")


if __name__ == "__main__":
    unittest.main()
