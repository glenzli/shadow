from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


OWNER_ROOT = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "shadow_raw_highlight_fixture_matrix", OWNER_ROOT / "fixture_matrix.py"
)
assert SPEC is not None and SPEC.loader is not None
fixture = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = fixture
SPEC.loader.exec_module(fixture)


def region(case_id: str) -> dict[str, object]:
    return {
        "schema": fixture.REGION_SCHEMA,
        "id": case_id,
        "crop_normalized": [0.0, 0.0, 1.0, 1.0],
        "clipped_core_rectangles": [[0.4, 0.4, 0.2, 0.2]],
        "reliable_exterior_rectangles": [[0.0, 0.0, 0.25, 0.25]],
        "boundary_radius_pixels": 4,
    }


class FixtureMatrixContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="shadow-fixture-test-")
        self.root = pathlib.Path(self.temporary.name)
        self.sources = []
        for name in ("nikon.nef", "lamp.nef", "sony.arw"):
            path = self.root / name
            path.write_bytes((name + "-bounded-fixture").encode("ascii"))
            self.sources.append(path)
        classes = sorted(fixture.REQUIRED_CLASSES)
        fixtures = []
        for index, source in enumerate(self.sources):
            cases = []
            for case_class in classes[index * 2 : index * 2 + 2]:
                case_id = case_class + "-case"
                cases.append({"id": case_id, "class": case_class, "region": region(case_id)})
            fixtures.append(
                {
                    "id": source.stem,
                    "source_path": str(source),
                    "license": "local-user-owned-research-fixture",
                    "cases": cases,
                }
            )
        self.candidate = {
            "id": "candidate-a",
            "write_ownership": "offline output",
            "context_radius": {"unit": "pixels", "value": 16},
            "reusable_upstream_state": "decoded CFA",
            "recomputation_frontier": "RAW source preparation",
            "residency_and_transfers": "CPU only",
            "cache_identity_impact": "candidate version",
            "cancellation_behavior": "bounded process",
            "preview_detail_export_equivalence": "same source result",
        }
        self.definition_path = self.root / "definition.json"
        self.definition_path.write_text(
            json.dumps(
                {
                    "schema": fixture.DEFINITION_SCHEMA,
                    "fixtures": fixtures,
                    "candidates": [self.candidate],
                }
            ),
            encoding="utf-8",
        )
        self.catalog_path = self.root / "fixture-matrix.json"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def build_catalog(self) -> None:
        self.assertEqual(
            fixture.main(
                [
                    "build",
                    "--definition",
                    str(self.definition_path),
                    "--output",
                    str(self.catalog_path),
                    "--matrix-version",
                    "test-v1",
                ]
            ),
            0,
        )

    def test_build_and_audit_preserve_external_no_copy_sources(self) -> None:
        self.build_catalog()
        document = json.loads(self.catalog_path.read_text(encoding="utf-8"))
        self.assertEqual(document["schema"], fixture.MATRIX_SCHEMA)
        self.assertEqual(len(document["fixtures"]), 3)
        self.assertTrue(
            all(
                source["source"]["copied_into_catalog"] is False
                for source in document["fixtures"]
            )
        )
        self.assertEqual(
            set(document["required_classes"]), fixture.REQUIRED_CLASSES
        )
        self.assertEqual(
            fixture.main(["audit", "--catalog", str(self.catalog_path)]), 0
        )
        regions = self.root / "regions"
        self.assertEqual(
            fixture.main(
                [
                    "export-regions",
                    "--catalog",
                    str(self.catalog_path),
                    "--output-directory",
                    str(regions),
                ]
            ),
            0,
        )
        receipt = json.loads((regions / "receipt.json").read_text(encoding="utf-8"))
        self.assertEqual(len(receipt["regions"]), 6)

    def test_audit_detects_changed_source_identity(self) -> None:
        self.build_catalog()
        self.sources[0].write_bytes(b"changed")
        self.assertEqual(
            fixture.main(["audit", "--catalog", str(self.catalog_path)]), 2
        )

    def test_complete_candidate_evaluation_keeps_per_case_results(self) -> None:
        self.build_catalog()
        catalog = json.loads(self.catalog_path.read_text(encoding="utf-8"))
        analysis_arguments = []
        for matrix_fixture in catalog["fixtures"]:
            for case in matrix_fixture["cases"]:
                case_id = case["id"]
                unclipped = case["class"] == "ordinary-unclipped-control"
                analysis_path = self.root / f"{case_id}.analysis.json"
                analysis_path.write_text(
                    json.dumps(
                        {
                            "schema": "shadow.raw-highlight-objective-analysis.v1",
                            "region": {"id": case_id},
                            "topology": {
                                "manifest": {"sha256": "a" * 64},
                                "mask": "physical-white",
                                "selected_sample_count": 0 if unclipped else 16,
                            },
                            "resource_usage": {
                                "analysis_elapsed_ms": 1.0,
                                "analysis_peak_rss_bytes": 1024,
                            },
                            "candidates": [
                                {
                                    "id": "candidate-a",
                                    "clipped_core_source": {
                                        "mode": "cfa-topology",
                                        "selected_pixel_count": 0 if unclipped else 16,
                                    },
                                    "metrics": {
                                        "boundary_continuity": {
                                            "available": not unclipped,
                                            "mean_hue_error_degrees": 1.0
                                        },
                                        "false_colour_exterior": {
                                            "area_fraction": 0.01
                                        },
                                    },
                                }
                            ],
                        }
                    ),
                    encoding="utf-8",
                )
                analysis_arguments.extend(["--analysis", f"{case_id}={analysis_path}"])
        evaluation = self.root / "candidate-a.evaluation.json"
        self.assertEqual(
            fixture.main(
                [
                    "evaluate",
                    "--catalog",
                    str(self.catalog_path),
                    "--candidate-id",
                    "candidate-a",
                    "--require-cfa-topology",
                    *analysis_arguments,
                    "--output",
                    str(evaluation),
                ]
            ),
            0,
        )
        document = json.loads(evaluation.read_text(encoding="utf-8"))
        self.assertEqual(document["schema"], fixture.EVALUATION_SCHEMA)
        self.assertFalse(document["aggregation_policy"]["whole_image_average"])
        self.assertTrue(
            document["aggregation_policy"]["factual_cfa_topology_required"]
        )
        self.assertEqual(len(document["results"]), 6)
        self.assertTrue(
            all(
                result["clipped_core_source"]["mode"] == "cfa-topology"
                for result in document["results"]
            )
        )


if __name__ == "__main__":
    unittest.main()
