"""Publication failures must preserve the last usable Windows deployment."""
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

MODULE_PATH = Path(__file__).resolve().parents[1] / "desktop_windows_runtime.py"
SPEC = importlib.util.spec_from_file_location("desktop_windows_runtime", MODULE_PATH)
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


class WindowsDeploymentContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.root = self.directory / "builds"
        self.source = self.directory / "source"
        self.source.mkdir()
        self.candidate = self.directory / "candidate"
        for name in runtime.REQUIRED:
            path = self.candidate / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())

    def publish(self):
        def git_output(arguments, **options):
            return "f" * 40 if options.get("text") else b" M source.cpp\n"
        with patch.object(runtime.subprocess, "check_output", side_effect=git_output):
            return runtime.promote(self.root, self.candidate, self.source, "validated-test")

    def test_new_release_keeps_previous_copy_and_rejects_missing_worker(self):
        first = self.publish()
        (self.candidate / "Shadow.exe").write_bytes(b"updated executable")
        second = self.publish()
        self.assertNotEqual(first, second)
        self.assertEqual((first / "Shadow.exe").read_bytes(), b"Shadow.exe")
        pointer = self.root / "current-debug-windows.json"
        published = pointer.read_bytes()
        self.assertEqual(json.loads(published)["previous_release"], first.relative_to(self.root).as_posix())
        self.assertEqual(runtime.current_release(self.root), second)
        (self.candidate / "shadow-photo-composition-worker.exe").unlink()
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            self.publish()
        self.assertEqual(pointer.read_bytes(), published)

    def test_pointer_cannot_escape_immutable_release_root(self):
        self.root.mkdir()
        (self.root / "current-debug-windows.json").write_text(json.dumps({"schema": 1, "release": "../candidate"}))
        with self.assertRaisesRegex(ValueError, "escapes"):
            runtime.current_release(self.root)

    def test_input_check_never_opens_an_application(self):
        self.publish()
        with patch.object(runtime.subprocess, "Popen") as spawn, patch("sys.stdout", io.StringIO()):
            runtime.launch(self.root, ["--check"])
            with self.assertRaisesRegex(ValueError, "extra arguments"):
                runtime.launch(self.root, ["--check", "unexpected"])
            spawn.assert_not_called()

    def test_server_debug_launch_preserves_explicit_library_choices(self):
        release = self.publish()
        cases = [
            ([], {}, ["--profile", "debug"]),
            (["--profile-launch-context", "debug"], {},
             ["--profile-launch-context", "debug", "--profile", "debug"]),
            (["--profile-launch-context=normal"], {},
             ["--profile-launch-context=normal", "--profile", "daily"]),
            (["--profile", "daily"], {}, ["--profile", "daily"]),
            (["--profile-root=D:/Libraries/Separate"], {},
             ["--profile-root=D:/Libraries/Separate"]),
            ([], {"SHADOW_SERVER_MANAGER_DATA_ROOT": "D:/Libraries/Separate"}, []),
            ([], {"SHADOW_SERVER_MANAGER_DATA_ROOT": ""}, []),
        ]
        for arguments, environment, expected in cases:
            with self.subTest(arguments=arguments, environment=environment):
                with patch.dict(runtime.os.environ, environment, clear=True), \
                        patch.object(runtime.subprocess, "Popen") as spawn, \
                        patch("sys.stdout", io.StringIO()):
                    runtime.launch(self.root, ["--server", *arguments])
                    self.assertEqual(spawn.call_args.args[0],
                                     [str(release / "Shadow Server.exe"), *expected])

    def test_photo_application_receives_its_development_launch_context(self):
        release = self.publish()
        arguments = ["--profile-launch-context", "debug", "--profile-root", "D:/Libraries/Separate"]
        with patch.object(runtime.subprocess, "Popen") as spawn, patch("sys.stdout", io.StringIO()):
            runtime.launch(self.root, arguments)
            self.assertEqual(spawn.call_args.args[0], [str(release / "Shadow.exe"), *arguments])


if __name__ == "__main__":
    unittest.main()
