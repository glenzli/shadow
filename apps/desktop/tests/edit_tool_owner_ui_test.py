#!/usr/bin/env python3
"""Owner/real packaged controls; separate evidence from formal stdio black box."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import sys
sys.dont_write_bytecode = True
from edit_tool_stdio_integration import digest, jpeg_fixture

parser = argparse.ArgumentParser()
parser.add_argument("executable")
parser.add_argument("fixture_executable")
parser.add_argument("--visible", action="store_true")
parser.add_argument("--evidence-root", type=Path)
args = parser.parse_args()
root = args.evidence_root or Path(tempfile.mkdtemp(prefix="shadow-tool-owner-ui-"))
root.mkdir(parents=True, exist_ok=True)
root = root.resolve()
source = root / "synthetic.jpg"
jpeg_fixture(source, str(Path(args.fixture_executable).resolve()))
before = digest(source)
env = dict(os.environ, SHADOW_PIPELINE_SMOKE_ACTION="agent-tools", SHADOW_AGENT_TOOL_EVIDENCE_DIR=str(root),
           RAYON_NUM_THREADS="2", OMP_NUM_THREADS="2")
if args.visible:
    env.pop("QT_QPA_PLATFORM", None)
    env.pop("QT_QUICK_BACKEND", None)
else:
    env.update(QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software")
with (root / "application.log").open("wb") as log:
    result = subprocess.run([str(Path(args.executable).resolve()), "--isolate", str(source)],
                            env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
assert result.returncode == 0, f"owner/UI acceptance exited {result.returncode}; evidence: {root}"
report = json.loads((root / "owner-ui-result.json").read_text())
assert report["result"] == "passed", report
assert digest(source) == before, "original changed"
assert (root / "owner-export.png").read_bytes().startswith(b"\x89PNG")
print(json.dumps(dict(report, artifactRoot=str(root), originalSha256=before)))
