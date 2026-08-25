from __future__ import annotations

import json
import pathlib
import tempfile
import unittest

import strict_alignment


class StrictAlignmentTests(unittest.TestCase):
    def _manifest(self, root: pathlib.Path) -> pathlib.Path:
        evidence: dict[str, str] = {
            "highlight_reference.clipped_photosites": "60",
            "highlight_reference.changed_photosites": "30",
            "highlight_reference.dimensions": "64x32",
            "highlight_reference.mean_absolute_difference": "0.001",
            "highlight_reference.maximum_absolute_difference": "0.125",
            "highlight_cfa_diagnostic.domain.strongest_boundary_difference": "0.250",
            "highlight_cfa_diagnostic.threshold": "0.987",
            "highlight_cfa_diagnostic.threshold_domain": "linear-response-limit",
            "highlight_reference.threshold": "0.987",
            "highlight_reference.threshold_domain": "physical-white",
            "highlight_reference.cfa.white_levels": "1000,1000,1000,1000",
            "highlight_reference.cfa.has_linear_response_limits": "yes",
            "highlight_reference.cfa.linear_response_limits": "950,950,950,950",
        }
        counts = {
            "R": (10, 5, 10, 5, 1.5, 1.4805, 0.1, 0.11),
            "G": (20, 10, 21, 10, 1.0, 0.987, 0.0, 0.0),
            "B": (30, 15, 30, 16, 1.2, 1.1844, -0.1, -0.09),
        }
        for channel, values in counts.items():
            clipped, changed, candidates, raised, gain, clip, reference_offset, shadow_offset = values
            evidence[f"highlight_reference.channel.{channel}.effective_white_balance_gain"] = str(gain)
            evidence[f"highlight_reference.channel.{channel}.clip"] = str(clip)
            evidence[f"highlight_reference.channel.{channel}.clipped_photosites"] = str(clipped)
            evidence[f"highlight_reference.channel.{channel}.changed_photosites"] = str(changed)
            evidence[f"highlight_reference.channel.{channel}.chrominance_offset"] = str(reference_offset)
            evidence[f"highlight_cfa_diagnostic.channel.{channel}.candidates"] = str(candidates)
            evidence[f"highlight_cfa_diagnostic.channel.{channel}.raised"] = str(raised)
            evidence[f"highlight_cfa_diagnostic.channel.{channel}.compiled_chrominance_offset"] = str(shadow_offset)
        manifest = {
            "schema": strict_alignment.ORACLE_MANIFEST_SCHEMA,
            "orchestrator_version": "test",
            "source": {"basename": "fixture.raw", "size_bytes": 16, "sha256": "a" * 64},
            "adapters": [
                {
                    "id": strict_alignment.STRICT_ADAPTER_ID,
                    "status": "succeeded",
                    "comparison_class": strict_alignment.STRICT_COMPARISON_CLASS,
                    "elapsed_ms": 12.5,
                    "evidence": evidence,
                    "executable": {"name": "shadow-raw-probe", "sha256": "b" * 64},
                }
            ],
        }
        path = root / "manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        return path

    def test_builds_channel_and_aggregate_alignment_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            manifest = self._manifest(root)
            report = strict_alignment.build_report([("fixture", manifest)])
            schema = json.loads(
                (pathlib.Path(__file__).with_name("strict-alignment.schema.json")).read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(schema["properties"]["schema"]["const"], report["schema"])
            self.assertEqual(report["schema"], strict_alignment.REPORT_SCHEMA)
            fixture = report["fixtures"][0]
            self.assertEqual(fixture["totals"]["reference_clipped_photosites"], 60)
            self.assertEqual(fixture["totals"]["shadow_terminal_candidates"], 61)
            self.assertEqual(fixture["totals"]["candidate_count_alignment"], "close-count")
            blue = fixture["channels"][2]
            self.assertEqual(blue["write_count_delta"], 1)
            self.assertFalse(
                fixture["interpretation_boundary"]["count_comparison_is_spatial_overlap"]
            )
            self.assertEqual(fixture["thresholds"]["shadow"]["domain"], "linear-response-limit")
            self.assertEqual(fixture["thresholds"]["reference"]["domain"], "physical-white")

    def test_writes_one_immutable_run(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            manifest = self._manifest(root)
            output = strict_alignment.write_report(
                [("fixture", manifest)], root / "runs", "strict-a"
            )
            self.assertTrue(output.is_file())
            with self.assertRaisesRegex(ValueError, "already exists"):
                strict_alignment.write_report(
                    [("fixture", manifest)], root / "runs", "strict-a"
                )

    def test_rejects_inconsistent_reference_channel_totals(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            manifest = self._manifest(root)
            payload = json.loads(manifest.read_text(encoding="utf-8"))
            payload["adapters"][0]["evidence"][
                "highlight_reference.channel.B.changed_photosites"
            ] = "14"
            manifest.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "do not sum"):
                strict_alignment.build_report([("fixture", manifest)])

    def test_reference_zero_write_count_is_not_reported_as_a_ratio(self) -> None:
        relative = strict_alignment._relative_count_delta(3, 0)
        self.assertIsNone(relative)
        self.assertEqual(
            strict_alignment._count_alignment(3, 0, relative),
            "reference-zero-shadow-positive",
        )


if __name__ == "__main__":
    unittest.main()
