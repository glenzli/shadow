#!/usr/bin/env python3
"""Black-box real process pipes, synthetic pixels, no models or UI test hooks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import struct
import sys
import subprocess
import tempfile
import time
import zlib

SCHEMA = "shadow.edit-tools/1"


def png_fixture(path, width=640, height=426):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend((20 + x * 130 // width, 24 + y * 115 // height, 38 + (x + y) * 90 // (width + height)))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def jpeg_fixture(path, executable):
    subprocess.run([executable, str(path)], check=True, timeout=10)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Client:
    def __init__(self, executable, source, root, suffix):
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
                   RAYON_NUM_THREADS="2", OMP_NUM_THREADS="2")
        # The production tool route suppresses all existing test harnesses.
        self.log = (root / (suffix + ".stderr.log")).open("wb")
        self.process = subprocess.Popen([executable, "--agent-stdio", "--isolate", str(source)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, env=env, bufsize=0)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.buffer = b""
        self.responses = {}
        self.sequence = 0
        self.transcript = []

    def packet(self, op, params=None):
        self.sequence += 1
        return {"schema": SCHEMA, "id": str(self.sequence), "op": op, "params": params or {}}

    def send(self, *packets):
        data = b"".join(json.dumps(packet, separators=(",", ":")).encode() + b"\n" for packet in packets)
        self.process.stdin.write(data)

    def receive(self, request_id, timeout=60):
        deadline = time.monotonic() + timeout
        while request_id not in self.responses:
            assert time.monotonic() < deadline, "timed out waiting for formal tool response"
            ready = self.selector.select(min(0.2, max(0, deadline - time.monotonic())))
            if not ready:
                assert self.process.poll() is None, "tool exited before reply; inspect stderr log"
                continue
            data = os.read(self.process.stdout.fileno(), 65536)
            assert data, "unexpected protocol EOF"
            self.buffer += data
            assert len(self.buffer) <= 512 * 1024, "unbounded protocol output"
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                response = json.loads(line)
                assert response["schema"] == SCHEMA, "stdout must contain only protocol JSON"
                self.responses[response["id"]] = response
                self.transcript.append(response)
        return self.responses.pop(request_id)

    def call(self, op, params=None):
        packet = self.packet(op, params)
        self.send(packet)
        return self.receive(packet["id"])

    def snapshot(self):
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            response = self.call("snapshot")
            if response["ok"]:
                result = response["result"]
                if not any(result[key] for key in ("busy", "dirty", "autosavePending", "gestureActive")):
                    return result
            else:
                assert response["error"]["code"] in ("not_ready", "busy"), response
            time.sleep(0.05)
        raise AssertionError(f"editor never reached a clean snapshot: {response}")

    def close(self, graceful=True):
        if self.process.poll() is None:
            try:
                if graceful:
                    self.snapshot()
                    assert self.call("shutdown")["ok"]
                    assert self.process.wait(timeout=15) == 0
            finally:
                if self.process.poll() is None:
                    self.process.terminate()
                    self.process.wait(timeout=15)
        self.selector.close()
        self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()


def checked(response):
    assert response["ok"], response
    return response["result"]


def rejected(response, code):
    assert not response["ok"] and response["error"]["code"] == code, response


def artifact_check(record, expected_path, magic):
    assert record["path"] == str(expected_path)
    assert expected_path.read_bytes().startswith(magic)
    assert digest(expected_path) == record["sha256"]
    assert expected_path.stat().st_size == int(record["byteLength"])
    assert record["width"] > 0 and record["height"] > 0


def run(executable, fixture_executable, root):
    source = root / "synthetic.jpg"
    jpeg_fixture(source, fixture_executable)
    original_digest = digest(source)
    client = Client(executable, source, root, "first")
    receipt = None
    first_identity = None
    try:
        discovery = checked(client.call("discover"))
        assert discovery["scope"] == "one_explicit_isolated_photo"
        assert {item["type"] for item in discovery["edits"]} == {"set_exposure", "set_contrast", "set_saturation"}
        assert discovery["preview"]["maximumOperations"] == 16
        assert discovery["apply"]["cancel"] == discovery["export"]["cancel"] == "unsupported"
        first = client.snapshot()
        first_identity = first["identity"]
        assert first_identity["workingCommitId"] == "" and not first["canUndo"]
        node = next(node for node in first["nodes"] if node["editable"])
        before_exposure = node["exposureStops"]
        assert node["contrastFactor"] == node["saturationFactor"] == 1
        assert set(node["availableEdits"]) == {"set_exposure", "set_contrast", "set_saturation"}
        params = {"expected": first_identity, "operation": {"type": "set_exposure", "nodeId": node["nodeId"], "stops": 1.25},
                  "outputPath": str(root / "cancelled.jpg")}
        # Publication runs on a worker, so either cancellation can win or it can
        # arrive after publication. The held-owner test proves the former case.
        startup_busy_retries = 0
        startup_deadline = time.monotonic() + 15
        while True:
            preview = client.packet("preview", params)
            cancel = client.packet("cancel", {"requestId": preview["id"]})
            client.send(preview, cancel)
            cancellation = client.receive(cancel["id"])
            cancelled_preview = client.receive(preview["id"])
            if cancelled_preview["ok"] or cancelled_preview["error"]["code"] != "busy":
                break
            # The initial display can schedule another render after a clean
            # snapshot. Busy is an admission rejection, so reacquire identity
            # and retry with new correlation IDs without weakening the owner.
            rejected(cancellation, "not_running")
            assert not (root / "cancelled.jpg").exists()
            assert time.monotonic() < startup_deadline, "startup never admitted preview"
            fresh = client.snapshot()
            assert fresh["nodes"] == first["nodes"] and not fresh["canUndo"]
            first = fresh
            first_identity = fresh["identity"]
            params["expected"] = first_identity
            startup_busy_retries += 1
        if cancellation["ok"] and cancellation["result"]["accepted"]:
            rejected(cancelled_preview, "cancelled")
            assert not (root / "cancelled.jpg").exists()
        else:
            if not cancellation["ok"]:
                rejected(cancellation, "not_running")
            artifact_check(checked(cancelled_preview)["artifact"], root / "cancelled.jpg", b"\xff\xd8")
        rejected(client.call("export", {"expected": first_identity, "outputPath": str(root / "neutral.png")}), "invalid_operation")
        params["outputPath"] = str(root / "candidate.jpg")
        preview_result = checked(client.call("preview", params))
        assert preview_result["current"] and preview_result["proposalId"]
        artifact_check(preview_result["artifact"], root / "candidate.jpg", b"\xff\xd8")
        after_preview = client.snapshot()
        assert after_preview["identity"]["workingCommitId"] == ""
        assert after_preview["nodes"][0]["exposureStops"] == before_exposure and not after_preview["canUndo"]
        rejected(client.call("apply", {"expected": first_identity, "proposalId": preview_result["proposalId"]}), "stale_snapshot")
        identity = after_preview["identity"]
        invalid = dict(identity, activeVariantId="another-variant")
        rejected(client.call("preview", dict(params, expected=invalid)), "stale_snapshot")
        bad_op = dict(params, expected=identity, operation={"type": "set_exposure", "nodeId": node["nodeId"], "stops": 16.1})
        rejected(client.call("preview", bad_op), "invalid_request")
        rejected(client.call("preview", dict(params, expected=identity)), "invalid_operation")  # existing output
        composed = [
            {"type": "set_exposure", "nodeId": node["nodeId"], "stops": 0.75},
            {"type": "set_contrast", "nodeId": node["nodeId"], "factor": 1.15},
            {"type": "set_saturation", "nodeId": node["nodeId"], "factor": 0.8},
        ]
        params = {"expected": identity, "operations": composed, "outputPath": str(root / "apply-candidate.jpg")}
        invalid_node = dict(composed[-1], nodeId="missing-node")
        rejected(client.call("preview", dict(params, operations=[composed[0], invalid_node])), "invalid_operation")
        assert not (root / "apply-candidate.jpg").exists()
        unchanged = client.snapshot()
        assert unchanged["nodes"] == first["nodes"] and not unchanged["canUndo"]
        identity = unchanged["identity"]
        params["expected"] = identity
        noop = dict(composed[0], stops=before_exposure)
        rejected(client.call("preview", dict(params, operations=[noop])), "invalid_operation")
        proposal = checked(client.call("preview", params))
        assert proposal["current"] and len(proposal["changes"]) == 3
        assert [item["previousValue"] for item in proposal["changes"]] == [0, 1, 1]
        assert [item["value"] for item in proposal["changes"]] == [0.75, 1.15, 0.8]
        # A rejected replacement must neither partially edit nor erase the valid candidate.
        rejected(client.call("preview", dict(params, operations=[composed[0], invalid_node],
                                              outputPath=str(root / "invalid-replacement.jpg"))), "invalid_operation")
        assert not (root / "invalid-replacement.jpg").exists()
        apply = client.packet("apply", {"expected": identity, "proposalId": proposal["proposalId"]})
        cancel = client.packet("cancel", {"requestId": apply["id"]})
        busy = client.packet("snapshot")
        client.send(apply, cancel, busy)
        cancellation = client.receive(cancel["id"])
        assert not cancellation["ok"] and cancellation["error"]["code"] in ("cancel_unsupported", "not_running"), cancellation
        during_apply = client.receive(busy["id"])
        if not during_apply["ok"]:
            rejected(during_apply, "busy")
        receipt = checked(client.receive(apply["id"]))
        if during_apply["ok"]:
            assert during_apply["result"]["identity"]["workingCommitId"] == receipt["commitId"]
        assert receipt["undoSteps"] == 1 and len(receipt["snapshotDigest"]) == 64
        rejected(client.call("apply", {"expected": identity, "proposalId": proposal["proposalId"]}), "stale_snapshot")
        applied = client.snapshot()
        assert applied["identity"]["workingCommitId"] == receipt["commitId"]
        assert applied["identity"]["baseCommitId"] == receipt["commitId"]
        assert applied["identity"]["activeVariantId"] == receipt["activeVariantId"]
        assert applied["nodes"][0]["exposureStops"] == 0.75 and applied["canUndo"]
        assert applied["nodes"][0]["contrastFactor"] == 1.15
        assert applied["nodes"][0]["saturationFactor"] == 0.8
        expected = applied["identity"]
        rejected(client.call("export", {"expected": expected, "outputPath": str(source)}), "invalid_operation")
        link = root / "dangling.png"
        link.symlink_to(root / "absent.png")
        rejected(client.call("export", {"expected": expected, "outputPath": str(link)}), "invalid_operation")
        output = root / "edited.png"
        export = client.packet("export", {"expected": expected, "outputPath": str(output)})
        cancel = client.packet("cancel", {"requestId": export["id"]})
        client.send(export, cancel)
        cancellation = client.receive(cancel["id"])
        assert not cancellation["ok"] and cancellation["error"]["code"] in ("cancel_unsupported", "not_running"), cancellation
        exported = checked(client.receive(export["id"]))["artifact"]
        artifact_check(exported, output, b"\x89PNG\r\n\x1a\n")
        assert exported["commitId"] == receipt["commitId"]
        assert (exported["width"], exported["height"]) == (640, 426)
        output_digest = digest(output)
        rejected(client.call("export", {"expected": expected, "outputPath": str(output)}), "invalid_operation")
        assert digest(output) == output_digest and digest(source) == original_digest
        retry = root / "retry.png"
        artifact_check(checked(client.call("export", {"expected": expected, "outputPath": str(retry)}))["artifact"], retry, b"\x89PNG")
        assert digest(retry) == output_digest, "same immutable commit has reproducible output"
    finally:
        try:
            client.close(graceful=sys.exc_info()[0] is None)
        finally:
            (root / "first.protocol.json").write_text(json.dumps(client.transcript, indent=2) + "\n")
    restarted = Client(executable, source, root, "restart")
    try:
        fresh = restarted.snapshot()
        assert fresh["identity"]["sessionId"] != first_identity["sessionId"]
        assert fresh["identity"]["workingCommitId"] == "", "temporary Catalog lifetime must be explicit"
        rejected(restarted.call("apply", {"expected": first_identity, "proposalId": "old-process-proposal"}), "stale_snapshot")
        assert digest(source) == original_digest
    finally:
        try:
            restarted.close(graceful=sys.exc_info()[0] is None)
        finally:
            (root / "restart.protocol.json").write_text(json.dumps(restarted.transcript, indent=2) + "\n")
    for name in ("unsupported.png", "broken.jpg"):
        bad_source = root / name
        if bad_source.suffix == ".png":
            png_fixture(bad_source)
        else:
            bad_source.write_bytes(b"not a JPEG")
        bad = Client(executable, bad_source, root, name)
        try:
            deadline = time.monotonic() + 15
            while True:
                response = bad.call("snapshot")
                assert not response["ok"], response
                if response["error"]["code"] != "not_ready":
                    break
                assert time.monotonic() < deadline, "admission failure remained not_ready"
                time.sleep(0.05)
            rejected(response, "source_unavailable")
            assert response["error"]["message"]
            assert checked(bad.call("discover"))["schema"] == SCHEMA
            checked(bad.call("shutdown"))
            assert bad.process.wait(timeout=15) == 0
        finally:
            bad.close(graceful=False)
            (root / (name + ".protocol.json")).write_text(json.dumps(bad.transcript, indent=2))
    return {"schema": 1, "result": "passed", "sourceSha256": original_digest,
            "startupBusyRetries": startup_busy_retries,
            "commitReceipt": receipt, "exportSha256": output_digest, "artifactRoot": str(root),
            "evidence": ["real_stdio_process", "strict_scope", "preview_no_edit_or_undo", "preview_cancel_contract",
                         "stale_snapshot", "typed_range", "composed_atomic_adjustments", "invalid_replacement_preserves_candidate", "no_op_rejected", "exact_apply_receipt", "unsupported_cancel_truthful",
                         "pinned_png_export", "original_unchanged", "existing_and_symlink_not_replaced",
                         "retry_same_pixels", "restart_rejects_old_session", "unsupported_and_corrupt_source_reported"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    parser.add_argument("fixture_executable")
    parser.add_argument("--evidence-root", type=Path)
    args = parser.parse_args()
    root = args.evidence_root or Path(tempfile.mkdtemp(prefix="shadow-agent-stdio-"))
    root.mkdir(parents=True, exist_ok=True)
    result = run(str(Path(args.executable).resolve()), str(Path(args.fixture_executable).resolve()), root.resolve())
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, separators=(",", ":")))


if __name__ == "__main__":
    main()
