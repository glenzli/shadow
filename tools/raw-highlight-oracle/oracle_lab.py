#!/usr/bin/env python3
"""Run bounded, provenance-aware RAW highlight reconstruction oracles.

This is an offline development tool.  It never participates in Shadow's desktop,
preview, detail, export, cache, or Recipe paths, and it never copies the source RAW
into its result directory.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Iterable, Sequence


TOOL_ROOT = pathlib.Path(__file__).resolve().parent
REPOSITORY_ROOT = TOOL_ROOT.parents[1]
UPSTREAM_LOCK = TOOL_ROOT / "upstreams.lock.json"
MANIFEST_SCHEMA = "shadow.raw-highlight-oracle-run.v1"
ORCHESTRATOR_VERSION = "20260826.1"
SAFE_RUN_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$")

BUILTIN_DARKTABLE_PROFILES = {
    "darktable-highlights-clip": "darktable-highlights-clip.xmp",
    "darktable-highlights-opposed": "darktable-highlights-opposed.xmp",
}

DEFAULT_ADAPTERS = (
    "shadow-rawframe",
    "shadow-normalized-dng",
    "shadow-cfa-opposed",
    "shadow-threshold-ablation",
    "libraw-identify",
    "libraw-h0-clip",
    "libraw-h2-blend",
    "libraw-h3-rebuild",
    "rawtherapee-disabled",
    "rawtherapee-coloropp",
    "rawtherapee-color-propagation",
    "darktable-highlights-clip",
    "darktable-highlights-opposed",
    "darktable-default",
    "darktable-xmp",
    "vkdt-hilite-disabled",
    "vkdt-hilite",
)


@dataclasses.dataclass(frozen=True)
class AdapterPlan:
    adapter_id: str
    stages: tuple[str, ...]
    comparison_class: str
    executable: pathlib.Path
    argv: tuple[str, ...]
    working_directory: pathlib.Path
    environment: dict[str, str]
    supporting_files: tuple[pathlib.Path, ...] = ()
    notes: tuple[str, ...] = ()


@dataclasses.dataclass(frozen=True)
class ToolSelection:
    shadow_probe: pathlib.Path | None
    shadow_decode_helper: pathlib.Path | None
    raw_identify: pathlib.Path | None
    dcraw_emu: pathlib.Path | None
    unprocessed_raw: pathlib.Path | None
    darktable_cli: pathlib.Path | None
    darktable_xmp: pathlib.Path | None
    rawtherapee_cli: pathlib.Path | None
    vkdt_cli: pathlib.Path | None
    vkdt_icd: pathlib.Path | None


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def resolve_executable(explicit: str | None, candidates: Sequence[str]) -> pathlib.Path | None:
    if explicit:
        expanded = pathlib.Path(explicit).expanduser()
        if expanded.parent != pathlib.Path(".") or expanded.is_absolute():
            resolved = expanded.resolve(strict=False)
            return resolved if resolved.is_file() and os.access(resolved, os.X_OK) else None
        located = shutil.which(explicit)
        return pathlib.Path(located).resolve() if located else None
    for candidate in candidates:
        located = shutil.which(candidate)
        if located:
            return pathlib.Path(located).resolve()
    return None


def resolve_optional_file(value: str | None) -> pathlib.Path | None:
    if not value:
        return None
    resolved = pathlib.Path(value).expanduser().resolve(strict=False)
    return resolved if resolved.is_file() else None


def resolve_rawtherapee_executable(explicit: str | None) -> pathlib.Path | None:
    """Resolve the real macOS CLI, bypassing RawTherapee's app launcher.

    The cask exposes ``rawtherapee-cli`` from the application bundle.  In some
    macOS builds that file is a small launcher which can abort before the CLI
    starts, while its sibling ``rawtherapee-cli-bin`` is the actual executable.
    Other platforms and caller-supplied test tools remain unchanged.
    """

    resolved = resolve_executable(explicit, ("rawtherapee-cli",))
    if resolved is None or resolved.name != "rawtherapee-cli":
        return resolved
    direct = resolved.with_name("rawtherapee-cli-bin")
    if direct.is_file() and os.access(direct, os.X_OK):
        return direct.resolve()
    return resolved


def output_root_is_external(output_root: pathlib.Path) -> bool:
    resolved = output_root.expanduser().resolve(strict=False)
    repository = REPOSITORY_ROOT.resolve()
    return resolved != repository and repository not in resolved.parents


def executable_identity(path: pathlib.Path) -> dict[str, object]:
    stat = path.stat()
    return {
        "name": path.name,
        "resolved_path": str(path),
        "size_bytes": stat.st_size,
        "sha256": sha256_file(path),
    }


def load_upstream_lock() -> dict[str, object]:
    with UPSTREAM_LOCK.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def normalized_parent_receipt(
    source: pathlib.Path, manifest_value: str | None
) -> dict[str, object] | None:
    if manifest_value is None:
        return None
    manifest_path = pathlib.Path(manifest_value).expanduser().resolve(strict=False)
    if not manifest_path.is_file():
        raise ValueError("normalized parent manifest does not exist")
    try:
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("normalized parent manifest is unreadable") from exc
    if document.get("schema") != MANIFEST_SCHEMA:
        raise ValueError("normalized parent manifest schema is unsupported")
    parent_run_name = document.get("run_name")
    if not isinstance(parent_run_name, str) or not SAFE_RUN_NAME.fullmatch(parent_run_name):
        raise ValueError("normalized parent manifest run name is invalid")
    matches: list[dict[str, object]] = []
    for adapter in document.get("adapters", []):
        if not isinstance(adapter, dict):
            continue
        if adapter.get("id") != "shadow-normalized-dng" or adapter.get("status") != "succeeded":
            continue
        for artifact in adapter.get("artifacts", []):
            if isinstance(artifact, dict) and artifact.get("path") == "normalized.dng":
                matches.append(artifact)
    source_sha = sha256_file(source)
    exact = [
        artifact
        for artifact in matches
        if artifact.get("sha256") == source_sha
        and artifact.get("size_bytes") == source.stat().st_size
    ]
    if len(exact) != 1:
        raise ValueError("input does not match exactly one normalized DNG parent artifact")
    parent_source = document.get("source")
    parent_source_sha = parent_source.get("sha256") if isinstance(parent_source, dict) else None
    if not isinstance(parent_source_sha, str) or re.fullmatch(r"[0-9a-f]{64}", parent_source_sha) is None:
        raise ValueError("normalized parent manifest source identity is invalid")
    return {
        "manifest_schema": MANIFEST_SCHEMA,
        "manifest_sha256": sha256_file(manifest_path),
        "run_name": parent_run_name,
        "adapter_id": "shadow-normalized-dng",
        "artifact_path": "normalized.dng",
        "artifact_sha256": source_sha,
        "artifact_size_bytes": source.stat().st_size,
        "source_raw_sha256": parent_source_sha,
    }


def redaction_pairs(source: pathlib.Path, run_directory: pathlib.Path) -> tuple[tuple[str, str], ...]:
    pairs = (
        (str(source), "<INPUT_RAW>"),
        (str(run_directory), "<RUN_DIRECTORY>"),
        (str(REPOSITORY_ROOT), "<SHADOW_REPOSITORY>"),
    )
    return tuple(sorted(pairs, key=lambda pair: len(pair[0]), reverse=True))


def redact(value: str, pairs: Iterable[tuple[str, str]]) -> str:
    result = value
    for original, replacement in pairs:
        result = result.replace(original, replacement)
    return result


def parse_key_value_evidence(stdout: str, pairs: Iterable[tuple[str, str]]) -> dict[str, str]:
    evidence: dict[str, str] = {}
    for line in stdout.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if not key or any(character.isspace() for character in key):
            continue
        evidence[key] = redact(value.strip(), pairs)
    return evidence


def artifact_manifest(directory: pathlib.Path) -> list[dict[str, object]]:
    artifacts: list[dict[str, object]] = []
    excluded = {"command.stdout.log", "command.stderr.log"}
    for path in sorted(directory.rglob("*")):
        if not path.is_file() or path.name in excluded:
            continue
        artifacts.append(
            {
                "path": str(path.relative_to(directory)),
                "size_bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    return artifacts


def adapter_directory(run_directory: pathlib.Path, adapter_id: str) -> pathlib.Path:
    return run_directory / "adapters" / adapter_id


def rawtherapee_plan(
    adapter_id: str,
    profile_name: str,
    tools: ToolSelection,
    source: pathlib.Path,
    run_directory: pathlib.Path,
) -> AdapterPlan | str:
    if tools.rawtherapee_cli is None:
        return "rawtherapee-cli was not found; pass --rawtherapee-cli"
    directory = adapter_directory(run_directory, adapter_id)
    profile = TOOL_ROOT / "profiles" / profile_name
    output = directory / "result.tif"
    environment = {
        "RT_SETTINGS": str(directory / "settings"),
        "RT_CACHE": str(directory / "cache"),
    }
    return AdapterPlan(
        adapter_id=adapter_id,
        stages=("container-decode", "raw-reconstruction", "demosaic", "colour"),
        comparison_class="independent-container-pipeline",
        executable=tools.rawtherapee_cli,
        argv=(
            str(tools.rawtherapee_cli),
            "-q",
            "-o",
            str(output),
            "-p",
            str(profile),
            "-t",
            "-b16",
            "-Y",
            "-c",
            str(source),
        ),
        working_directory=directory,
        environment=environment,
        supporting_files=(profile,),
        notes=(
            "RawTherapee starts from neutral parameters and applies only the named partial PP3.",
            "Decoder and reconstruction differences are intentionally not attributed to one stage.",
        ),
    )


def darktable_plan(
    adapter_id: str,
    tools: ToolSelection,
    source: pathlib.Path,
    run_directory: pathlib.Path,
) -> AdapterPlan | str:
    if tools.darktable_cli is None:
        return "darktable-cli was not found; pass --darktable-cli"
    if adapter_id == "darktable-xmp" and tools.darktable_xmp is None:
        return "a controlled XMP is required; pass --darktable-xmp"

    directory = adapter_directory(run_directory, adapter_id)
    output = directory / "result.tif"
    input_arguments = [str(source)]
    supporting_files: tuple[pathlib.Path, ...] = ()
    controlled_ablation = adapter_id in BUILTIN_DARKTABLE_PROFILES
    if controlled_ablation:
        profile = TOOL_ROOT / "profiles" / BUILTIN_DARKTABLE_PROFILES[adapter_id]
        input_arguments.append(str(profile))
        supporting_files = (profile,)
    elif adapter_id == "darktable-xmp":
        assert tools.darktable_xmp is not None
        input_arguments.append(str(tools.darktable_xmp))
        supporting_files = (tools.darktable_xmp,)
    input_arguments.append(str(output))

    if controlled_ablation:
        highlight_stage = (
            "cfa-opposed-reconstruction"
            if adapter_id == "darktable-highlights-opposed"
            else "cfa-highlight-clip"
        )
        comparison_class = "same-darktable-pipeline-highlight-ablation"
        note = (
            "The bundled XMP profiles are byte-identical except for the highlight "
            "module mode: clip versus inpaint opposed."
        )
    elif adapter_id == "darktable-default":
        highlight_stage = "raw-reconstruction"
        comparison_class = "independent-container-pipeline"
        note = (
            "The in-memory empty library selects Darktable's executable-defined "
            "default history without reading an adjacent sidecar."
        )
    else:
        highlight_stage = "raw-reconstruction"
        comparison_class = "independent-container-pipeline"
        note = (
            "A caller-supplied XMP is mandatory so adjacent user sidecars "
            "cannot affect the run."
        )

    return AdapterPlan(
        adapter_id=adapter_id,
        stages=("container-decode", highlight_stage, "demosaic", "colour"),
        comparison_class=comparison_class,
        executable=tools.darktable_cli,
        argv=(
            str(tools.darktable_cli),
            *input_arguments,
            "--apply-custom-presets",
            "false",
            "--hq",
            "true",
            "--core",
            "--configdir",
            str(directory / "config"),
            "--cachedir",
            str(directory / "cache"),
            "--tmpdir",
            str(directory / "tmp"),
            "--library",
            ":memory:" if adapter_id == "darktable-default" else str(directory / "library.db"),
            "--conf",
            "plugins/imageio/format/tiff/bpp=16",
        ),
        working_directory=directory,
        environment={},
        supporting_files=supporting_files,
        notes=(note,),
    )


def build_plan(
    adapter_id: str,
    tools: ToolSelection,
    source: pathlib.Path,
    run_directory: pathlib.Path,
) -> AdapterPlan | str:
    directory = adapter_directory(run_directory, adapter_id)
    if adapter_id == "shadow-rawframe":
        if tools.shadow_probe is None:
            return "shadow-raw-probe was not found; pass --shadow-probe"
        return AdapterPlan(
            adapter_id=adapter_id,
            stages=("shadow-provider-route", "provider-neutral-rawframe"),
            comparison_class="shadow-decoder-evidence",
            executable=tools.shadow_probe,
            argv=(
                str(tools.shadow_probe),
                str(source),
                str(directory),
                "--raw-frame-only",
            ),
            working_directory=directory,
            environment={},
            notes=("Writes the untouched provider-neutral uint16 sensor plane and descriptor log.",),
        )
    if adapter_id == "shadow-normalized-dng":
        if tools.shadow_decode_helper is None:
            return "shadow-image-decode-helper was not found; pass --shadow-decode-helper"
        if tools.raw_identify is None:
            return "raw-identify is required for independent DNG recognition"
        if tools.unprocessed_raw is None:
            return "LibRaw unprocessed_raw is required for independent CFA verification"
        script = TOOL_ROOT / "research_dng.py"
        normalized_owner = TOOL_ROOT / "normalized_mosaic.py"
        return AdapterPlan(
            adapter_id=adapter_id,
            stages=(
                "shadow-provider-route",
                "provider-neutral-active-rawframe",
                "lossless-research-dng",
                "libraw-unprocessed-reimport",
            ),
            comparison_class="same-decoded-cfa-interchange",
            executable=pathlib.Path(sys.executable).resolve(),
            argv=(
                sys.executable,
                str(script),
                "from-raw",
                "--decode-helper",
                str(tools.shadow_decode_helper),
                "--input",
                str(source),
                "--output-directory",
                str(directory),
                "--raw-identify",
                str(tools.raw_identify),
                "--unprocessed-raw",
                str(tools.unprocessed_raw),
            ),
            working_directory=TOOL_ROOT,
            environment={},
            supporting_files=(script, normalized_owner),
            notes=(
                "The active CFA strip is copied byte-for-byte from Shadow RawFrame staging.",
                "LibRaw unprocessed_raw must independently re-export identical uint16 samples.",
                "DNGPrivateData preserves exact four-site metadata that standard DNG cannot express.",
            ),
        )
    if adapter_id == "shadow-cfa-opposed":
        if tools.shadow_probe is None:
            return "shadow-raw-probe was not found; pass --shadow-probe"
        return AdapterPlan(
            adapter_id=adapter_id,
            stages=("shadow-provider-route", "provider-neutral-rawframe", "cfa-reconstruction"),
            comparison_class="same-decoded-cfa-reference",
            executable=tools.shadow_probe,
            argv=(
                str(tools.shadow_probe),
                str(source),
                str(directory),
                "--highlight-cfa-diagnostic",
            ),
            working_directory=directory,
            environment={},
            notes=(
                "Shadow current and the pinned Darktable opposed reference share the exact RawFrame.",
                "This is the first strict same-decoder reconstruction comparison.",
            ),
        )
    if adapter_id == "shadow-threshold-ablation":
        if tools.shadow_probe is None:
            return "shadow-raw-probe was not found; pass --shadow-probe"
        return AdapterPlan(
            adapter_id=adapter_id,
            stages=(
                "shadow-provider-route",
                "provider-neutral-rawframe",
                "fixed-opposed-reconstruction",
                "threshold-domain-ablation",
            ),
            comparison_class="same-decoded-cfa-threshold-ablation",
            executable=tools.shadow_probe,
            argv=(
                str(tools.shadow_probe),
                str(source),
                str(directory),
                "--highlight-threshold-ablation",
            ),
            working_directory=directory,
            environment={},
            notes=(
                "Both branches share RawFrame, white balance, compiled opposed chrominance, "
                "area sampling, and camera matrix.",
                "Only response-limit versus physical-white candidate admission changes.",
                "The adapter is offline evidence and cannot affect Shadow product rendering.",
            ),
        )
    if adapter_id == "libraw-identify":
        if tools.raw_identify is None:
            return "raw-identify was not found; pass --raw-identify"
        return AdapterPlan(
            adapter_id=adapter_id,
            stages=("container-identification", "decoder-metadata"),
            comparison_class="independent-container-decode",
            executable=tools.raw_identify,
            argv=(str(tools.raw_identify), "-v", "-w", str(source)),
            working_directory=directory,
            environment={},
        )
    if adapter_id.startswith("libraw-h"):
        if tools.dcraw_emu is None:
            return "dcraw_emu was not found; pass --dcraw-emu"
        modes = {
            "libraw-h0-clip": "0",
            "libraw-h2-blend": "2",
            "libraw-h3-rebuild": "3",
        }
        mode = modes[adapter_id]
        output = directory / "result.tiff"
        return AdapterPlan(
            adapter_id=adapter_id,
            stages=("container-decode", "raw-reconstruction", "demosaic", "colour"),
            comparison_class="independent-container-pipeline",
            executable=tools.dcraw_emu,
            argv=(
                str(tools.dcraw_emu),
                "-v",
                "-w",
                "-4",
                "-q",
                "3",
                "-H",
                mode,
                "-T",
                "-Z",
                str(output),
                str(source),
            ),
            working_directory=directory,
            environment={},
            notes=("Linear 16-bit output with camera white balance and no automatic brightening.",),
        )
    if adapter_id == "rawtherapee-disabled":
        return rawtherapee_plan(
            adapter_id, "rawtherapee-disabled.pp3", tools, source, run_directory
        )
    if adapter_id == "rawtherapee-coloropp":
        return rawtherapee_plan(
            adapter_id, "rawtherapee-coloropp.pp3", tools, source, run_directory
        )
    if adapter_id == "rawtherapee-color-propagation":
        return rawtherapee_plan(
            adapter_id, "rawtherapee-color-propagation.pp3", tools, source, run_directory
        )
    if adapter_id in {
        "darktable-default",
        "darktable-xmp",
        *BUILTIN_DARKTABLE_PROFILES,
    }:
        return darktable_plan(adapter_id, tools, source, run_directory)
    if adapter_id in {"vkdt-hilite-disabled", "vkdt-hilite"}:
        if tools.vkdt_cli is None:
            return "vkdt-cli was not found; pass --vkdt-cli"
        output = directory / "result"
        enabled = adapter_id == "vkdt-hilite"
        graph = TOOL_ROOT / "graphs" / (
            "vkdt-hilite.cfg" if enabled else "vkdt-hilite-disabled.cfg"
        )
        environment = (
            {"VK_ICD_FILENAMES": str(tools.vkdt_icd)}
            if tools.vkdt_icd is not None
            else {}
        )
        supporting_files = (
            (graph, tools.vkdt_icd) if tools.vkdt_icd is not None else (graph,)
        )
        return AdapterPlan(
            adapter_id=adapter_id,
            stages=(
                ("container-decode", "raw-denoise", "cfa-inpaint", "demosaic", "colour")
                if enabled
                else ("container-decode", "raw-denoise", "demosaic", "colour")
            ),
            comparison_class="same-vkdt-pipeline-highlight-ablation",
            executable=tools.vkdt_cli,
            argv=(
                str(tools.vkdt_cli),
                "-g",
                str(graph),
                "--last-frame-only",
                "--filename",
                str(output),
                "--format",
                "o-pfm",
                "--colour-prim",
                "sRGB",
                "--colour-trc",
                "linear",
                "--config",
                f"param:i-raw:main:filename:{source}",
            ),
            working_directory=tools.vkdt_cli.parent,
            environment=environment,
            supporting_files=supporting_files,
            notes=(
                (
                    "vkdt reconstructs highlights in raw mosaic space before demosaic."
                    if enabled
                    else "This baseline removes only vkdt's raw-mosaic hilite node."
                ),
                "The graph intentionally omits display tone mapping and exports linear PFM.",
                "On macOS the adapter requires host Metal access; a sandboxed process may report "
                "VK_ERROR_INCOMPATIBLE_DRIVER even when the recorded MoltenVK ICD is valid.",
            ),
        )
    raise ValueError(f"unknown adapter: {adapter_id}")


def planned_record(
    adapter_id: str,
    plan_or_reason: AdapterPlan | str,
    source: pathlib.Path,
    run_directory: pathlib.Path,
) -> dict[str, object]:
    if isinstance(plan_or_reason, str):
        return {
            "id": adapter_id,
            "status": "unavailable",
            "reason": plan_or_reason,
        }
    plan = plan_or_reason
    pairs = redaction_pairs(source, run_directory)
    return {
        "id": adapter_id,
        "status": "planned",
        "stages": list(plan.stages),
        "comparison_class": plan.comparison_class,
        "executable": executable_identity(plan.executable),
        "argv": [redact(argument, pairs) for argument in plan.argv],
        "working_directory": redact(str(plan.working_directory), pairs),
        "environment_overrides": {
            key: redact(value, pairs) for key, value in sorted(plan.environment.items())
        },
        "supporting_files": [
            {
                "name": path.name,
                "path": redact(str(path), pairs),
                "size_bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
            for path in plan.supporting_files
        ],
        "notes": list(plan.notes),
    }


def run_adapter(
    record: dict[str, object],
    plan: AdapterPlan,
    source: pathlib.Path,
    run_directory: pathlib.Path,
    timeout_seconds: float,
) -> None:
    directory = adapter_directory(run_directory, plan.adapter_id)
    directory.mkdir(parents=True, exist_ok=False)
    for key in ("RT_SETTINGS", "RT_CACHE"):
        value = plan.environment.get(key)
        if value is not None:
            pathlib.Path(value).mkdir(parents=True, exist_ok=True)
    if plan.adapter_id.startswith("darktable-"):
        for name in ("config", "cache", "tmp"):
            (directory / name).mkdir(parents=True, exist_ok=True)

    environment = os.environ.copy()
    environment.update(plan.environment)
    environment["LC_ALL"] = "C"
    environment["TZ"] = "UTC"
    started = time.monotonic()
    stdout = ""
    stderr = ""
    exit_code: int | None = None
    error: str | None = None
    try:
        completed = subprocess.run(
            plan.argv,
            cwd=plan.working_directory,
            env=environment,
            check=False,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout_seconds,
        )
        exit_code = completed.returncode
        stdout = completed.stdout
        stderr = completed.stderr
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        error = f"timed out after {timeout_seconds:.1f} seconds"
    except OSError as exc:
        error = f"failed to execute adapter: {exc}"
    elapsed_ms = (time.monotonic() - started) * 1000.0

    pairs = redaction_pairs(source, run_directory)
    stdout = redact(stdout, pairs)
    stderr = redact(stderr, pairs)
    (directory / "command.stdout.log").write_text(stdout, encoding="utf-8")
    (directory / "command.stderr.log").write_text(stderr, encoding="utf-8")
    record.update(
        {
            "status": "succeeded" if exit_code == 0 and error is None else "failed",
            "exit_code": exit_code,
            "elapsed_ms": round(elapsed_ms, 3),
            "stdout_log": "command.stdout.log",
            "stderr_log": "command.stderr.log",
            "evidence": parse_key_value_evidence(stdout, pairs),
            "artifacts": artifact_manifest(directory),
        }
    )
    if error is not None:
        record["error"] = error


def write_manifest(path: pathlib.Path, manifest: dict[str, object]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def selected_tools(args: argparse.Namespace) -> ToolSelection:
    return ToolSelection(
        shadow_probe=resolve_executable(args.shadow_probe, ("shadow-raw-probe",)),
        shadow_decode_helper=resolve_executable(
            args.shadow_decode_helper, ("shadow-image-decode-helper",)
        ),
        raw_identify=resolve_executable(args.raw_identify, ("raw-identify",)),
        dcraw_emu=resolve_executable(args.dcraw_emu, ("dcraw_emu",)),
        unprocessed_raw=resolve_executable(args.unprocessed_raw, ("unprocessed_raw",)),
        darktable_cli=resolve_executable(
            args.darktable_cli,
            ("darktable-cli", "/Applications/darktable.app/Contents/MacOS/darktable-cli"),
        ),
        darktable_xmp=resolve_optional_file(args.darktable_xmp),
        rawtherapee_cli=resolve_rawtherapee_executable(args.rawtherapee_cli),
        vkdt_cli=resolve_executable(args.vkdt_cli, ("vkdt-cli",)),
        vkdt_icd=resolve_optional_file(args.vkdt_icd),
    )


def adapter_ids(args: argparse.Namespace) -> tuple[str, ...]:
    selected = tuple(dict.fromkeys(args.oracle or DEFAULT_ADAPTERS))
    unknown = sorted(set(selected).difference(DEFAULT_ADAPTERS))
    if unknown:
        raise ValueError(f"unknown oracle adapter(s): {', '.join(unknown)}")
    return selected


def inventory(args: argparse.Namespace) -> int:
    tools = selected_tools(args)
    source = pathlib.Path("/INPUT.raw")
    run_directory = pathlib.Path("/RUN")
    records = []
    for adapter_id in adapter_ids(args):
        plan = build_plan(adapter_id, tools, source, run_directory)
        records.append(planned_record(adapter_id, plan, source, run_directory))
    print(
        json.dumps(
            {
                "schema": "shadow.raw-highlight-oracle-inventory.v1",
                "orchestrator_version": ORCHESTRATOR_VERSION,
                "adapters": records,
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0


def run(args: argparse.Namespace) -> int:
    source = pathlib.Path(args.input).expanduser().resolve(strict=False)
    if not source.is_file():
        raise ValueError(f"input RAW does not exist or is not a regular file: {source}")
    parent_receipt = normalized_parent_receipt(source, args.normalized_parent_manifest)
    output_root = pathlib.Path(args.output_root).expanduser().resolve(strict=False)
    if not output_root_is_external(output_root):
        raise ValueError("oracle output must be outside the Shadow source worktree")
    run_name = args.run_name or (
        f"{source.stem}-{dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%SZ')}"
    )
    if not SAFE_RUN_NAME.fullmatch(run_name):
        raise ValueError("run name must contain only letters, digits, dot, underscore, or hyphen")
    run_directory = output_root / run_name
    if run_directory.exists():
        raise ValueError(f"run directory already exists: {run_directory}")

    tools = selected_tools(args)
    selected = adapter_ids(args)
    required = set(args.require or ())
    unknown_required = required.difference(selected)
    if unknown_required:
        raise ValueError("required adapters must also be selected with --oracle")
    run_directory.mkdir(parents=True)

    manifest: dict[str, object] = {
        "schema": MANIFEST_SCHEMA,
        "orchestrator_version": ORCHESTRATOR_VERSION,
        "created_utc": utc_now(),
        "run_name": run_name,
        "source": {
            "basename": source.name,
            "size_bytes": source.stat().st_size,
            "sha256": sha256_file(source),
            "copied_into_run": False,
        },
        "normalized_parent": parent_receipt,
        "comparison_boundary": {
            "strict_same_decode": [
                adapter_id
                for adapter_id in selected
                if adapter_id in {"shadow-cfa-opposed", "shadow-threshold-ablation"}
            ],
            "decoder_evidence": [
                adapter_id
                for adapter_id in selected
                if adapter_id in {"shadow-rawframe", "libraw-identify"}
            ],
            "independent_container_pipelines": [
                adapter_id
                for adapter_id in selected
                if adapter_id
                not in {
                    "shadow-rawframe",
                    "shadow-normalized-dng",
                    "shadow-cfa-opposed",
                    "shadow-threshold-ablation",
                    "libraw-identify",
                }
            ],
            "normalized_interchange": [
                adapter_id for adapter_id in selected if adapter_id == "shadow-normalized-dng"
            ],
            "normalized_children": list(selected) if parent_receipt is not None else [],
            "normalized_interchange_status": (
                "verified-parent"
                if parent_receipt is not None
                else "implemented"
                if "shadow-normalized-dng" in selected
                else "not-selected"
            ),
        },
        "upstream_lock": load_upstream_lock(),
        "adapters": [],
    }
    records: list[dict[str, object]] = manifest["adapters"]  # type: ignore[assignment]
    plans: list[AdapterPlan | str] = []
    for adapter_id in selected:
        plan = build_plan(adapter_id, tools, source, run_directory)
        plans.append(plan)
        records.append(planned_record(adapter_id, plan, source, run_directory))
    write_manifest(run_directory / "manifest.json", manifest)

    if not args.dry_run:
        for record, plan in zip(records, plans, strict=True):
            if isinstance(plan, str):
                continue
            run_adapter(record, plan, source, run_directory, args.timeout_seconds)
            write_manifest(run_directory / "manifest.json", manifest)
    manifest["completed_utc"] = utc_now()
    manifest["dry_run"] = bool(args.dry_run)
    write_manifest(run_directory / "manifest.json", manifest)

    failures = {record["id"] for record in records if record["status"] == "failed"}
    unavailable = {record["id"] for record in records if record["status"] == "unavailable"}
    successes = {record["id"] for record in records if record["status"] == "succeeded"}
    print(f"oracle.run_directory={run_directory}")
    print(f"oracle.manifest={run_directory / 'manifest.json'}")
    print(f"oracle.succeeded={','.join(sorted(successes)) or 'none'}")
    print(f"oracle.failed={','.join(sorted(failures)) or 'none'}")
    print(f"oracle.unavailable={','.join(sorted(unavailable)) or 'none'}")

    if required.intersection(failures | unavailable):
        return 1
    if args.strict and (failures or unavailable):
        return 1
    if not args.dry_run and not successes:
        return 1
    return 0


def add_tool_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--shadow-probe")
    parser.add_argument("--shadow-decode-helper")
    parser.add_argument("--raw-identify")
    parser.add_argument("--dcraw-emu")
    parser.add_argument("--unprocessed-raw")
    parser.add_argument("--darktable-cli")
    parser.add_argument("--darktable-xmp")
    parser.add_argument("--rawtherapee-cli")
    parser.add_argument("--vkdt-cli")
    parser.add_argument(
        "--vkdt-icd",
        help="optional Vulkan ICD JSON recorded and passed as VK_ICD_FILENAMES",
    )
    parser.add_argument(
        "--oracle",
        action="append",
        choices=DEFAULT_ADAPTERS,
        help="select one adapter; repeat to select several (default: all)",
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Run isolated RAW decode and highlight reconstruction oracles"
    )
    subcommands = result.add_subparsers(dest="command", required=True)
    inventory_parser = subcommands.add_parser("inventory", help="show adapter availability")
    add_tool_arguments(inventory_parser)
    inventory_parser.set_defaults(handler=inventory)

    run_parser = subcommands.add_parser("run", help="create one immutable oracle run")
    add_tool_arguments(run_parser)
    run_parser.add_argument("--input", required=True)
    run_parser.add_argument("--output-root", required=True)
    run_parser.add_argument("--run-name")
    run_parser.add_argument(
        "--normalized-parent-manifest",
        help="verify and record the parent shadow-normalized-dng run manifest",
    )
    run_parser.add_argument("--require", action="append", choices=DEFAULT_ADAPTERS)
    run_parser.add_argument("--strict", action="store_true")
    run_parser.add_argument("--dry-run", action="store_true")
    run_parser.add_argument("--timeout-seconds", type=float, default=600.0)
    run_parser.set_defaults(handler=run)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    if getattr(arguments, "timeout_seconds", 1.0) <= 0.0:
        print("error: --timeout-seconds must be positive", file=sys.stderr)
        return 2
    try:
        return int(arguments.handler(arguments))
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
