from __future__ import annotations

import importlib.util
import json
import pathlib
import re
import struct
import sys
import tempfile
import unittest


OWNER_ROOT = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "shadow_raw_highlight_oracle", OWNER_ROOT / "oracle_lab.py"
)
assert SPEC is not None and SPEC.loader is not None
oracle = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = oracle
SPEC.loader.exec_module(oracle)


FAKE_EXECUTABLE = """#!/usr/bin/env python3
import pathlib
import shutil
import struct
import sys

args = sys.argv[1:]
name = pathlib.Path(sys.argv[0]).name
if args and args[0] == "raw-frame-staging":
    manifest = pathlib.Path(args[2])
    manifest.parent.mkdir(parents=True, exist_ok=True)
    samples = b"".join(struct.pack("<H", value) for value in range(16))
    pathlib.Path(str(manifest) + ".u16le").write_bytes(samples)
    matrix = "1,0,0,0,1,0,0,0,1"
    manifest.write_text(
        "shadow-raw-frame-staging-20260822.1 "
        "descriptor_contract=active-camera-colour-response-20260822.1 "
        "width=4 height=4 cfa=RGGB black=4,4,4,4 white=1023,1023,1023,1023 "
        "linear_response=900,900,900,900 has_linear_response=1 orientation=0 "
        "bits_per_sample=10 as_shot_neutral=2,1,1,1.5 "
        f"camera_to_xyz_d50={matrix} xyz_to_camera_d65={matrix} "
        f"camera_to_linear_srgb_d65={matrix} pending_dng_opcode_bytes=0,0,0 "
        "provider_id_hex=736861646f772e74657374 provider_version_hex=312e30 "
        "sample_bytes=32\\n",
        encoding="utf-8",
    )
    print("shadow-raw-frame-staging-20260822.1 raw-frame-staging")
elif "--raw-frame-only" in args:
    output = pathlib.Path(args[1])
    output.mkdir(parents=True, exist_ok=True)
    (output / "raw-frame.bin").write_bytes(b"raw-frame")
    print("raw.width=4")
elif "--highlight-cfa-diagnostic" in args:
    output = pathlib.Path(args[1])
    output.mkdir(parents=True, exist_ok=True)
    (output / "difference.ppm").write_bytes(b"P6\\n1 1\\n255\\n\\0\\0\\0")
    print("highlight.reference=darktable-opposed")
elif "-D" in args and "-Z" in args:
    output = pathlib.Path(args[args.index("-Z") + 1])
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args[-1], output)
elif "-T" in args and args[-1].endswith(".dng"):
    output = pathlib.Path(args[-1] + ".tiff")
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args[-1], output)
elif "-Z" in args:
    output = pathlib.Path(args[args.index("-Z") + 1])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(b"libraw-tiff")
elif "-o" in args:
    output = pathlib.Path(args[args.index("-o") + 1])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(b"rawtherapee-tiff")
elif "--apply-custom-presets" in args:
    output_index = 2 if len(args) > 2 and args[1].endswith(".xmp") else 1
    output = pathlib.Path(args[output_index])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(b"darktable-tiff")
elif "--filename" in args:
    output = pathlib.Path(args[args.index("--filename") + 1]).with_suffix(".pfm")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(b"PF\\n1 1\\n-1.0\\n" + bytes(12))
else:
    print(f"identify.executable={name}")
"""


class OracleLabContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="shadow-oracle-test-")
        self.root = pathlib.Path(self.temporary.name)
        self.source = self.root / "fixture.raw"
        self.source.write_bytes(b"bounded-raw-fixture")
        self.executable = self.root / "fake-oracle"
        self.executable.write_text(FAKE_EXECUTABLE, encoding="utf-8")
        self.executable.chmod(0o755)
        self.xmp = self.root / "controlled.xmp"
        self.xmp.write_text("<x:xmpmeta/>", encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def all_tool_arguments(self) -> list[str]:
        executable = str(self.executable)
        return [
            "--shadow-probe",
            executable,
            "--shadow-decode-helper",
            executable,
            "--raw-identify",
            executable,
            "--dcraw-emu",
            executable,
            "--unprocessed-raw",
            executable,
            "--darktable-cli",
            executable,
            "--darktable-xmp",
            str(self.xmp),
            "--rawtherapee-cli",
            executable,
            "--vkdt-cli",
            executable,
        ]

    def test_output_root_must_be_external_to_repository(self) -> None:
        self.assertFalse(oracle.output_root_is_external(oracle.REPOSITORY_ROOT / "oracle-output"))
        self.assertTrue(oracle.output_root_is_external(self.root / "oracle-output"))

    def test_inventory_requires_explicit_darktable_xmp(self) -> None:
        arguments = oracle.parser().parse_args(
            [
                "inventory",
                "--oracle",
                "darktable-xmp",
                "--darktable-cli",
                str(self.executable),
            ]
        )
        tools = oracle.selected_tools(arguments)
        plan = oracle.build_plan(
            "darktable-xmp", tools, pathlib.Path("/INPUT.raw"), pathlib.Path("/RUN")
        )
        self.assertEqual(plan, "a controlled XMP is required; pass --darktable-xmp")

    def test_bundled_darktable_profiles_differ_only_in_highlight_mode(self) -> None:
        parameters = []
        for name in (
            "darktable-highlights-clip.xmp",
            "darktable-highlights-opposed.xmp",
        ):
            contents = (OWNER_ROOT / "profiles" / name).read_text(encoding="utf-8")
            match = re.search(r'darktable:params="([0-9a-f]+)"', contents)
            self.assertIsNotNone(match)
            assert match is not None
            parameters.append(bytes.fromhex(match.group(1)))
        clip, opposed = parameters
        self.assertEqual(len(clip), 48)
        self.assertEqual(struct.unpack("<i", clip[:4]), (0,))
        self.assertEqual(struct.unpack("<i", opposed[:4]), (5,))
        self.assertEqual(clip[4:], opposed[4:])

    def test_rawtherapee_macos_launcher_resolves_to_direct_cli(self) -> None:
        launcher = self.root / "rawtherapee-cli"
        direct = self.root / "rawtherapee-cli-bin"
        launcher.write_text(FAKE_EXECUTABLE, encoding="utf-8")
        direct.write_text(FAKE_EXECUTABLE, encoding="utf-8")
        launcher.chmod(0o755)
        direct.chmod(0o755)

        self.assertEqual(
            oracle.resolve_rawtherapee_executable(str(launcher)), direct.resolve()
        )
        arguments = oracle.parser().parse_args(
            ["inventory", "--rawtherapee-cli", str(launcher)]
        )
        plan = oracle.build_plan(
            "rawtherapee-disabled",
            oracle.selected_tools(arguments),
            pathlib.Path("/INPUT.raw"),
            pathlib.Path("/RUN"),
        )
        self.assertIsInstance(plan, oracle.AdapterPlan)
        assert isinstance(plan, oracle.AdapterPlan)
        self.assertEqual(plan.executable, direct.resolve())
        self.assertEqual(set(plan.environment), {"RT_SETTINGS", "RT_CACHE"})

    def test_normalized_parent_receipt_requires_exact_artifact(self) -> None:
        parent_manifest = self.root / "parent-manifest.json"
        source_sha = oracle.sha256_file(self.source)
        parent_manifest.write_text(
            json.dumps(
                {
                    "schema": oracle.MANIFEST_SCHEMA,
                    "run_name": "parent-run",
                    "source": {"sha256": "1" * 64},
                    "adapters": [
                        {
                            "id": "shadow-normalized-dng",
                            "status": "succeeded",
                            "artifacts": [
                                {
                                    "path": "normalized.dng",
                                    "size_bytes": self.source.stat().st_size,
                                    "sha256": source_sha,
                                }
                            ],
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        receipt = oracle.normalized_parent_receipt(self.source, str(parent_manifest))
        assert receipt is not None
        self.assertEqual(receipt["run_name"], "parent-run")
        self.assertEqual(receipt["artifact_sha256"], source_sha)
        output_root = self.root / "parent-linked-runs"
        self.assertEqual(
            oracle.main(
                [
                    "run",
                    "--input",
                    str(self.source),
                    "--normalized-parent-manifest",
                    str(parent_manifest),
                    "--output-root",
                    str(output_root),
                    "--run-name",
                    "parent-linked-child",
                    "--oracle",
                    "rawtherapee-disabled",
                    "--rawtherapee-cli",
                    str(self.executable),
                    "--dry-run",
                ]
            ),
            0,
        )
        child = json.loads(
            (output_root / "parent-linked-child" / "manifest.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(
            child["comparison_boundary"]["normalized_interchange_status"],
            "verified-parent",
        )
        self.assertEqual(
            child["comparison_boundary"]["normalized_children"],
            ["rawtherapee-disabled"],
        )
        self.source.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "does not match"):
            oracle.normalized_parent_receipt(self.source, str(parent_manifest))

    def test_all_adapters_record_reproducible_redacted_run(self) -> None:
        output_root = self.root / "runs"
        arguments = [
            "run",
            "--input",
            str(self.source),
            "--output-root",
            str(output_root),
            "--run-name",
            "all-adapters",
            "--strict",
            *self.all_tool_arguments(),
        ]
        self.assertEqual(oracle.main(arguments), 0)

        run_directory = output_root / "all-adapters"
        manifest = json.loads((run_directory / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], oracle.MANIFEST_SCHEMA)
        self.assertFalse(manifest["source"]["copied_into_run"])
        self.assertNotIn(str(self.source), json.dumps(manifest))
        self.assertNotIn(str(run_directory), json.dumps(manifest))
        for log in run_directory.rglob("command.*.log"):
            contents = log.read_text(encoding="utf-8")
            self.assertNotIn(str(self.source), contents)
            self.assertNotIn(str(run_directory), contents)
        self.assertEqual(
            {record["id"] for record in manifest["adapters"]}, set(oracle.DEFAULT_ADAPTERS)
        )
        self.assertTrue(all(record["status"] == "succeeded" for record in manifest["adapters"]))
        self.assertFalse(any(path.name == self.source.name for path in run_directory.rglob("*")))
        self.assertTrue(
            all(record["executable"]["sha256"] for record in manifest["adapters"])
        )
        supporting_files = {
            record["id"]: record["supporting_files"] for record in manifest["adapters"]
        }
        self.assertEqual(len(supporting_files["darktable-xmp"]), 1)
        self.assertEqual(len(supporting_files["darktable-highlights-clip"]), 1)
        self.assertEqual(len(supporting_files["darktable-highlights-opposed"]), 1)
        self.assertEqual(len(supporting_files["rawtherapee-coloropp"]), 1)
        self.assertEqual(len(supporting_files["vkdt-hilite"]), 1)
        self.assertTrue(supporting_files["darktable-xmp"][0]["sha256"])

        darktable_records = {
            record["id"]: record
            for record in manifest["adapters"]
            if record["id"].startswith("darktable-highlights-")
        }
        self.assertEqual(
            {
                record["comparison_class"]
                for record in darktable_records.values()
            },
            {"same-darktable-pipeline-highlight-ablation"},
        )
        clip_profile = pathlib.Path(
            darktable_records["darktable-highlights-clip"]["supporting_files"][0]["path"]
        )
        opposed_profile = pathlib.Path(
            darktable_records["darktable-highlights-opposed"]["supporting_files"][0]["path"]
        )
        self.assertNotEqual(clip_profile.name, opposed_profile.name)

        with self.assertRaisesRegex(ValueError, "already exists"):
            oracle.run(oracle.parser().parse_args(arguments))

    def test_required_unavailable_adapter_fails_without_substitution(self) -> None:
        output_root = self.root / "missing-runs"
        status = oracle.main(
            [
                "run",
                "--input",
                str(self.source),
                "--output-root",
                str(output_root),
                "--run-name",
                "missing-darktable",
                "--oracle",
                "darktable-xmp",
                "--require",
                "darktable-xmp",
                "--dry-run",
            ]
        )
        self.assertEqual(status, 1)
        manifest = json.loads(
            (output_root / "missing-darktable" / "manifest.json").read_text(encoding="utf-8")
        )
        self.assertEqual(manifest["adapters"][0]["status"], "unavailable")


if __name__ == "__main__":
    unittest.main()
