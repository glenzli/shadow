#!/usr/bin/env python3
"""Run/compare one fixed native import workload in fresh, private storage profiles."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import sqlite3
import subprocess
import time
import uuid


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def corpus_identity(root):
    # Hash each physical file once, including a hard-linked diagnostic corpus.
    physical = {}
    manifest = []
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        st = path.stat()
        key = (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns)
        if key not in physical:
            physical[key] = digest(path)
        manifest.append([str(path.relative_to(root)), st.st_size, st.st_mtime_ns, physical[key]])
    encoded = json.dumps(manifest, separators=(",", ":")).encode()
    return {"sha256": hashlib.sha256(encoded).hexdigest(), "files": len(manifest)}


def bundle_identity(app):
    # The RAW helper and other bundled executables participate in the measured
    # pipeline even when the GUI binary is unchanged.
    manifest = [[str(p.relative_to(app)), digest(p)]
                for p in sorted(app.rglob("*")) if p.is_file()]
    return hashlib.sha256(json.dumps(manifest, separators=(",", ":")).encode()).hexdigest()


def catalog_visual_state(database):
    with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as db:
        proxies = db.execute("SELECT * FROM representation_cached_artifacts WHERE role = 'generated_proxy' "
                             "ORDER BY representation_id, variant_key").fetchall()
        observations = db.execute("SELECT COUNT(*) FROM representation_technical_observations").fetchone()[0]
        integrity = db.execute("PRAGMA quick_check").fetchone()[0]
    serializable = [[v.hex() if isinstance(v, bytes) else v for v in row] for row in proxies]
    return {"proxies": len(proxies), "observations": observations, "integrity": integrity,
            "proxy_records_sha256": hashlib.sha256(json.dumps(serializable, separators=(",", ":")).encode()).hexdigest()}


def prepare_backfill_profile(seed, profile):
    """Only a fresh private profile loses analysis; source Catalog and pixels stay read-only."""
    with sqlite3.connect(f"file:{seed / 'catalog.sqlite'}?mode=ro", uri=True) as src:
        with sqlite3.connect(profile / "catalog.sqlite") as dst:
            src.backup(dst)
            dst.execute("DELETE FROM representation_technical_observations")
            dst.commit()
    shutil.copytree(seed / "cache/blobs", profile / "cache/blobs")
    return catalog_visual_state(profile / "catalog.sqlite")


def isolated_stage_counts(profile):
    # Fresh runtime evidence: count per-source routes, not process CPU time.
    counts = {}
    for path in (profile / "cache/decode-helper/safety").glob("*.json"):
        record = json.loads(path.read_text())
        operation = record["key"]["operation"]
        counts[operation] = counts.get(operation, 0) + 1
    return counts


def run(args):
    source = args.source.resolve(strict=True)
    app = args.app.resolve(strict=True)
    binary = app / "Contents/MacOS/Shadow"
    output = args.output.resolve()
    if output == source or output.is_relative_to(source) or source.is_relative_to(output):
        raise ValueError("benchmark output and read-only source must be separate trees")
    identity = corpus_identity(source)
    output.mkdir(parents=True, exist_ok=False)
    profile = output / "profile"
    profile.mkdir()
    seed_identity = None
    backfill_before = None
    if args.seed_catalog:
        seed = args.seed_catalog.resolve(strict=True)
        with sqlite3.connect(f"file:{seed}?mode=ro", uri=True) as src:
            with sqlite3.connect(profile / "catalog.sqlite") as dst:
                src.backup(dst)
                # Revalidation benchmark: preserve the same catalog identities/order,
                # and require both candidates to regenerate every visual from scratch.
                dst.execute("DELETE FROM representation_cached_artifacts")
                dst.execute("DELETE FROM representation_decode_snapshots")
                dst.commit()
        seed_identity = digest(profile / "catalog.sqlite")
    if args.backfill_profile:
        backfill_before = prepare_backfill_profile(args.backfill_profile.resolve(strict=True), profile)
        if backfill_before["integrity"] != "ok" or backfill_before["proxies"] != identity["files"]:
            raise ValueError("backfill seed must contain one intact generated proxy per input photo")
        seed_identity = digest(profile / "catalog.sqlite")
    (profile / "storage-profile.json").write_text(json.dumps({
        "id": str(uuid.uuid4()), "kind": "custom", "schemaVersion": 1,
    }))
    env = {k: v for k, v in os.environ.items() if not k.startswith("SHADOW_DESKTOP_")}
    env.update(
        SHADOW_PROFILE_REGISTRY_ROOT=str(output / "registry"),
        SHADOW_DESKTOP_SMOKE_TEST="1", SHADOW_REVIEW_DIAGNOSTICS="1",
        SHADOW_DESKTOP_BULK_IMPORT_SMOKE="1", SHADOW_DESKTOP_BULK_FIXED_WORKLOAD="1",
        SHADOW_DESKTOP_SCAN_FOLDER=str(source),
        SHADOW_DESKTOP_BULK_EXPECTED_PHOTOS=str(identity["files"]),
        SHADOW_INFER_BASE_URL="http://127.0.0.1:9",
    )
    if args.fifo:
        env["SHADOW_DESKTOP_BULK_FIFO_BASELINE"] = "1"
    binary_hash = digest(binary)
    bundle_hash = bundle_identity(app)
    helper = app / "Contents/MacOS/shadow-image-decode-helper"
    helper_identity = {"path": str(helper), "sha256": digest(helper)}
    logpath = output / "application.log"
    peak_rss = 0
    forced = False
    started = time.monotonic()
    with logpath.open("w") as log:
        proc = subprocess.Popen(
            [str(binary), "--profile-root", str(profile), "--profile-launch-context", "debug"],
            env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        try:
            while proc.poll() is None and time.monotonic() - started < args.timeout:
                sample = subprocess.run(
                    ["ps", "-p", str(proc.pid), "-o", "rss="], capture_output=True, text=True,
                    check=False,
                ).stdout.strip()
                if sample:
                    peak_rss = max(peak_rss, int(sample) * 1024)
                time.sleep(0.25)
        finally:
            if proc.poll() is None:
                forced = True
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
    marker = "Bulk import acceptance "
    reports = [json.loads(line.split(marker, 1)[1]) for line in logpath.read_text().splitlines() if marker in line]
    result = {
        "schema": 2, "policy": "fifo" if args.fifo else "visible", "corpus": identity, "seed_sha256": seed_identity,
        "machine": [platform.system(), platform.release(), platform.machine(), os.cpu_count(),
                    hashlib.sha256(platform.node().encode()).hexdigest()],
        "binary_sha256": binary_hash, "bundle_sha256": bundle_hash,
        "helper_identity": helper_identity,
        "artifact_unchanged": bundle_hash == bundle_identity(app),
        "corpus_unchanged": identity == corpus_identity(source),
        "elapsed_s": time.monotonic() - started, "peak_gui_rss_bytes": peak_rss,
        "exit_code": proc.returncode, "forced_stop": forced,
        "report": reports[-1] if reports else None,
        "scenario": "analysis-backfill" if args.backfill_profile else "cold-proxies",
        "backfill_before": backfill_before,
        "backfill_after": catalog_visual_state(profile / "catalog.sqlite") if args.backfill_profile else None,
        "isolated_stage_counts": isolated_stage_counts(profile),
    }
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
    return 0 if valid(result) else 1


def valid(result):
    if result.get("schema") != 2:
        return False
    report = result.get("report") or {}
    if result.get("scenario") == "analysis-backfill":
        before, after = result.get("backfill_before") or {}, result.get("backfill_after") or {}
        if (before.get("observations") != 0 or after.get("integrity") != "ok"
                or after.get("proxies") != result["corpus"]["files"]
                or after.get("observations") != result["corpus"]["files"]):
            return False
    return (result["exit_code"] == 0 and not result["forced_stop"]
            and result["artifact_unchanged"] and result["corpus_unchanged"] and report.get("success")
            and report.get("workload") == "four-pages-four-targets-v1"
            and report.get("pages_requested") == 3 and report.get("loaded_rows") == 384
            and [step["source_row"] for step in report.get("steps", [])] == [288, 192, 96, 0])


def compare(before, after):
    if not valid(before) or not valid(after):
        raise ValueError("both runs must complete the fixed workload and normal exit")
    for field in ("corpus", "seed_sha256", "machine"):
        if before[field] != after[field]:
            raise ValueError(f"incomparable {field}")
    scenario = before.get("scenario", "cold-proxies")
    if scenario != after.get("scenario", "cold-proxies"):
        raise ValueError("incomparable scenario")
    if scenario == "analysis-backfill":
        if before["policy"] != after["policy"]:
            raise ValueError("analysis-backfill comparison must use the same scheduling policy")
        if before["backfill_before"] != after["backfill_before"]:
            raise ValueError("analysis-backfill comparison must start from identical proxy records")
        if not before.get("helper_identity") or before["helper_identity"] != after.get("helper_identity"):
            raise ValueError("analysis-backfill comparison must retain the same isolated decoder")
    else:
        if (before["binary_sha256"] != after["binary_sha256"]
                or before["bundle_sha256"] != after["bundle_sha256"]):
            raise ValueError("FIFO and visible policies must use the same linked app")
        if before["policy"] != "fifo" or after["policy"] != "visible":
            raise ValueError("expected FIFO baseline and visible-priority candidate")
    a, b = before["report"], after["report"]
    for field in ("total_photos", "loaded_rows", "pages_requested", "window_size", "device_pixel_ratio"):
        if a.get(field) != b.get(field):
            raise ValueError(f"incomparable {field}")
    if [s["title"] for s in a["steps"]] != [s["title"] for s in b["steps"]]:
        raise ValueError("the workload selected different photos")
    if [s["source"] for s in a["steps"]] != [s["source"] for s in b["steps"]]:
        raise ValueError("the workload selected different source files")
    metrics = {}
    def add(name, old, new):
        metrics[name] = {"baseline": old, "candidate": new,
                         "change_percent": 100 * (new / old - 1) if old else None}
    for field in ("complete_ms", "first_thumbnail_ready_ms", "event_loop_gap_p95_upper_ms"):
        add(field, a[field], b[field])
    add("peak_gui_rss_bytes", before["peak_gui_rss_bytes"], after["peak_gui_rss_bytes"])
    for i, (old, new) in enumerate(zip(a["steps"], b["steps"])):
        add(f"step_{i}_proxy_ready_ms", old["proxy_ready_ms"], new["proxy_ready_ms"])
    return {"comparable": True, "metrics": metrics,
            "note": "Single-run observations; event-loop p95 is a histogram upper bound, not an exact percentile."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    run_parser = sub.add_parser("run")
    for name in ("app", "source", "output"):
        run_parser.add_argument(f"--{name}", type=Path, required=True)
    seed_options = run_parser.add_mutually_exclusive_group()
    seed_options.add_argument("--seed-catalog", type=Path)
    seed_options.add_argument("--backfill-profile", type=Path,
                              help="Copy Catalog/proxy blobs into a fresh profile and regenerate analysis only")
    run_parser.add_argument("--fifo", action="store_true", help="Use FIFO baseline in the same app")
    run_parser.add_argument("--timeout", type=int, default=900)
    comparison = sub.add_parser("compare")
    comparison.add_argument("baseline", type=Path)
    comparison.add_argument("candidate", type=Path)
    args = parser.parse_args()
    if args.command == "run":
        return run(args)
    print(json.dumps(compare(json.loads(args.baseline.read_text()), json.loads(args.candidate.read_text())), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
