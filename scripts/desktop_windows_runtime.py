"""Immutable Windows desktop release publication; invoked by the shared xtask workflow."""
from __future__ import annotations

import argparse
from datetime import UTC, datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

REQUIRED = (
    "Shadow.exe", "Shadow Server.exe", "shadow-image-decode-helper.exe",
    "shadow-photo-composition-worker.exe", "Qt6Core.dll", "Qt6Quick.dll",
    "platforms/qwindows.dll", "GeoNames/shadow-geonames-cities-v1.tsv",
    "GeoNames/NOTICE.txt",
)


def verify(directory: Path) -> None:
    missing = [name for name in REQUIRED if not (directory / name).is_file()]
    if missing:
        raise RuntimeError("incomplete Windows deployment: " + ", ".join(missing))


def digests(directory: Path) -> dict[str, str]:
    result = {}
    for path in sorted(directory.rglob("*")):
        if path.is_symlink() or path.is_junction():
            raise RuntimeError(f"deployment contains an external link: {path}")
        if path.is_file():
            with path.open("rb") as stream:
                result[path.relative_to(directory).as_posix()] = hashlib.file_digest(stream, "sha256").hexdigest()
    return result


def promote(root: Path, candidate: Path, source: Path, label: str) -> Path:
    candidate = candidate.resolve(strict=True)
    if not label.strip():
        raise ValueError("a validation label is required")
    if candidate == source or source in candidate.parents:
        raise ValueError("the deployment candidate must be outside the source tree")
    if candidate == root or candidate in root.parents:
        raise ValueError("the release directory must not be nested in the candidate")
    verify(candidate)
    expected = digests(candidate)
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    status = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain=v1"])
    identity = datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ") + "-" + revision[:12] + "-" + uuid.uuid4().hex[:8]
    release_root = root / "releases" / "windows-debug"
    release_root.mkdir(parents=True, exist_ok=True)
    incoming = release_root / (".incoming-" + identity)
    release = release_root / identity
    shutil.copytree(candidate, incoming)
    # Preserve a failed candidate for diagnosis. Never change a release the user may be running.
    if digests(incoming) != expected:
        raise RuntimeError(f"copy verification failed; candidate retained at {incoming}")
    manifest = {"schema": 1, "source_revision": revision, "validation_label": label,
                "source_status_sha256": hashlib.sha256(status).hexdigest(), "files": expected}
    (incoming / "build-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    incoming.rename(release)
    pointer = root / "current-debug-windows.json"
    previous = json.loads(pointer.read_text(encoding="utf-8")) if pointer.exists() else None
    next_pointer = root / (".current-debug-windows-" + uuid.uuid4().hex + ".json")
    value = {"schema": 1, "release": release.relative_to(root).as_posix(),
             "previous_release": previous.get("release") if previous else None}
    with next_pointer.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(next_pointer, pointer)
    return release


def current_release(root: Path) -> Path:
    pointer = json.loads((root / "current-debug-windows.json").read_text(encoding="utf-8"))
    if pointer.get("schema") != 1:
        raise ValueError("unknown Windows debug pointer schema")
    release = (root / pointer["release"]).resolve(strict=True)
    if not release.is_relative_to((root / "releases" / "windows-debug").resolve()):
        raise ValueError("Windows debug pointer escapes the release directory")
    verify(release)
    return release


def launch(root: Path, arguments: list[str]) -> None:
    release = current_release(root)
    if arguments[:1] == ["--check"]:
        if len(arguments) != 1:
            raise ValueError("--check does not accept extra arguments")
        print(f"Windows debug applications: {release}")
        return
    executable = "Shadow.exe"
    if arguments and arguments[0] == "--server":
        executable = "Shadow Server.exe"
        arguments = arguments[1:]
    foreground = bool(arguments and arguments[0] == "--foreground")
    if foreground:
        arguments = arguments[1:]
    log_root = root / "logs"
    log_root.mkdir(exist_ok=True)
    log_path = log_root / ("shadow-server-debug.log" if executable != "Shadow.exe" else "shadow-debug.log")
    environment = os.environ.copy()
    environment.setdefault("QT_FORCE_STDERR_LOGGING", "1")
    # The standalone Server uses StorageProfile directly, rather than the
    # desktop's launch-history registry. Select its development library even
    # for RelWithDebInfo, while retaining intentional profile/root overrides.
    if executable == "Shadow Server.exe":
        selected = any(argument in ("--profile", "--profile-root")
                       or argument.startswith(("--profile=", "--profile-root="))
                       for argument in arguments)
        if not selected and "SHADOW_SERVER_MANAGER_DATA_ROOT" not in environment:
            normal_context = "--profile-launch-context=normal" in arguments or any(
                arguments[index:index + 2] == ["--profile-launch-context", "normal"]
                for index in range(len(arguments)))
            arguments = [*arguments, "--profile", "daily" if normal_context else "debug"]
    with log_path.open("ab") as log:
        child = subprocess.Popen([str(release / executable), *arguments], cwd=release,
                                 stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                 env=environment,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        print(f"Started {executable} (pid {child.pid}); log: {log_path}")
        if foreground:
            raise SystemExit(child.wait())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("promote", "run"))
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--label")
    options, arguments = parser.parse_known_args()
    root = options.build_root.resolve()
    if options.action == "promote":
        if arguments or options.candidate is None or options.source_root is None or options.label is None:
            parser.error("promotion requires candidate, source root, and validation label")
        print(f"Windows debug applications: {promote(root, options.candidate, options.source_root.resolve(), options.label)}")
    else:
        launch(root, arguments[1:] if arguments[:1] == ["--"] else arguments)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Windows desktop workflow: {error}", file=sys.stderr)
        raise SystemExit(1) from error
