#!/usr/bin/env python3

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

import numpy as np

import pmrid


class PmridAdapterContract(unittest.TestCase):
    def test_iso_polynomial_and_shadow_noise_transform_are_equivalent(self) -> None:
        for iso in (100.0, 200.0, 800.0, 1600.0, 3200.0, 6400.0):
            read, shot = pmrid.pmrid_normalized_noise(iso)
            scale, offset = pmrid.noise_transform_parameters(
                np.full(4, read),
                np.full(4, shot),
            )
            expected_scale, expected_offset = pmrid.pmrid_iso_transform_parameters(iso)
            np.testing.assert_allclose(scale, expected_scale, rtol=0.0, atol=1.0e-13)
            np.testing.assert_allclose(offset, expected_offset, rtol=0.0, atol=1.0e-13)

    def test_invalid_noise_calibration_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "physically valid"):
            pmrid.noise_transform_parameters(
                np.zeros(4),
                np.zeros(4),
            )
        with self.assertRaisesRegex(ValueError, "physically valid"):
            pmrid.noise_transform_parameters(
                np.full(4, np.nan),
                np.ones(4),
            )

    def test_output_payload_must_stay_outside_the_repository(self) -> None:
        repository_path = Path(__file__).resolve().parents[2] / "forbidden-output"
        with self.assertRaisesRegex(ValueError, "inside the repository"):
            pmrid.validate_output_directory(repository_path)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "new"
            self.assertEqual(pmrid.validate_output_directory(output), output.resolve())

    def test_pinned_receptive_field_requires_a_large_exact_halo(self) -> None:
        self.assertEqual(pmrid.PMRID_RECEPTIVE_FIELD, 965)
        self.assertEqual(pmrid.PMRID_EXACT_HALO, 482)
        self.assertEqual(pmrid.PMRID_EXACT_HALO * 2 + 1, 965)

    @unittest.skipUnless(
        os.environ.get("SHADOW_TEST_PMRID_UPSTREAM"),
        "set SHADOW_TEST_PMRID_UPSTREAM to the pinned public checkout",
    )
    def test_external_checkpoint_loads_with_the_verified_contract(self) -> None:
        upstream = Path(os.environ["SHADOW_TEST_PMRID_UPSTREAM"])
        receipt = pmrid.verify_upstream(upstream)
        self.assertEqual(receipt.revision, pmrid.UPSTREAM_REVISION)
        network = pmrid.load_verified_network(upstream)
        self.assertEqual(
            sum(parameter.numel() for parameter in network.parameters()),
            1_031_908,
        )


if __name__ == "__main__":
    unittest.main()
