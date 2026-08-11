from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest

import execution_baseline


class RawNindExecutionBaselineContract(unittest.TestCase):
    def test_checked_in_baseline_matches_current_execution_contract(self) -> None:
        baseline = execution_baseline.validate_baseline()
        receipt = execution_baseline.validation_receipt(baseline)
        self.assertEqual(receipt["result"], "validated")
        self.assertEqual(len(receipt["baseline_canonical_json_sha256"]), 64)
        self.assertEqual(len(receipt["baseline_file_sha256"]), 64)
        self.assertNotEqual(
            receipt["baseline_canonical_json_sha256"],
            receipt["baseline_file_sha256"],
        )
        self.assertFalse(receipt["contains_payload_paths"])

    def test_adapter_drift_is_rejected(self) -> None:
        baseline = execution_baseline.validate_baseline()
        changed = copy.deepcopy(baseline)
        changed["adapter_semantics"]["tiling"]["step_packed"] = 289
        with tempfile.TemporaryDirectory(dir="/private/tmp") as directory:
            path = Path(directory) / "changed-baseline.json"
            path.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "adapter semantics"):
                execution_baseline.validate_baseline(path)

    def test_policy_drift_is_rejected(self) -> None:
        baseline = execution_baseline.validate_baseline()
        changed = copy.deepcopy(baseline)
        changed["routing_policy"]["silent_legacy_fallback"] = True
        with tempfile.TemporaryDirectory(dir="/private/tmp") as directory:
            path = Path(directory) / "changed-baseline.json"
            path.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "routing policy"):
                execution_baseline.validate_baseline(path)

    def test_target_route_drift_is_rejected(self) -> None:
        baseline = execution_baseline.validate_baseline()
        changed = copy.deepcopy(baseline)
        changed["target_route"]["intent"] = "raw.generic_model"
        with tempfile.TemporaryDirectory(dir="/private/tmp") as directory:
            path = Path(directory) / "changed-baseline.json"
            path.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "target route"):
                execution_baseline.validate_baseline(path)

    def test_receipt_is_external_no_overwrite_and_contains_no_paths(self) -> None:
        baseline = execution_baseline.validate_baseline()
        receipt = execution_baseline.validation_receipt(baseline)
        with tempfile.TemporaryDirectory(dir="/private/tmp") as directory:
            destination = Path(directory) / "baseline-receipt.json"
            execution_baseline.write_external_receipt(destination, receipt)
            written = json.loads(destination.read_text(encoding="utf-8"))
            self.assertEqual(written, receipt)
            self.assertNotIn(str(execution_baseline.REPOSITORY_ROOT), written)
            with self.assertRaises(FileExistsError):
                execution_baseline.write_external_receipt(destination, receipt)

    def test_receipt_inside_repository_is_rejected(self) -> None:
        receipt = execution_baseline.validation_receipt(
            execution_baseline.validate_baseline()
        )
        with self.assertRaisesRegex(ValueError, "outside the repository"):
            execution_baseline.write_external_receipt(
                execution_baseline.SCRIPT_DIRECTORY / "forbidden-receipt.json",
                receipt,
            )


if __name__ == "__main__":
    unittest.main()
