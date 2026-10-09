#!/usr/bin/env python3
"""Public external-stage subprocess contract; synthetic PNGs and no model calls."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import struct
import zlib
import time
import sys
import unicodedata

from edit_tool_stdio_integration import Client, artifact_check, checked, digest, png_fixture, rejected


def run(executable, root):
    root.mkdir(parents=True, exist_ok=True)
    measurements = {}

    def launch(name, wrong_digest=False, start_process=True, transparent=False, result_name="result.json"):
        folder = root / name
        folder.mkdir()
        source, output = folder / "input.png", folder / "edited.png"
        request, result = folder / "request.json", folder / result_name
        png_fixture(source)
        if transparent:
            def chunk(kind, data):
                return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
            source.write_bytes(b"\x89PNG\r\n\x1a\n"
                               + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
                               + chunk(b"IDAT", zlib.compress(bytes([0, 30, 40, 50, 128])))
                               + chunk(b"IEND", b""))
        binding = {"path": str(source), "revision": "shape-version-7", "sha256": digest(source)}
        if wrong_digest:
            binding["sha256"] = "0" * 64
        envelope = {"schema": "shadow.external-edit/1", "requestId": name,
                    "input": binding, "output": {"path": str(output)}}
        request.write_text(json.dumps(envelope))
        args = ["--pipeline-edit", "--request", str(request), "--result", str(result), "--agent-stdio"]
        launched_at = time.monotonic()
        client = Client(executable, source, folder, name, args) if start_process else None
        if client:
            client.stage_started_at = launched_at
        return client, envelope, output, result, args

    def exact_receipt_shape(receipt):
        # Shape uses deny_unknown_fields at all four levels. Optional stdio
        # observations must never leak new metadata into its terminal contract.
        completed = receipt["outcome"] == "completed"
        assert set(receipt) == {"schema", "requestId", "input", "outcome", "errors"} | ({"artifact", "render"} if completed else set())
        assert set(receipt["input"]) == {"path", "revision", "sha256"}
        if completed:
            assert set(receipt["artifact"]) == {"path", "sha256", "byteLength", "width", "height", "format", "colorSpace", "bitDepth"}
            assert set(receipt["render"]) == {"sessionId", "photoId", "representationId", "recipeCommitId", "recipeSnapshotDigest"}

    def terminal(client, envelope, output, result, code, outcome):
        assert client.process.wait(timeout=90) == code
        receipt = json.loads(result.read_text())
        exact_receipt_shape(receipt)
        assert receipt["schema"] == "shadow.external-edit-result/1"
        assert receipt["requestId"] == envelope["requestId"] and receipt["input"] == envelope["input"]
        assert receipt["outcome"] == outcome, receipt
        if outcome == "completed":
            artifact_check(receipt["artifact"], output, b"\x89PNG\r\n\x1a\n")
            assert receipt["artifact"]["width"] == 640 and receipt["artifact"]["height"] == 426
            assert receipt["artifact"]["colorSpace"] == "sRGB" and receipt["artifact"]["bitDepth"] == 8
            assert all(receipt["render"][key] for key in
                       ("sessionId", "photoId", "representationId", "recipeCommitId", "recipeSnapshotDigest"))
        else:
            assert "artifact" not in receipt and "render" not in receipt
        return receipt

    def ready_reply(client, operation, params):
        deadline = time.monotonic() + 20
        while True:
            response = client.call(operation, params)
            retryable = ("busy", "draft_pending") if operation == "shutdown" else ("busy",)
            if response["ok"] or response["error"]["code"] not in retryable:
                return response
            assert time.monotonic() < deadline, "initial display never released the owner"
            fresh = client.snapshot()
            if operation != "shutdown":
                params["expected"] = fresh["identity"]
            time.sleep(0.05)

    def ready_call(client, operation, params):
        return checked(ready_reply(client, operation, params))

    start = time.monotonic()
    client, envelope, output, result, args = launch("ai-complete")
    try:
        caps = checked(client.call("discover"))
        assert "complete" in caps["operations"]
        assert caps["externalStage"]["input"] == envelope["input"]
        assert caps["externalStage"]["exportCancellation"] == "unsupported"
        first = client.snapshot()
        measurements["cold_open_to_snapshot_ms"] = round((time.monotonic() - client.stage_started_at) * 1000, 2)
        current = ready_call(client, "render_current", {"expected": first["identity"], "outputPath": str(output.parent / "current.jpg")})
        assert set(current) == {"artifact", "current", "expected"} and current["current"]
        assert not output.exists() and not result.exists(), "observation impersonated a terminal return"
        after_current = client.snapshot()
        assert after_current["identity"] == current["expected"] and after_current["nodes"] == first["nodes"]
        assert not after_current["canUndo"] and not after_current["identity"]["workingCommitId"]
        first = after_current
        node = next(item for item in first["nodes"] if item["editable"])
        start = time.monotonic()
        proposal = ready_call(client, "preview", {"expected": first["identity"], "operations": [
            {"type": "set_exposure", "nodeId": node["nodeId"], "stops": 0.75},
            {"type": "set_contrast", "nodeId": node["nodeId"], "factor": 1.15}],
            "outputPath": str(output.parent / "preview.jpg")})
        measurements["composed_preview_with_admission_retries_ms"] = round((time.monotonic() - start) * 1000, 2)
        reviewed = client.snapshot()
        checked(client.call("apply", {"expected": reviewed["identity"], "proposalId": proposal["proposalId"]}))
        fresh = client.snapshot()
        rejected(client.call("complete", {"expected": first["identity"]}), "stale_snapshot")
        rejected(client.call("export", {"expected": fresh["identity"], "outputPath": str(output)}), "invalid_operation")
        start = time.monotonic()
        ready_call(client, "complete", {"expected": fresh["identity"]})
        receipt = terminal(client, envelope, output, result, 0, "completed")
        measurements["complete_through_exit_ms"] = round((time.monotonic() - start) * 1000, 2)
        assert receipt["render"]["recipeCommitId"] == fresh["identity"]["workingCommitId"]
        assert receipt["render"]["sessionId"] == fresh["identity"]["sessionId"]
        assert digest(Path(envelope["input"]["path"])) == envelope["input"]["sha256"]
        output_hash, result_hash = digest(output), digest(result)
        retry = subprocess.run([executable] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        assert retry.returncode != 0 and digest(output) == output_hash and digest(result) == result_hash
    finally:
        client.close(graceful=False)

    client, envelope, output, result, _ = launch("reserved-observation", result_name="caf\u00e9.jpg")
    try:
        reserved_paths = [result]
        if sys.platform == "darwin":
            reserved_paths.append(result.with_name(unicodedata.normalize("NFD", result.name)))
        for reserved_path in reserved_paths:
            state = client.snapshot()
            rejected(ready_reply(client, "render_current", {"expected": state["identity"], "outputPath": str(reserved_path)}), "invalid_operation")
            state = client.snapshot()
            node = next(item for item in state["nodes"] if item["editable"])
            rejected(ready_reply(client, "preview", {"expected": state["identity"], "operation": {"type": "set_exposure", "nodeId": node["nodeId"], "stops": 0.5}, "outputPath": str(reserved_path)}), "invalid_operation")
        assert not result.exists() and not output.exists()
        ready_call(client, "complete", {"expected": client.snapshot()["identity"]})
        terminal(client, envelope, output, result, 0, "completed")
    finally:
        client.close(graceful=False)

    for name in ("neutral", "cancel", "source-changed", "output-conflict", "result-conflict"):
        client, envelope, output, result, _ = launch(name)
        try:
            state = client.snapshot()
            original = Path(envelope["input"]["path"])
            if name == "source-changed":
                before = original.stat()
                data = bytearray(original.read_bytes())
                data[-1] ^= 1
                original.write_bytes(data)
                os.utime(original, ns=(before.st_atime_ns, before.st_mtime_ns))
                # The editor still owns the immutable copy even when source metadata is identical.
                assert client.snapshot()["identity"] == state["identity"]
            if name == "output-conflict":
                output.write_bytes(b"another output owner")
            if name == "result-conflict":
                result.write_bytes(b"another result owner")
            if name == "cancel":
                ready_call(client, "shutdown", {})
                terminal(client, envelope, output, result, 2, "cancelled")
                assert not output.exists()
            else:
                ready_call(client, "complete", {"expected": state["identity"]})
                if name == "result-conflict":
                    assert client.process.wait(timeout=90) == 1
                    assert result.read_bytes() == b"another result owner"
                else:
                    outcome = "completed" if name == "neutral" else "failed"
                    receipt = terminal(client, envelope, output, result, 0 if name == "neutral" else 1, outcome)
                    if name == "source-changed":
                        assert any("input_digest_mismatch" in error for error in receipt["errors"])
                        assert not output.exists()
                    if name == "output-conflict":
                        assert output.read_bytes() == b"another output owner"
            if name != "source-changed":
                assert digest(original) == envelope["input"]["sha256"]
        finally:
            client.close(graceful=False)

    client, envelope, output, result, _ = launch("wrong-digest", wrong_digest=True)
    try:
        receipt = terminal(client, envelope, output, result, 1, "failed")
        assert any("input_digest_mismatch" in error for error in receipt["errors"])
        assert not output.exists()
    finally:
        client.close(graceful=False)

    client, envelope, output, result, _ = launch("transparent", transparent=True)
    try:
        state = client.snapshot()
        ready_call(client, "complete", {"expected": state["identity"]})
        receipt = terminal(client, envelope, output, result, 1, "failed")
        assert any("Transparent PNG input is not supported" in error for error in receipt["errors"])
        assert not output.exists()
    finally:
        client.close(graceful=False)

    client, envelope, output, result, _ = launch("held-stdout")
    try:
        client.snapshot()
        time.sleep(0.2)
        for attempt in range(5):
            queued = [client.packet("discover") for _ in range(64)]
            closing = client.packet("shutdown")
            client.send(*queued, closing)
            deadline = time.monotonic() + 2
            while not result.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            if result.exists():
                assert client.process.poll() is None, "process exited before held stdout drain"
            for packet in queued:
                checked(client.receive(packet["id"]))
            response = client.receive(closing["id"])
            if response["ok"]:
                assert result.exists(), "terminal result did not precede held stdout drain"
                break
            rejected(response, "draft_pending")
            client.snapshot()
        else:
            raise AssertionError("clean shutdown never admitted after initial display")
        terminal(client, envelope, output, result, 2, "cancelled")
    finally:
        client.close(graceful=False)

    client, envelope, output, result, _ = launch("detach")
    try:
        client.snapshot()
        client.process.stdin.close()
        time.sleep(0.3)
        assert client.process.poll() is None and not result.exists(), "EOF closed the human session"
    finally:
        client.close(graceful=False)

    # The packaged QML and human controller share the same pipeline completion.
    client, envelope, output, result, args = launch("human-owner", start_process=False)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
               SHADOW_PIPELINE_SMOKE_ACTION="complete",
               SHADOW_PIPELINE_SMOKE_SCREENSHOT=str(output.parent / "return-stage.png"), RAYON_NUM_THREADS="2", OMP_NUM_THREADS="2")
    with (output.parent / "human.stderr.log").open("wb") as log:
        human = subprocess.run([executable] + args[:-1], stdout=log, stderr=log, env=env, timeout=120)
    assert human.returncode == 0
    receipt = json.loads(result.read_text())
    exact_receipt_shape(receipt)
    assert receipt["outcome"] == "completed" and receipt["input"] == envelope["input"]
    artifact_check(receipt["artifact"], output, b"\x89PNG\r\n\x1a\n")
    assert digest(Path(envelope["input"]["path"])) == envelope["input"]["sha256"]
    report = {"passed": ["current_observation_without_return", "reserved_jpeg_named_terminal_protected", "strict_four_level_result_unchanged", "composed-ai-return", "stale-complete-rejected", "reserved-return-path-rejected", "transparent-input-failed-result", "neutral-return", "cancelled-result",
                         "same-metadata-source-change", "output-conflict", "result-conflict", "wrong-digest",
                         "duplicate-launch-no-overwrite", "terminal-result-before-held-stdout-drain", "eof-detaches", "packaged-human-return-pointer"],
              "timings": measurements, "sample": "one synthetic 640x426 opaque RGB8 PNG; debug build; no model calls",
              "limits": "timings are smoke observations, not a comparative performance benchmark; EOF continuation only checks process lifetime"}
    if sys.platform == "darwin":
        report["passed"].append("macos_unicode_alias_terminal_protected")
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    parser.add_argument("--output", type=Path)
    options = parser.parse_args()
    if options.output:
        run(options.executable, options.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="shadow-external-stage-") as temporary:
            run(options.executable, Path(temporary).resolve())
