"""Admission, audit, and candidate evaluation for local RAW highlight fixtures."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import pathlib
import sys
from typing import Iterable


DEFINITION_SCHEMA = "shadow.raw-highlight-fixture-definition.v1"
MATRIX_SCHEMA = "shadow.raw-highlight-fixture-matrix.v1"
EVALUATION_SCHEMA = "shadow.raw-highlight-candidate-evaluation.v1"
REGION_SCHEMA = "shadow.raw-highlight-region.v1"
REQUIRED_CLASSES = frozenset(
    {
        "nikon-he-smooth-clipping",
        "lamp-dark-neighbour",
        "sony-sun-disc-gradient",
        "specular-highlight",
        "saturated-single-colour-emitter",
        "ordinary-unclipped-control",
    }
)
CANDIDATE_FIELDS = (
    "write_ownership",
    "context_radius",
    "reusable_upstream_state",
    "recomputation_frontier",
    "residency_and_transfers",
    "cache_identity_impact",
    "cancellation_behavior",
    "preview_detail_export_equivalence",
)


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _external_output(path: pathlib.Path) -> pathlib.Path:
    resolved = path.expanduser().resolve()
    repository_root = pathlib.Path(__file__).resolve().parents[2]
    try:
        resolved.relative_to(repository_root)
    except ValueError:
        return resolved
    raise ValueError("fixture and evaluation outputs must remain outside the Shadow repository")


def _write_new_json(path: pathlib.Path, document: dict[str, object]) -> None:
    if path.exists():
        raise ValueError(f"refusing to overwrite existing output: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def _validate_rectangle(rectangle: Iterable[float], name: str) -> None:
    values = tuple(float(value) for value in rectangle)
    if len(values) != 4:
        raise ValueError(f"{name} must contain x, y, width, height")
    x, y, width, height = values
    if x < 0.0 or y < 0.0 or width <= 0.0 or height <= 0.0 or x + width > 1.0 or y + height > 1.0:
        raise ValueError(f"{name} must be a positive rectangle within [0, 1]")


def _validate_region(region: dict[str, object], case_id: str) -> None:
    if region.get("schema") != REGION_SCHEMA:
        raise ValueError(f"case {case_id} must use {REGION_SCHEMA}")
    if region.get("id") != case_id:
        raise ValueError(f"case {case_id} region id must match the case id")
    _validate_rectangle(region.get("crop_normalized", ()), f"case {case_id} crop")
    core = region.get("clipped_core_rectangles")
    if not isinstance(core, list) or not core:
        raise ValueError(f"case {case_id} requires clipped_core_rectangles")
    for rectangle in core:
        _validate_rectangle(rectangle, f"case {case_id} clipped core")
    for rectangle in region.get("reliable_exterior_rectangles", []):
        _validate_rectangle(rectangle, f"case {case_id} reliable exterior")
    radius = int(region.get("boundary_radius_pixels", 8))
    if radius < 1 or radius > 128:
        raise ValueError(f"case {case_id} boundary radius must be between 1 and 128")


def _validate_candidate(candidate: dict[str, object]) -> None:
    identifier = candidate.get("id")
    if not isinstance(identifier, str) or not identifier:
        raise ValueError("candidate id must be a non-empty string")
    for field in CANDIDATE_FIELDS:
        value = candidate.get(field)
        if value is None or value == "" or value == [] or value == {}:
            raise ValueError(f"candidate {identifier} must declare {field}")
    radius = candidate["context_radius"]
    if not isinstance(radius, dict) or radius.get("unit") not in {"pixels", "tiles", "full-frame"}:
        raise ValueError(f"candidate {identifier} has an invalid context_radius")
    if radius.get("unit") != "full-frame" and int(radius.get("value", -1)) < 0:
        raise ValueError(f"candidate {identifier} context radius must be non-negative")


def _validate_case(case: dict[str, object]) -> None:
    identifier = case.get("id")
    if not isinstance(identifier, str) or not identifier:
        raise ValueError("case id must be a non-empty string")
    case_class = case.get("class")
    if case_class not in REQUIRED_CLASSES:
        raise ValueError(f"case {identifier} has unknown class {case_class!r}")
    region = case.get("region")
    if not isinstance(region, dict):
        raise ValueError(f"case {identifier} has no region")
    _validate_region(region, identifier)


def build_matrix(args: argparse.Namespace) -> int:
    definition_path = args.definition.expanduser().resolve()
    output_path = _external_output(args.output)
    definition = json.loads(definition_path.read_text(encoding="utf-8"))
    if definition.get("schema") != DEFINITION_SCHEMA:
        raise ValueError(f"definition must use {DEFINITION_SCHEMA}")
    fixtures: list[dict[str, object]] = []
    seen_fixture_ids: set[str] = set()
    seen_case_ids: set[str] = set()
    for fixture in definition.get("fixtures", []):
        identifier = fixture.get("id")
        if not isinstance(identifier, str) or not identifier or identifier in seen_fixture_ids:
            raise ValueError("fixture ids must be non-empty and unique")
        seen_fixture_ids.add(identifier)
        source = pathlib.Path(str(fixture.get("source_path", ""))).expanduser().resolve()
        if not source.is_file():
            raise ValueError(f"fixture source does not exist: {source}")
        license_name = fixture.get("license")
        if not isinstance(license_name, str) or not license_name:
            raise ValueError(f"fixture {identifier} must declare its local license/provenance")
        cases = fixture.get("cases")
        if not isinstance(cases, list) or not cases:
            raise ValueError(f"fixture {identifier} must contain at least one case")
        admitted_cases: list[dict[str, object]] = []
        for case in cases:
            _validate_case(case)
            case_id = str(case["id"])
            if case_id in seen_case_ids:
                raise ValueError(f"duplicate case id: {case_id}")
            seen_case_ids.add(case_id)
            admitted_cases.append(case)
        fixtures.append(
            {
                "id": identifier,
                "source": {
                    "absolute_path": str(source),
                    "basename": source.name,
                    "size_bytes": source.stat().st_size,
                    "sha256": sha256_file(source),
                    "license": license_name,
                    "copied_into_catalog": False,
                },
                "cases": admitted_cases,
            }
        )
    candidates = definition.get("candidates", [])
    candidate_ids: set[str] = set()
    for candidate in candidates:
        _validate_candidate(candidate)
        if candidate["id"] in candidate_ids:
            raise ValueError(f"duplicate candidate id: {candidate['id']}")
        candidate_ids.add(str(candidate["id"]))
    document = {
        "schema": MATRIX_SCHEMA,
        "matrix_version": args.matrix_version,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
        "definition": {
            "basename": definition_path.name,
            "size_bytes": definition_path.stat().st_size,
            "sha256": sha256_file(definition_path),
        },
        "payload_policy": {
            "raw_files_copied": False,
            "catalog_is_local_only": True,
            "coordination_payload_forbidden": True,
        },
        "required_classes": sorted(REQUIRED_CLASSES),
        "fixtures": fixtures,
        "candidates": candidates,
    }
    audit_document(document, verify_sources=True, require_coverage=True)
    _write_new_json(output_path, document)
    print(f"fixture.matrix={output_path}")
    print(f"fixture.sources={len(fixtures)}")
    print(f"fixture.cases={len(seen_case_ids)}")
    return 0


def audit_document(
    document: dict[str, object], verify_sources: bool, require_coverage: bool
) -> dict[str, object]:
    if document.get("schema") != MATRIX_SCHEMA:
        raise ValueError(f"matrix must use {MATRIX_SCHEMA}")
    fixture_ids: set[str] = set()
    case_ids: set[str] = set()
    classes: set[str] = set()
    for fixture in document.get("fixtures", []):
        fixture_id = fixture.get("id")
        if not isinstance(fixture_id, str) or not fixture_id or fixture_id in fixture_ids:
            raise ValueError("matrix fixture ids must be non-empty and unique")
        fixture_ids.add(fixture_id)
        source = fixture.get("source")
        if not isinstance(source, dict) or source.get("copied_into_catalog") is not False:
            raise ValueError(f"fixture {fixture_id} violates the no-copy policy")
        source_path = pathlib.Path(str(source.get("absolute_path", "")))
        if verify_sources:
            if not source_path.is_file():
                raise ValueError(f"fixture source is missing: {source_path}")
            if source_path.stat().st_size != source.get("size_bytes") or sha256_file(source_path) != source.get("sha256"):
                raise ValueError(f"fixture source identity changed: {source_path}")
        for case in fixture.get("cases", []):
            _validate_case(case)
            case_id = str(case["id"])
            if case_id in case_ids:
                raise ValueError(f"duplicate matrix case id: {case_id}")
            case_ids.add(case_id)
            classes.add(str(case["class"]))
    candidates = document.get("candidates", [])
    candidate_ids: set[str] = set()
    for candidate in candidates:
        _validate_candidate(candidate)
        if candidate["id"] in candidate_ids:
            raise ValueError(f"duplicate matrix candidate id: {candidate['id']}")
        candidate_ids.add(str(candidate["id"]))
    missing = sorted(REQUIRED_CLASSES - classes)
    if require_coverage and missing:
        raise ValueError(f"fixture matrix is missing required classes: {', '.join(missing)}")
    return {
        "fixture_count": len(fixture_ids),
        "case_count": len(case_ids),
        "candidate_count": len(candidate_ids),
        "covered_classes": sorted(classes),
        "missing_classes": missing,
        "source_identity_verified": verify_sources,
    }


def audit_matrix(args: argparse.Namespace) -> int:
    path = args.catalog.expanduser().resolve()
    document = json.loads(path.read_text(encoding="utf-8"))
    receipt = audit_document(
        document, verify_sources=not args.skip_source_hash, require_coverage=not args.allow_partial
    )
    print(json.dumps(receipt, indent=2, sort_keys=True))
    return 0


def export_regions(args: argparse.Namespace) -> int:
    catalog_path = args.catalog.expanduser().resolve()
    output_directory = _external_output(args.output_directory)
    if output_directory.exists():
        raise ValueError(f"refusing to reuse region output directory: {output_directory}")
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    audit_document(catalog, verify_sources=not args.skip_source_hash, require_coverage=True)
    output_directory.mkdir(parents=True)
    artifacts: list[dict[str, object]] = []
    for case_id, case in sorted(_matrix_cases(catalog).items()):
        path = output_directory / f"{case_id}.json"
        path.write_text(
            json.dumps(case["region"], indent=2, sort_keys=True, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        artifacts.append(
            {
                "case_id": case_id,
                "class": case["class"],
                "path": path.name,
                "size_bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    receipt = {
        "schema": "shadow.raw-highlight-region-export.v1",
        "fixture_matrix_sha256": sha256_file(catalog_path),
        "regions": artifacts,
    }
    (output_directory / "receipt.json").write_text(
        json.dumps(receipt, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"fixture.region_directory={output_directory}")
    print(f"fixture.regions={len(artifacts)}")
    return 0


def _parse_analysis(value: str) -> tuple[str, pathlib.Path]:
    identifier, separator, raw_path = value.partition("=")
    if not separator or not identifier or not raw_path:
        raise argparse.ArgumentTypeError("expected CASE_ID=/absolute/analysis.json")
    return identifier, pathlib.Path(raw_path).expanduser().resolve()


def _matrix_cases(document: dict[str, object]) -> dict[str, dict[str, object]]:
    return {
        str(case["id"]): case
        for fixture in document.get("fixtures", [])
        for case in fixture.get("cases", [])
    }


def evaluate_candidate(args: argparse.Namespace) -> int:
    catalog_path = args.catalog.expanduser().resolve()
    output_path = _external_output(args.output)
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    audit_document(catalog, verify_sources=not args.skip_source_hash, require_coverage=True)
    declarations = {
        str(candidate["id"]): candidate for candidate in catalog.get("candidates", [])
    }
    if args.candidate_id not in declarations:
        raise ValueError(f"candidate {args.candidate_id!r} is not declared in the matrix")
    cases = _matrix_cases(catalog)
    supplied = dict(args.analysis)
    missing = sorted(set(cases) - set(supplied))
    extra = sorted(set(supplied) - set(cases))
    if missing or extra:
        raise ValueError(f"analysis coverage mismatch missing={missing} extra={extra}")
    results: list[dict[str, object]] = []
    for case_id, case in sorted(cases.items()):
        analysis_path = supplied[case_id]
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
        if analysis.get("schema") != "shadow.raw-highlight-objective-analysis.v1":
            raise ValueError(f"analysis for {case_id} has the wrong schema")
        if analysis.get("region", {}).get("id") != case_id:
            raise ValueError(f"analysis region does not match case {case_id}")
        candidate_matches = [
            candidate
            for candidate in analysis.get("candidates", [])
            if candidate.get("id") == args.candidate_id
        ]
        if len(candidate_matches) != 1:
            raise ValueError(
                f"analysis for {case_id} must contain candidate {args.candidate_id} exactly once"
            )
        candidate = candidate_matches[0]
        boundary = candidate["metrics"]["boundary_continuity"]
        false_colour = candidate["metrics"]["false_colour_exterior"]
        results.append(
            {
                "case_id": case_id,
                "class": case["class"],
                "analysis": {
                    "basename": analysis_path.name,
                    "size_bytes": analysis_path.stat().st_size,
                    "sha256": sha256_file(analysis_path),
                },
                "boundary_continuity": boundary,
                "false_colour_exterior": false_colour,
                "resource_usage": analysis.get("resource_usage"),
                "oracle_run": candidate.get("oracle_run"),
            }
        )
    document = {
        "schema": EVALUATION_SCHEMA,
        "evaluation_version": "20260825.1",
        "candidate": declarations[args.candidate_id],
        "fixture_matrix": {
            "basename": catalog_path.name,
            "size_bytes": catalog_path.stat().st_size,
            "sha256": sha256_file(catalog_path),
            "matrix_version": catalog.get("matrix_version"),
        },
        "aggregation_policy": {
            "whole_image_average": False,
            "per_case_results_preserved": True,
            "automatic_production_admission": False,
        },
        "results": results,
    }
    _write_new_json(output_path, document)
    print(f"fixture.evaluation={output_path}")
    print(f"fixture.evaluated_cases={len(results)}")
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build", help="admit a local definition into a hashed matrix")
    build.add_argument("--definition", type=pathlib.Path, required=True)
    build.add_argument("--output", type=pathlib.Path, required=True)
    build.add_argument("--matrix-version", required=True)
    build.set_defaults(handler=build_matrix)
    audit = commands.add_parser("audit", help="verify identities, regions, and required coverage")
    audit.add_argument("--catalog", type=pathlib.Path, required=True)
    audit.add_argument("--skip-source-hash", action="store_true")
    audit.add_argument("--allow-partial", action="store_true")
    audit.set_defaults(handler=audit_matrix)
    export = commands.add_parser(
        "export-regions", help="materialize immutable Phase 3 region specs from a matrix"
    )
    export.add_argument("--catalog", type=pathlib.Path, required=True)
    export.add_argument("--output-directory", type=pathlib.Path, required=True)
    export.add_argument("--skip-source-hash", action="store_true")
    export.set_defaults(handler=export_regions)
    evaluate = commands.add_parser(
        "evaluate", help="bind complete per-case objective analyses to a declared candidate"
    )
    evaluate.add_argument("--catalog", type=pathlib.Path, required=True)
    evaluate.add_argument("--candidate-id", required=True)
    evaluate.add_argument(
        "--analysis", type=_parse_analysis, action="append", required=True, metavar="CASE=PATH"
    )
    evaluate.add_argument("--output", type=pathlib.Path, required=True)
    evaluate.add_argument("--skip-source-hash", action="store_true")
    evaluate.set_defaults(handler=evaluate_candidate)
    return root


def main(argv: list[str] | None = None) -> int:
    try:
        arguments = parser().parse_args(argv)
        return int(arguments.handler(arguments))
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
