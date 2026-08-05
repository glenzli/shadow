from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
HELPER = PROJECT_ROOT / "scripts" / "acquire_debug_promotion_lock.py"


class DebugPromotionLockTest(unittest.TestCase):
    def run_helper(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["python3", str(HELPER), *arguments],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def test_acquire_records_owner_and_token_checked_release(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            lock_path = Path(temporary) / "promotion.lock"
            acquired = self.run_helper(
                "acquire", str(lock_path), "--owner", "agent-a"
            )
            self.assertEqual(acquired.returncode, 0, acquired.stderr)
            token = acquired.stdout.strip()
            metadata = json.loads(
                (lock_path / "owner.json").read_text(encoding="utf-8")
            )
            self.assertEqual(metadata["owner"], "agent-a")
            self.assertEqual(metadata["token"], token)

            wrong_owner = self.run_helper(
                "release", str(lock_path), "--token", "wrong-token"
            )
            self.assertEqual(wrong_owner.returncode, 73)
            self.assertTrue(lock_path.is_dir())

            released = self.run_helper(
                "release", str(lock_path), "--token", token
            )
            self.assertEqual(released.returncode, 0, released.stderr)
            self.assertFalse(lock_path.exists())

    def test_existing_lock_is_the_only_contention_result(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            lock_path = Path(temporary) / "promotion.lock"
            first = self.run_helper(
                "acquire", str(lock_path), "--owner", "agent-a"
            )
            self.assertEqual(first.returncode, 0, first.stderr)

            second = self.run_helper(
                "acquire", str(lock_path), "--owner", "agent-b"
            )
            self.assertEqual(second.returncode, 73)
            self.assertIn("may be active", second.stderr)
            self.assertIn('"owner": "agent-a"', second.stderr)
            self.assertNotIn(first.stdout.strip(), second.stderr)

            self.run_helper(
                "release", str(lock_path), "--token", first.stdout.strip()
            )

    def test_missing_parent_is_an_environment_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            lock_path = Path(temporary) / "missing" / "promotion.lock"
            failed = self.run_helper(
                "acquire", str(lock_path), "--owner", "agent-a"
            )
            self.assertEqual(failed.returncode, 77)
            self.assertIn("errno=ENOENT", failed.stderr)
            self.assertIn("not evidence that another steward", failed.stderr)

    def test_non_directory_lock_is_malformed_environment_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            lock_path = Path(temporary) / "promotion.lock"
            lock_path.write_text("not a directory\n", encoding="utf-8")
            failed = self.run_helper(
                "acquire", str(lock_path), "--owner", "agent-a"
            )
            self.assertEqual(failed.returncode, 77)
            self.assertIn("exists but is not a directory", failed.stderr)
            self.assertIn("not evidence", failed.stderr)


if __name__ == "__main__":
    unittest.main()
