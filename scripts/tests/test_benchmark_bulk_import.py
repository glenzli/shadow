"""Comparisons must reject the workload mismatches seen in exploratory runs."""
import copy
import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("benchmark", Path(__file__).parents[1] / "benchmark_bulk_import.py")
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


def run_result():
    return {"schema": 2, "exit_code": 0, "forced_stop": False, "artifact_unchanged": True, "corpus_unchanged": True,
            "corpus": {"sha256": "corpus", "files": 1000}, "seed_sha256": "seed",
            "machine": ["Darwin", "same", "arm64"], "peak_gui_rss_bytes": 1_000_000,
            "binary_sha256": "same-app", "bundle_sha256": "same-bundle", "policy": "fifo",
            "report": {"success": True, "workload": "four-pages-four-targets-v1",
                       "pages_requested": 3, "loaded_rows": 384, "total_photos": 1000,
                       "complete_ms": 5000, "first_thumbnail_ready_ms": 100,
                       "event_loop_gap_p95_upper_ms": 32, "window_size": [1440, 900], "device_pixel_ratio": 2,
                       "steps": [{"source_row": i, "title": str(i), "source": str(i), "proxy_ready_ms": 500}
                                 for i in [288, 192, 96, 0]]}}


class ComparisonContracts(unittest.TestCase):
    def test_backfill_comparison_requires_identical_input_and_complete_analysis(self):
        before = run_result()
        before.update(scenario="analysis-backfill", policy="visible",
                      helper_identity={"path": "same-helper", "sha256": "decoder"},
                      backfill_before={"proxies": 1000, "observations": 0, "integrity": "ok",
                                       "proxy_records_sha256": "original"},
                      backfill_after={"proxies": 1000, "observations": 1000, "integrity": "ok"})
        after = copy.deepcopy(before)
        after.update(binary_sha256="new-gui", bundle_sha256="new-bundle")
        self.assertTrue(benchmark.compare(before, after)["comparable"])
        for field, value in [("helper_identity", {"path": "other", "sha256": "decoder"}),
                             ("scenario", "cold-proxies"), ("policy", "fifo")]:
            changed = copy.deepcopy(after)
            changed[field] = value
            with self.assertRaises(ValueError):
                benchmark.compare(before, changed)
        after["backfill_after"]["observations"] = 999
        with self.assertRaises(ValueError):
            benchmark.compare(before, after)

    def test_backfill_preparation_only_clears_analysis_in_new_profile(self):
        with tempfile.TemporaryDirectory() as root:
            seed, profile = Path(root) / "seed", Path(root) / "profile"
            (seed / "cache/blobs/b3").mkdir(parents=True)
            (seed / "cache/blobs/b3/proxy").write_bytes(b"unchanged cached pixels")
            profile.mkdir()
            with sqlite3.connect(seed / "catalog.sqlite") as db:
                db.executescript("CREATE TABLE representation_cached_artifacts (representation_id BLOB, variant_key TEXT, role TEXT);"
                                 "INSERT INTO representation_cached_artifacts VALUES (X'0102', 'variant', 'generated_proxy');"
                                 "CREATE TABLE representation_technical_observations (id INTEGER);"
                                 "INSERT INTO representation_technical_observations VALUES (1);")
            before = benchmark.catalog_visual_state(seed / "catalog.sqlite")
            copied = benchmark.prepare_backfill_profile(seed, profile)
            self.assertEqual(copied["observations"], 0)
            self.assertEqual(copied["proxy_records_sha256"], before["proxy_records_sha256"])
            self.assertEqual(benchmark.catalog_visual_state(seed / "catalog.sqlite"), before)
            self.assertEqual((profile / "cache/blobs/b3/proxy").read_bytes(), b"unchanged cached pixels")

    def test_comparable_runs_report_observed_change(self):
        before = run_result()
        after = copy.deepcopy(before)
        after["policy"] = "visible"
        after["report"]["steps"][0]["proxy_ready_ms"] = 100
        result = benchmark.compare(before, after)
        self.assertTrue(result["comparable"])
        self.assertEqual(result["metrics"]["step_0_proxy_ready_ms"]["change_percent"], -80)

    def test_changed_work_or_failed_run_cannot_be_a_speedup(self):
        for field, value in [("loaded_rows", 416), ("pages_requested", 4), ("window_size", [1200, 800]),
                             ("device_pixel_ratio", 1)]:
            with self.subTest(field=field):
                before, after = run_result(), run_result()
                after["policy"] = "visible"
                after["report"][field] = value
                with self.assertRaises(ValueError):
                    benchmark.compare(before, after)
        for field, value in [("schema", 1), ("forced_stop", True), ("artifact_unchanged", False),
                             ("corpus_unchanged", False), ("bundle_sha256", "other"),
                             ("seed_sha256", "other")]:
            with self.subTest(field=field):
                before, after = run_result(), run_result()
                after["policy"] = "visible"
                after[field] = value
                with self.assertRaises(ValueError):
                    benchmark.compare(before, after)
        before, after = run_result(), run_result()
        after["policy"] = "visible"
        after["report"]["steps"][0]["title"] = "different-photo"
        with self.assertRaises(ValueError):
            benchmark.compare(before, after)


if __name__ == "__main__":
    unittest.main()
