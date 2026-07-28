from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "check_qt_translations.py"
SPEC = importlib.util.spec_from_file_location("check_qt_translations", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CHECKER
SPEC.loader.exec_module(CHECKER)


def catalog(messages: str) -> str:
    return (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<TS version="2.1" language="zh_CN" sourcelanguage="en">'
        f"{messages}</TS>"
    )


def context(name: str, messages: str) -> str:
    return f"<context><name>{name}</name>{messages}</context>"


def message(source: str, translation: str, state: str = "") -> str:
    state_attribute = f' type="{state}"' if state else ""
    return (
        "<message>"
        f"<source>{source}</source>"
        f"<translation{state_attribute}>{translation}</translation>"
        "</message>"
    )


class TranslationCatalogContractTests(unittest.TestCase):
    def read(self, contents: str):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "catalog.ts"
            path.write_text(contents, encoding="utf-8")
            return CHECKER.read_catalog(path)

    def test_exact_finished_catalog_preserves_placeholder_multiplicity(self):
        contents = catalog(
            context(
                "Panel",
                message("Saved %1 of %1 files", "已保存 %1/%1 个文件"),
            )
        )
        parsed = self.read(contents)
        self.assertEqual(CHECKER.validate_catalogs(parsed, parsed), [])

    def test_unfinished_translation_is_rejected(self):
        committed = self.read(
            catalog(context("Panel", message("Open", "打开", "unfinished")))
        )
        observed = self.read(catalog(context("Panel", message("Open", "打开"))))
        errors = CHECKER.validate_catalogs(committed, observed)
        self.assertTrue(any("unfinished" in error for error in errors))

    def test_missing_and_stale_identities_are_rejected(self):
        committed = self.read(catalog(context("Panel", message("Old", "旧"))))
        observed = self.read(catalog(context("Panel", message("New", "新"))))
        errors = CHECKER.validate_catalogs(committed, observed)
        self.assertTrue(any("missing translation entry" in error for error in errors))
        self.assertTrue(any("stale translation entry" in error for error in errors))

    def test_placeholder_mismatch_is_rejected(self):
        committed = self.read(
            catalog(context("Panel", message("Saved %1 files", "已保存文件")))
        )
        observed = self.read(
            catalog(context("Panel", message("Saved %1 files", "已保存 %1 个文件")))
        )
        errors = CHECKER.validate_catalogs(committed, observed)
        self.assertTrue(any("placeholder mismatch" in error for error in errors))

    def test_duplicate_message_identity_fails_closed(self):
        contents = catalog(
            context("Panel", message("Open", "打开") + message("Open", "开启"))
        )
        with self.assertRaises(CHECKER.TranslationContractError):
            self.read(contents)


if __name__ == "__main__":
    unittest.main()
