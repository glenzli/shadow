from __future__ import annotations

from dataclasses import replace
import math
import unittest
from unittest import mock

import benchmark
import dataset_manifest
import public_pair
import quality


def _metrics(mse: float) -> quality.ErrorMetrics:
    return quality.ErrorMetrics(
        mse=mse,
        rmse=math.sqrt(mse),
        mae=math.sqrt(mse),
        psnr_db=10.0 * math.log10(1.0 / mse),
        mean_bias=0.0,
    )


def _quality(scene_index: int, delta: float) -> quality.PairQualityReceipt:
    baseline = _metrics(0.01)
    denoised_mse = baseline.mse / (10.0 ** (delta / 10.0))
    denoised = _metrics(denoised_mse)
    raw = public_pair.VerifiedPublicRaw(
        role="noisy",
        path=f"/tmp/noisy-{scene_index}.dng",
        dataverse_file_id=scene_index + 1,
        persistent_id=f"doi:test/{scene_index}",
        filename=f"noisy-{scene_index}.dng",
        size_bytes=10,
        official_md5="0" * 32,
        sha256="1" * 64,
    )
    return quality.PairQualityReceipt(
        dataset_doi=public_pair.DATASET_DOI,
        dataset_license=public_pair.DATASET_LICENSE,
        noisy=raw,
        ground_truth=replace(raw, role="ground_truth"),
        provider="cpu",
        metric_space="normalized-linear-camera-rgb",
        ground_truth_method="canonical-bilinear-demosaic",
        baseline_method="canonical-bilinear-demosaic",
        gain_policy="independent-scalar-mean-match-to-ground-truth",
        saturation_threshold=0.98,
        alignment_shift_sensor=[0, 0],
        alignment_probe_mae=0.0,
        noisy_sensor_shape=[1024, 1024],
        ground_truth_sensor_shape=[1024, 1024],
        noisy_raw_pattern=[[0, 1], [1, 2]],
        ground_truth_raw_pattern=[[0, 1], [1, 2]],
        noisy_force_rggb_crop_sensor=[0, 0],
        ground_truth_force_rggb_crop_sensor=[0, 0],
        tile_origin_packed_noisy=[0, 0],
        ground_truth_origin_sensor=[0, 0],
        evaluation_halo_sensor=200,
        evaluation_shape_sensor=[624, 624],
        valid_fraction=1.0,
        baseline_gain_to_ground_truth=1.0,
        denoised_gain_to_ground_truth=1.0,
        baseline=baseline,
        denoised=denoised,
        psnr_delta_db=delta,
        mse_ratio=denoised_mse / baseline.mse,
        improved_on_probe=denoised_mse < baseline.mse,
    )


class RawNindBenchmarkContract(unittest.TestCase):
    def setUp(self) -> None:
        self.manifest = {
            "manifest_sha256": dataset_manifest.PILOT_MANIFEST_SHA256,
            "pairs": [
                {
                    "scene": selection.scene,
                    "camera_make": selection.camera_make,
                    "camera_model": selection.camera_model,
                    "noisy_iso": selection.noisy_iso,
                }
                for selection in dataset_manifest.PILOT_SELECTIONS
            ],
        }
        validation = mock.patch(
            "benchmark.dataset_manifest.validate_manifest"
        )
        validation.start()
        self.addCleanup(validation.stop)

    def _pairs(self, deltas: list[float]) -> list[benchmark.EvaluatedPair]:
        return [
            benchmark.EvaluatedPair(
                scene=pair["scene"],
                camera_make=pair["camera_make"],
                camera_model=pair["camera_model"],
                noisy_iso=pair["noisy_iso"],
                quality=_quality(index, deltas[index]),
            )
            for index, pair in enumerate(self.manifest["pairs"])
        ]

    def test_aggregate_passes_only_with_pair_and_median_margins(self) -> None:
        report = benchmark.aggregate(
            self.manifest,
            self._pairs([4.0] * 10),
        )
        self.assertEqual(report["status"], "passed")
        self.assertTrue(report["foundation_benchmark_admitted"])
        self.assertEqual(report["summary"]["worst_psnr_delta_db"], 4.0)

    def test_one_regression_cannot_hide_behind_a_strong_average(self) -> None:
        report = benchmark.aggregate(
            self.manifest,
            self._pairs([-0.1] + [12.0] * 9),
        )
        self.assertEqual(report["status"], "failed")
        self.assertFalse(report["foundation_benchmark_admitted"])
        self.assertTrue(
            any("7D-2" in failure for failure in report["gate_failures"])
        )

    def test_median_gate_is_independent_of_the_worst_pair_gate(self) -> None:
        report = benchmark.aggregate(
            self.manifest,
            self._pairs([1.0] * 10),
        )
        self.assertEqual(report["status"], "failed")
        self.assertTrue(
            any("median PSNR" in failure for failure in report["gate_failures"])
        )


if __name__ == "__main__":
    unittest.main()
