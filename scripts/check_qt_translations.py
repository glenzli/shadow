#!/usr/bin/env python3
"""Validate that the shipped Chinese catalog exactly covers desktop production text."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


INVALID_TRANSLATION_TYPES = frozenset({"unfinished", "vanished", "obsolete"})
PLACEHOLDER_PATTERN = re.compile(r"%(?:L?\d+|n)")


class TranslationContractError(RuntimeError):
    """Raised when a Qt translation catalog violates the release contract."""


@dataclass(frozen=True, order=True)
class MessageKey:
    context: str
    source: str
    comment: str
    message_id: str
    numerus: bool

    def label(self) -> str:
        suffix = f" [{self.comment}]" if self.comment else ""
        return f"{self.context}: {self.source}{suffix}"


@dataclass(frozen=True)
class Translation:
    state: str
    text: str


def read_catalog(path: Path) -> dict[MessageKey, Translation]:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        raise TranslationContractError(f"cannot read Qt catalog {path}: {error}") from error
    if root.tag != "TS":
        raise TranslationContractError(f"{path} is not a Qt TS catalog")

    messages: dict[MessageKey, Translation] = {}
    for context_element in root.findall("context"):
        context = (context_element.findtext("name") or "").strip()
        if not context:
            raise TranslationContractError(f"{path} contains a context without a name")
        for message in context_element.findall("message"):
            source = message.findtext("source") or ""
            key = MessageKey(
                context=context,
                source=source,
                comment=message.findtext("comment") or "",
                message_id=message.get("id", ""),
                numerus=message.get("numerus") == "yes",
            )
            if not source:
                raise TranslationContractError(
                    f"{path} contains a message without source text in {context}"
                )
            if key in messages:
                raise TranslationContractError(
                    f"{path} contains duplicate message identity {key.label()}"
                )
            translation_element = message.find("translation")
            if translation_element is None:
                translation = Translation(state="missing", text="")
            else:
                translation = Translation(
                    state=translation_element.get("type", ""),
                    text="".join(translation_element.itertext()).strip(),
                )
            messages[key] = translation
    return messages


def validate_catalogs(
    committed: dict[MessageKey, Translation],
    observed: dict[MessageKey, Translation],
) -> list[str]:
    errors: list[str] = []
    missing = sorted(observed.keys() - committed.keys())
    stale = sorted(committed.keys() - observed.keys())
    errors.extend(f"missing translation entry: {key.label()}" for key in missing)
    errors.extend(f"stale translation entry: {key.label()}" for key in stale)

    for key in sorted(observed.keys() & committed.keys()):
        translation = committed[key]
        if translation.state == "missing" or translation.state in INVALID_TRANSLATION_TYPES:
            errors.append(
                f"translation is {translation.state or 'unfinished'}: {key.label()}"
            )
            continue
        if not translation.text:
            errors.append(f"translation is empty: {key.label()}")
            continue
        source_placeholders = Counter(PLACEHOLDER_PATTERN.findall(key.source))
        translation_placeholders = Counter(
            PLACEHOLDER_PATTERN.findall(translation.text)
        )
        if source_placeholders != translation_placeholders:
            errors.append(
                "placeholder mismatch: "
                f"{key.label()} source={dict(source_placeholders)} "
                f"translation={dict(translation_placeholders)}"
            )
    return errors


def resolve_tool(explicit: str | None, environment_name: str, default: str) -> str:
    candidate = explicit or os.environ.get(environment_name) or default
    resolved = shutil.which(candidate)
    if resolved is None:
        raise TranslationContractError(
            f"required Qt tool {candidate!r} was not found; "
            f"set {environment_name} to its executable"
        )
    return resolved


def run_tool(arguments: list[str], label: str) -> None:
    result = subprocess.run(
        arguments,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        output = "\n".join(part for part in (result.stdout, result.stderr) if part)
        raise TranslationContractError(
            f"{label} failed with exit code {result.returncode}\n{output}".rstrip()
        )


def repository_root() -> Path:
    return Path(__file__).resolve().parent.parent


def parse_arguments(arguments: list[str]) -> argparse.Namespace:
    root = repository_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--catalog",
        type=Path,
        default=root / "apps/desktop/i18n/shadow_zh_CN.ts",
    )
    parser.add_argument(
        "--source-dir",
        action="append",
        type=Path,
        dest="source_directories",
    )
    parser.add_argument("--lupdate")
    parser.add_argument("--lrelease")
    return parser.parse_args(arguments)


def check(arguments: argparse.Namespace) -> int:
    root = repository_root()
    catalog = arguments.catalog.resolve()
    source_directories = arguments.source_directories or [
        root / "apps/desktop/qml",
        root / "apps/desktop/src",
    ]
    source_directories = [path.resolve() for path in source_directories]
    if not catalog.is_file():
        raise TranslationContractError(f"translation catalog does not exist: {catalog}")
    missing_directories = [path for path in source_directories if not path.is_dir()]
    if missing_directories:
        rendered = ", ".join(str(path) for path in missing_directories)
        raise TranslationContractError(f"translation source directories do not exist: {rendered}")

    lupdate = resolve_tool(arguments.lupdate, "LUPDATE", "lupdate")
    lrelease = resolve_tool(arguments.lrelease, "LRELEASE", "lrelease")
    committed = read_catalog(catalog)

    with tempfile.TemporaryDirectory(prefix="shadow-i18n-") as temporary:
        temporary_root = Path(temporary)
        observed_catalog = temporary_root / catalog.name
        compiled_catalog = temporary_root / f"{catalog.stem}.qm"
        shutil.copy2(catalog, observed_catalog)
        run_tool(
            [
                lupdate,
                *(str(path) for path in source_directories),
                "-recursive",
                "-no-obsolete",
                "-ts",
                str(observed_catalog),
            ],
            "lupdate",
        )
        observed = read_catalog(observed_catalog)
        errors = validate_catalogs(committed, observed)
        if errors:
            raise TranslationContractError("\n".join(errors))
        run_tool(
            [
                lrelease,
                "-nounfinished",
                str(observed_catalog),
                "-qm",
                str(compiled_catalog),
            ],
            "lrelease",
        )
        if not compiled_catalog.is_file() or compiled_catalog.stat().st_size == 0:
            raise TranslationContractError("lrelease did not produce a non-empty QM catalog")

    print(
        "desktop-i18n: "
        f"{len(committed)} production messages, exact Chinese coverage, "
        "placeholders valid, QM compiled"
    )
    return 0


def main(arguments: list[str] | None = None) -> int:
    try:
        return check(parse_arguments(arguments if arguments is not None else sys.argv[1:]))
    except TranslationContractError as error:
        print(f"desktop-i18n: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
