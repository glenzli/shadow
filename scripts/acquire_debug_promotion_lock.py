#!/usr/bin/env python3
"""Acquire and release the canonical debug promotion lock with exact errors."""

from __future__ import annotations

import argparse
import errno
import json
import os
import secrets
import socket
import stat
import sys
from datetime import UTC, datetime
from pathlib import Path


LOCK_CONTENDED = 73
ENVIRONMENT_FAILURE = 77


def now() -> str:
    return datetime.now(UTC).isoformat(timespec="seconds").replace("+00:00", "Z")


def environment_error(action: str, path: Path, error: OSError) -> int:
    error_name = errno.errorcode.get(error.errno, "UNKNOWN")
    print(
        f"promote debug build: cannot {action} canonical build lock: {path}",
        file=sys.stderr,
    )
    print(
        f"promote debug build: lock environment failure: "
        f"errno={error_name}({error.errno}) message={error.strerror}",
        file=sys.stderr,
    )
    print(
        "promote debug build: this is an environment or permission failure, "
        "not evidence that another steward owns the lock",
        file=sys.stderr,
    )
    return ENVIRONMENT_FAILURE


def read_owner(lock_path: Path) -> dict[str, object] | None:
    try:
        value = json.loads((lock_path / "owner.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def acquire(lock_path: Path, owner: str) -> int:
    try:
        os.mkdir(lock_path, 0o700)
    except FileExistsError:
        try:
            mode = lock_path.lstat().st_mode
        except OSError as error:
            return environment_error("inspect", lock_path, error)
        if not stat.S_ISDIR(mode):
            print(
                f"promote debug build: canonical build lock exists but is not a "
                f"directory: {lock_path}",
                file=sys.stderr,
            )
            print(
                "promote debug build: malformed lock state is not evidence that "
                "another steward is active",
                file=sys.stderr,
            )
            return ENVIRONMENT_FAILURE
        metadata = read_owner(lock_path)
        print(
            "promote debug build: canonical build lock already exists; another "
            "steward may be active",
            file=sys.stderr,
        )
        print(f"promote debug build: lock path: {lock_path}", file=sys.stderr)
        if metadata is not None:
            public_metadata = {
                key: metadata[key]
                for key in ("schema", "owner", "pid", "host", "acquired_at")
                if key in metadata
            }
            print(
                "promote debug build: lock owner metadata: "
                + json.dumps(public_metadata, sort_keys=True),
                file=sys.stderr,
            )
        return LOCK_CONTENDED
    except OSError as error:
        return environment_error("create", lock_path, error)

    token = secrets.token_hex(16)
    metadata = {
        "schema": 1,
        "owner": owner,
        "token": token,
        "pid": os.getppid(),
        "host": socket.gethostname(),
        "acquired_at": now(),
    }
    try:
        with (lock_path / "owner.json").open("x", encoding="utf-8") as stream:
            json.dump(metadata, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except OSError as error:
        try:
            os.rmdir(lock_path)
        except OSError:
            pass
        return environment_error("record owner for", lock_path, error)
    print(token)
    return 0


def release(lock_path: Path, token: str) -> int:
    metadata = read_owner(lock_path)
    if metadata is None:
        print(
            f"promote debug build: cannot verify canonical build lock owner: {lock_path}",
            file=sys.stderr,
        )
        return LOCK_CONTENDED
    if metadata.get("token") != token:
        print(
            "promote debug build: refusing to release canonical build lock with a "
            "different owner token",
            file=sys.stderr,
        )
        return LOCK_CONTENDED
    try:
        (lock_path / "owner.json").unlink()
        lock_path.rmdir()
    except OSError as error:
        return environment_error("release", lock_path, error)
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    subparsers = result.add_subparsers(dest="command", required=True)

    acquire_parser = subparsers.add_parser("acquire")
    acquire_parser.add_argument("lock_path", type=Path)
    acquire_parser.add_argument("--owner", required=True)

    release_parser = subparsers.add_parser("release")
    release_parser.add_argument("lock_path", type=Path)
    release_parser.add_argument("--token", required=True)
    return result


def main() -> int:
    arguments = parser().parse_args()
    if arguments.command == "acquire":
        owner = arguments.owner.strip()
        if not owner or "\n" in owner or "\r" in owner or len(owner) > 240:
            print("promote debug build: lock owner must fit on one line", file=sys.stderr)
            return 64
        return acquire(arguments.lock_path, owner)
    return release(arguments.lock_path, arguments.token)


if __name__ == "__main__":
    raise SystemExit(main())
