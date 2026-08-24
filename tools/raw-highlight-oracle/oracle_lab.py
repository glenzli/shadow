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
ORCHESTRATOR_VERSION = "20260825.1"
SAFE_RUN_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$")

DEFAULT_ADAPTERS = (
    "shadow-rawframe",
    "shadow-cfa-opposed",
    "libraw-identify",
    "libraw-h0-clip",
    "libraw-h2-blend",
    "libraw-h3-rebuild",
    "rawtherapee-disabled",
    "rawtherapee-coloropp",
    "rawtherapee-color-propagation",
    "darktable-xmp",
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
    raw_identify: pathlib.Path | None
    dcraw_emu: pathlib.Path | None
    darktable_cli: pathlib.Path | None
    darktable_xmp: pathlib.Path | None
    rawtherapee_cli: pathlib.Path | None
    vkdt_cli: pathlib.Path | None


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
        "XDG_CACHE_HOME": str(directory / "cache"),
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
    if adapter_id == "darktable-xmp":
        if tools.darktable_cli is None:
            return "darktable-cli was not found; pass --darktable-cli"
        if tools.darktable_xmp is None:
            return "a controlled XMP is required; pass --darktable-xmp"
        output = directory / "result.tif"
        return AdapterPlan(
            adapter_id=adapter_id,
            stages=("container-decode", "raw-reconstruction", "demosaic", "colour"),
            comparison_class="independent-container-pipeline",
            executable=tools.darktable_cli,
            argv=(
                str(tools.darktable_cli),
                str(source),
                str(tools.darktable_xmp),
                str(output),
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
                str(directory / "library.db"),
                "--conf",
                "plugins/imageio/format/tiff/bpp=16",
            ),
            working_directory=directory,
            environment={},
            supporting_files=(tools.darktable_xmp,),
            notes=(
                "A caller-supplied XMP is mandatory so adjacent user sidecars cannot affect the run.",
            ),
        )
    if adapter_id == "vkdt-hilite":
        if tools.vkdt_cli is None:
            return "vkdt-cli was not found; pass --vkdt-cli"
        output = directory / "result"
        graph = TOOL_ROOT / "graphs" / "vkdt-hilite.cfg"
        return AdapterPlan(
            adapter_id=adapter_id,
            stages=("container-decode", "raw-denoise", "cfa-inpaint", "demosaic", "colour"),
            comparison_class="independent-container-pipeline",
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
            environment={},
            supporting_files=(graph,),
            notes=(
                "vkdt reconstructs highlights in raw mosaic space before demosaic.",
                "The graph intentionally omits display tone mapping and exports linear PFM.",
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
    for value in plan.environment.values():
        pathlib.Path(value).mkdir(parents=True, exist_ok=True)
    if plan.adapter_id == "darktable-xmp":
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
        raw_identify=resolve_executable(args.raw_identify, ("raw-identify",)),
        dcraw_emu=resolve_executable(args.dcraw_emu, ("dcraw_emu",)),
        darktable_cli=resolve_executable(args.darktable_cli, ("darktable-cli",)),
        darktable_xmp=resolve_optional_file(args.darktable_xmp),
        rawtherapee_cli=resolve_executable(args.rawtherapee_cli, ("rawtherapee-cli",)),
        vkdt_cli=resolve_executable(args.vkdt_cli, ("vkdt-cli",)),
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
        "comparison_boundary": {
            "strict_same_decode": [
                adapter_id for adapter_id in selected if adapter_id == "shadow-cfa-opposed"
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
                not in {"shadow-rawframe", "shadow-cfa-opposed", "libraw-identify"}
            ],
            "normalized_interchange_status": "not-implemented",
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
    parser.add_argument("--raw-identify")
    parser.add_argument("--dcraw-emu")
    parser.add_argument("--darktable-cli")
    parser.add_argument("--darktable-xmp")
    parser.add_argument("--rawtherapee-cli")
    parser.add_argument("--vkdt-cli")
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
