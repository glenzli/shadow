"""Build a repeatable same-RawFrame Shadow/Darktable highlight-alignment receipt."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import pathlib
import tempfile
from typing import Iterable


REPORT_SCHEMA = "shadow.raw-highlight-strict-alignment.v1"
REPORT_VERSION = "20260826.1"
ORACLE_MANIFEST_SCHEMA = "shadow.raw-highlight-oracle-run.v1"
STRICT_ADAPTER_ID = "shadow-cfa-opposed"
STRICT_COMPARISON_CLASS = "same-decoded-cfa-reference"
CHANNELS = ("R", "G", "B")


def _parse_assignment(value: str) -> tuple[str, pathlib.Path]:
    identifier, separator, raw_path = value.partition("=")
    if not separator or not identifier or not raw_path:
        raise argparse.ArgumentTypeError("expected ID=/absolute/manifest.json")
    if not identifier.replace("-", "").replace("_", "").isalnum():
        raise argparse.ArgumentTypeError("ID may contain only letters, numbers, '-' and '_'")
    return identifier, pathlib.Path(raw_path).expanduser().resolve()


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _identity(path: pathlib.Path) -> dict[str, object]:
    if not path.is_file():
        raise ValueError(f"manifest does not exist: {path}")
    return {
        "basename": path.name,
        "size_bytes": path.stat().st_size,
        "sha256": _sha256(path),
    }


def _read_manifest(path: pathlib.Path) -> dict[str, object]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read oracle manifest {path.name}: {exc}") from exc
    if not isinstance(payload, dict) or payload.get("schema") != ORACLE_MANIFEST_SCHEMA:
        raise ValueError(f"unsupported oracle manifest schema in {path.name}")
    return payload


def _strict_adapter(manifest: dict[str, object], path: pathlib.Path) -> dict[str, object]:
    adapters = manifest.get("adapters")
    if not isinstance(adapters, list):
        raise ValueError(f"oracle manifest has no adapter list: {path.name}")
    matches = [adapter for adapter in adapters if isinstance(adapter, dict) and adapter.get("id") == STRICT_ADAPTER_ID]
    if len(matches) != 1:
        raise ValueError(f"oracle manifest must contain one {STRICT_ADAPTER_ID} adapter: {path.name}")
    adapter = matches[0]
    if adapter.get("status") != "succeeded":
        raise ValueError(f"strict adapter did not succeed: {path.name}")
    if adapter.get("comparison_class") != STRICT_COMPARISON_CLASS:
        raise ValueError(f"strict adapter comparison class changed: {path.name}")
    return adapter


def _evidence_value(evidence: dict[str, object], key: str) -> object:
    if key not in evidence:
        raise ValueError(f"strict adapter is missing evidence key: {key}")
    return evidence[key]


def _evidence_int(evidence: dict[str, object], key: str) -> int:
    value = _evidence_value(evidence, key)
    try:
        parsed = int(str(value))
    except ValueError as exc:
        raise ValueError(f"strict adapter evidence is not an integer: {key}={value}") from exc
    if parsed < 0:
        raise ValueError(f"strict adapter evidence must be non-negative: {key}={value}")
    return parsed


def _evidence_float(evidence: dict[str, object], key: str) -> float:
    value = _evidence_value(evidence, key)
    try:
        parsed = float(str(value))
    except ValueError as exc:
        raise ValueError(f"strict adapter evidence is not numeric: {key}={value}") from exc
    if not math.isfinite(parsed):
        raise ValueError(f"strict adapter evidence must be finite: {key}={value}")
    return parsed


def _relative_count_delta(candidate: int, reference: int) -> float | None:
    if reference == 0:
        return 0.0 if candidate == 0 else None
    return abs(candidate - reference) / reference


def _count_alignment(candidate: int, reference: int, relative_delta: float | None) -> str:
    if reference == 0 and candidate > 0:
        return "reference-zero-shadow-positive"
    assert relative_delta is not None
    if relative_delta == 0.0:
        return "exact-count"
    if relative_delta <= 0.02:
        return "close-count"
    return "divergent-count"


def analyze_fixture(identifier: str, manifest_path: pathlib.Path) -> dict[str, object]:
    manifest = _read_manifest(manifest_path)
    adapter = _strict_adapter(manifest, manifest_path)
    evidence = adapter.get("evidence")
    if not isinstance(evidence, dict):
        raise ValueError(f"strict adapter has no evidence map: {manifest_path.name}")

    channels: list[dict[str, object]] = []
    for channel in CHANNELS:
        reference_clipped = _evidence_int(
            evidence, f"highlight_reference.channel.{channel}.clipped_photosites"
        )
        reference_changed = _evidence_int(
            evidence, f"highlight_reference.channel.{channel}.changed_photosites"
        )
        shadow_candidates = _evidence_int(
            evidence, f"highlight_cfa_diagnostic.channel.{channel}.candidates"
        )
        shadow_raised = _evidence_int(
            evidence, f"highlight_cfa_diagnostic.channel.{channel}.raised"
        )
        if reference_changed > reference_clipped:
            raise ValueError(f"reference changed count exceeds clipped count for {channel}")
        if shadow_raised > shadow_candidates:
            raise ValueError(f"Shadow raised count exceeds candidate count for {channel}")
        candidate_delta = _relative_count_delta(shadow_candidates, reference_clipped)
        write_delta = _relative_count_delta(shadow_raised, reference_changed)
        channels.append(
            {
                "channel": channel,
                "reference": {
                    "effective_white_balance_gain": _evidence_float(
                        evidence,
                        f"highlight_reference.channel.{channel}.effective_white_balance_gain",
                    ),
                    "clip_value": _evidence_float(
                        evidence, f"highlight_reference.channel.{channel}.clip"
                    ),
                    "clipped_photosites": reference_clipped,
                    "changed_photosites": reference_changed,
                    "chrominance_offset": _evidence_float(
                        evidence,
                        f"highlight_reference.channel.{channel}.chrominance_offset",
                    ),
                },
                "shadow": {
                    "terminal_candidates": shadow_candidates,
                    "raised_photosites": shadow_raised,
                    "compiled_chrominance_offset": _evidence_float(
                        evidence,
                        f"highlight_cfa_diagnostic.channel.{channel}.compiled_chrominance_offset",
                    ),
                },
                "candidate_count_delta": shadow_candidates - reference_clipped,
                "candidate_count_relative_delta": candidate_delta,
                "candidate_count_alignment": _count_alignment(
                    shadow_candidates, reference_clipped, candidate_delta
                ),
                "write_count_delta": shadow_raised - reference_changed,
                "write_count_relative_delta": write_delta,
                "write_count_alignment": _count_alignment(
                    shadow_raised, reference_changed, write_delta
                ),
                "chrominance_offset_delta": _evidence_float(
                    evidence,
                    f"highlight_cfa_diagnostic.channel.{channel}.compiled_chrominance_offset",
                )
                - _evidence_float(
                    evidence, f"highlight_reference.channel.{channel}.chrominance_offset"
                ),
            }
        )

    reference_clipped_total = sum(
        int(channel["reference"]["clipped_photosites"]) for channel in channels
    )
    reference_changed_total = sum(
        int(channel["reference"]["changed_photosites"]) for channel in channels
    )
    declared_reference_clipped = _evidence_int(
        evidence, "highlight_reference.clipped_photosites"
    )
    declared_reference_changed = _evidence_int(
        evidence, "highlight_reference.changed_photosites"
    )
    if reference_clipped_total != declared_reference_clipped:
        raise ValueError("per-channel clipped counts do not sum to the declared reference total")
    if reference_changed_total != declared_reference_changed:
        raise ValueError("per-channel changed counts do not sum to the declared reference total")

    shadow_candidates_total = sum(
        int(channel["shadow"]["terminal_candidates"]) for channel in channels
    )
    shadow_raised_total = sum(
        int(channel["shadow"]["raised_photosites"]) for channel in channels
    )
    candidate_delta = _relative_count_delta(shadow_candidates_total, reference_clipped_total)
    write_delta = _relative_count_delta(shadow_raised_total, reference_changed_total)
    source = manifest.get("source")
    if not isinstance(source, dict):
        raise ValueError(f"oracle manifest has no source identity: {manifest_path.name}")
    return {
        "id": identifier,
        "manifest": _identity(manifest_path),
        "source": {
            "basename": source.get("basename"),
            "size_bytes": source.get("size_bytes"),
            "sha256": source.get("sha256"),
        },
        "orchestrator_version": manifest.get("orchestrator_version"),
        "probe": adapter.get("executable"),
        "channels": channels,
        "totals": {
            "reference_clipped_photosites": reference_clipped_total,
            "reference_changed_photosites": reference_changed_total,
            "shadow_terminal_candidates": shadow_candidates_total,
            "shadow_raised_photosites": shadow_raised_total,
            "candidate_count_relative_delta": candidate_delta,
            "candidate_count_alignment": _count_alignment(
                shadow_candidates_total, reference_clipped_total, candidate_delta
            ),
            "write_count_relative_delta": write_delta,
            "write_count_alignment": _count_alignment(
                shadow_raised_total, reference_changed_total, write_delta
            ),
        },
        "linear_output": {
            "dimensions": _evidence_value(evidence, "highlight_reference.dimensions"),
            "mean_absolute_difference": _evidence_float(
                evidence, "highlight_reference.mean_absolute_difference"
            ),
            "maximum_absolute_difference": _evidence_float(
                evidence, "highlight_reference.maximum_absolute_difference"
            ),
            "strongest_boundary_difference": _evidence_float(
                evidence, "highlight_cfa_diagnostic.domain.strongest_boundary_difference"
            ),
        },
        "thresholds": {
            "shadow": {
                "value": _evidence_float(evidence, "highlight_cfa_diagnostic.threshold"),
                "domain": _evidence_value(
                    evidence, "highlight_cfa_diagnostic.threshold_domain"
                ),
            },
            "reference": {
                "value": _evidence_float(evidence, "highlight_reference.threshold"),
                "domain": _evidence_value(evidence, "highlight_reference.threshold_domain"),
            },
            "cfa_white_levels": _evidence_value(
                evidence, "highlight_reference.cfa.white_levels"
            ),
            "has_linear_response_limits": _evidence_value(
                evidence, "highlight_reference.cfa.has_linear_response_limits"
            ),
            "cfa_linear_response_limits": _evidence_value(
                evidence, "highlight_reference.cfa.linear_response_limits"
            ),
        },
        "elapsed_ms": float(adapter.get("elapsed_ms", 0.0)),
        "interpretation_boundary": {
            "count_comparison_is_spatial_overlap": False,
            "same_rawframe": True,
            "shared_downstream": "shadow-area-sampling-and-camera-matrix",
            "quality_ranking": False,
        },
    }


def build_report(fixtures: Iterable[tuple[str, pathlib.Path]]) -> dict[str, object]:
    fixture_results = [analyze_fixture(identifier, path) for identifier, path in fixtures]
    if not fixture_results:
        raise ValueError("at least one strict oracle manifest is required")
    return {
        "schema": REPORT_SCHEMA,
        "version": REPORT_VERSION,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "fixtures": fixture_results,
        "aggregate": {
            "fixture_count": len(fixture_results),
            "maximum_candidate_count_relative_delta": max(
                float(fixture["totals"]["candidate_count_relative_delta"])
                for fixture in fixture_results
            ),
            "maximum_write_count_relative_delta": max(
                float(fixture["totals"]["write_count_relative_delta"])
                for fixture in fixture_results
            ),
            "maximum_linear_absolute_difference": max(
                float(fixture["linear_output"]["maximum_absolute_difference"])
                for fixture in fixture_results
            ),
        },
        "method": {
            "comparison": "Shadow production versus pinned Darktable-opposed adaptation",
            "input_boundary": "same provider-neutral RawFrame",
            "counts": "per-CFA-channel ownership counts; not a spatial intersection proof",
            "threshold_hypothesis": (
                "Shadow production selects response-shoulder candidates while the pinned "
                "Darktable adaptation selects its physical-white clip domain"
            ),
            "linear_output": "shared Shadow area sampling and camera matrix",
            "admission": "diagnostic evidence only",
        },
    }


def write_report(
    fixtures: Iterable[tuple[str, pathlib.Path]], output_root: pathlib.Path, run_name: str
) -> pathlib.Path:
    run_directory = output_root.expanduser().resolve() / run_name
    if run_directory.exists():
        raise ValueError(f"alignment run already exists: {run_directory}")
    run_directory.mkdir(parents=True)
    output = run_directory / "alignment.json"
    report = build_report(fixtures)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", dir=run_directory, delete=False
    ) as temporary:
        json.dump(report, temporary, indent=2, sort_keys=True)
        temporary.write("\n")
        temporary_path = pathlib.Path(temporary.name)
    temporary_path.replace(output)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare Shadow production and Darktable-opposed ownership on one RawFrame."
    )
    parser.add_argument(
        "--fixture",
        action="append",
        required=True,
        type=_parse_assignment,
        help="repeatable ID=/absolute/oracle-manifest.json",
    )
    parser.add_argument("--output-root", required=True, type=pathlib.Path)
    parser.add_argument("--run-name", required=True)
    arguments = parser.parse_args()
    try:
        output = write_report(arguments.fixture, arguments.output_root, arguments.run_name)
    except ValueError as exc:
        parser.error(str(exc))
    print(f"alignment.report={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
