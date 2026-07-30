from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path

import numpy as np
import torch

import baseline


class IdentityPrediction:
    def __init__(self) -> None:
        self.calls = 0

    def predict(self, features: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
        self.calls += 1
        return {"denoised_mosaic": features["mosaic"].copy()}


class BaselineContractTests(unittest.TestCase):
    def test_synthetic_batch_is_deterministic_and_matches_noise_contract(self) -> None:
        first = torch.Generator(device="cpu")
        first.manual_seed(9)
        second = torch.Generator(device="cpu")
        second.manual_seed(9)
        left = baseline.generate_batch(2, 16, first)
        right = baseline.generate_batch(2, 16, second)
        for left_value, right_value in zip(left, right, strict=True):
            self.assertTrue(torch.equal(left_value, right_value))
        clean, noisy, noise = left
        self.assertEqual(tuple(clean.shape), (2, 4, 16, 16))
        self.assertEqual(tuple(noisy.shape), (2, 4, 16, 16))
        self.assertEqual(tuple(noise.shape), (2, 8))
        self.assertTrue(bool(torch.all(noise[:, :4] > 0.0)))
        self.assertTrue(bool(torch.all(noise[:, 4:] > 0.0)))

    def test_tiling_round_trips_odd_extents_and_covers_every_core_once(self) -> None:
        model = IdentityPrediction()
        mosaic = np.arange(4 * 71 * 67, dtype=np.float32).reshape(1, 4, 71, 67)
        noise = np.full((1, 8), 0.01, dtype=np.float32)
        output, timings = baseline.tiled_predict(model, mosaic, noise, 32, 5)
        self.assertTrue(np.array_equal(output, mosaic))
        self.assertEqual(model.calls, 16)
        self.assertEqual(len(timings), model.calls)

    def test_fixed_tile_export_wrapper_preserves_model_output(self) -> None:
        torch.manual_seed(4)
        model = baseline.SyntheticRawDenoiser(features=8).eval()
        wrapper = baseline.FixedTileExportModel(model, tile_edge=16).eval()
        mosaic = torch.rand((1, 4, 16, 16))
        noise = torch.rand((1, 8)) * 0.01
        with torch.no_grad():
            self.assertTrue(torch.equal(model(mosaic, noise), wrapper(mosaic, noise)))

    def test_learned_state_identity_ignores_registration_order(self) -> None:
        first = torch.nn.Sequential(
            torch.nn.Linear(2, 3),
            torch.nn.ReLU(),
            torch.nn.Linear(3, 1),
        )
        second = torch.nn.Sequential(
            torch.nn.Linear(2, 3),
            torch.nn.ReLU(),
            torch.nn.Linear(3, 1),
        )
        second.load_state_dict(first.state_dict())
        self.assertEqual(
            baseline.state_dict_identity(first),
            baseline.state_dict_identity(second),
        )
        with torch.no_grad():
            second[0].weight[0, 0] += 0.25
        self.assertNotEqual(
            baseline.state_dict_identity(first),
            baseline.state_dict_identity(second),
        )

    def test_quality_gate_rejects_a_model_that_ignores_noise_conditioning(self) -> None:
        quality = baseline.QualityMetrics(
            noisy_psnr_db=30.0,
            denoised_psnr_db=33.0,
            improvement_db=3.0,
            noisy_edge_psnr_db=29.0,
            denoised_edge_psnr_db=30.0,
            noisy_flat_psnr_db=30.0,
            denoised_flat_psnr_db=34.0,
            noise_conditioning_rmse=0.0,
            coreml_tiled_vs_torch_max_abs=0.0,
            coreml_tiled_vs_torch_rmse=0.0,
        )
        self.assertIn(
            "Core ML output is not measurably conditioned by the noise tensor",
            baseline.quality_gate_failures(quality),
        )

    def test_tree_identity_matches_the_native_length_prefixed_contract(self) -> None:
        with tempfile.TemporaryDirectory(suffix=".mlmodelc") as directory:
            root = Path(directory)
            (root / "z.bin").write_bytes(b"z")
            (root / "nested").mkdir()
            (root / "nested" / "a.bin").write_bytes(b"alpha")
            digest = hashlib.sha256()
            digest.update(b"shadow-coreml-model-tree-v1\0")
            for relative, contents in (
                ("nested/a.bin", b"alpha"),
                ("z.bin", b"z"),
            ):
                encoded = relative.encode("utf-8")
                digest.update(len(encoded).to_bytes(8, "big"))
                digest.update(encoded)
                digest.update(len(contents).to_bytes(8, "big"))
                digest.update(contents)
            self.assertEqual(
                baseline.compiled_model_tree_identity(root),
                f"sha256-tree-v1:{digest.hexdigest()}",
            )

    def test_payload_directory_inside_repository_is_rejected(self) -> None:
        repository_output = Path(__file__).resolve().parents[2] / "forbidden-model-output"
        with self.assertRaisesRegex(ValueError, "inside the repository"):
            baseline.validate_output_directory(repository_output)


if __name__ == "__main__":
    unittest.main()
