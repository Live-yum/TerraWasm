#!/usr/bin/env python3
"""Exercise the real ratchet against isolated Git histories, without network."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

CHECKER = Path(__file__).resolve().parents[1] / "scripts/check-architecture-boundaries.py"


class ArchitectureBoundaries(unittest.TestCase):
    def check_change(self, path, before, after, expected):
        with tempfile.TemporaryDirectory(prefix="terra-architecture-") as tmp:
            root = Path(tmp)
            def git(*args):
                return subprocess.run(["git", *args], cwd=root, check=True,
                                      capture_output=True, text=True)
            git("init", "-q")
            git("config", "user.name", "Architecture fixture")
            git("config", "user.email", "fixture@example.invalid")
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(before)
            git("add", ".")
            git("commit", "-qm", "baseline")
            git("update-ref", "refs/remotes/origin/refactor", "HEAD")
            target.write_text(after)
            git("add", ".")
            git("commit", "-qm", "candidate")
            env = dict(os.environ, ARCHITECTURE_BASE_REF="refactor")
            env.pop("GITHUB_BASE_REF", None)
            result = subprocess.run([sys.executable, str(CHECKER)], cwd=root, env=env,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
            if expected:
                self.assertIn("Architecture boundary violations:", result.stdout)

    def test_api_document_migration_is_allowed(self):
        self.check_change("src/terra_api.c",
                          "TxWorld* world=tx_get_world(handle);uint64_t n=world->workspace_revision;\n",
                          "TxWorld* world=tx_get_world(handle);uint64_t n=world->document.workspace_revision;\n", 0)

    def test_new_api_handle_resolution_is_allowed(self):
        self.check_change("src/terra_api.c", "/* API */\n", "TxWorld* world=tx_get_world(handle);\n", 0)

    def test_core_handle_resolution_is_rejected(self):
        self.check_change("src/terra_map.c", "/* core */\n", "TxWorld* world=tx_get_world(handle);\n", 1)

    def test_api_cannot_redeclare_registry(self):
        self.check_change("src/terra_api.c", "/* API */\n", "extern TxWorld* tx_get_world(uint32_t);\n", 1)

    def test_core_cannot_redeclare_registry(self):
        self.check_change("src/terra_map.c", "/* core */\n", "extern TxWorld* tx_get_world(uint32_t);\n", 1)

    def test_registry_owner_is_allowed(self):
        self.check_change("src/terra_registry.c", "/* owner */\n",
                          "static TxWorld g_worlds[1];\nTxWorld* tx_get_world(uint32_t h) { return 0; }\n", 0)

    def test_api_cannot_access_slot_table(self):
        self.check_change("src/terra_api.c", "/* API */\n", "TxWorld* world=&g_worlds[0];\n", 1)

    def test_core_cannot_request_slot_table(self):
        self.check_change("src/terra_map.c", "/* core */\n", "TxWorld* world=tx_world_registry_slots();\n", 1)

    def test_multiline_core_lookup_is_rejected(self):
        self.check_change("src/terra_map.c", "/* core */\n", "tx_get_world\n(handle);\n", 1)

    def test_comments_and_strings_are_not_dependencies(self):
        self.check_change("src/terra_map.c", "/* core */\n",
                          '/* tx_get_world(handle); */\n// g_worlds\nchar* text="typedef struct TxWorld";\n', 0)

    def test_existing_core_call_reformat_is_not_increase(self):
        self.check_change("src/terra_map.c", "TxWorld* w=tx_get_world(h); int n=w->version;\n",
                          "TxWorld* w=tx_get_world\n(h); int n=w->document.version;\n", 0)

    def test_second_core_call_is_rejected(self):
        self.check_change("src/terra_map.c", "tx_get_world(h);\n",
                          "tx_get_world(h); tx_get_world(other);\n", 1)

    def test_multiline_duplicate_type_is_rejected(self):
        self.check_change("include/other.h", "/* types */\n",
                          "typedef\nstruct\nTxWorld { int field; } TxWorld;\n", 1)

    def test_multiline_registry_declaration_is_rejected(self):
        self.check_change("src/terra_api.c", "/* API */\n",
                          "extern\nTxWorld*\ntx_get_world\n(uint32_t);\n", 1)

    def test_duplicate_world_type_is_rejected(self):
        self.check_change("include/other.h", "/* other */\n", "typedef struct TxWorld { int field; } TxWorld;\n", 1)

    def test_canonical_world_type_is_allowed(self):
        self.check_change("include/terra_types.h", "/* types */\n", "typedef struct TxWorld { int field; } TxWorld;\n", 0)


class ArchitectureEncoding(unittest.TestCase):
    """Source bytes are UTF-8; Git diagnostics can use a different locale."""

    def setUp(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("architecture_checker", CHECKER)
        self.checker = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.checker)

    def test_localized_diagnostic_does_not_block_missing_baseline(self):
        from unittest.mock import patch
        result = subprocess.CompletedProcess([], 128, b"", b"\xd6\xd0\xce\xc4")
        with patch.object(self.checker.subprocess, "run", return_value=result), \
             patch.object(self.checker, "git", return_value=""):
            self.assertEqual(self.checker.source_at("base", "src/new.c"), "")

    def test_localized_diagnostic_preserved_on_real_failure(self):
        from unittest.mock import patch
        result = subprocess.CompletedProcess([], 128, b"", b"\xd6\xd0\xce\xc4")
        with patch.object(self.checker.subprocess, "run", return_value=result), \
             patch.object(self.checker, "git", return_value="src/existing.c\n"):
            with self.assertRaises(RuntimeError) as error:
                self.checker.source_at("base", "src/existing.c")
        self.assertEqual(str(error.exception), r"\xd6\xd0\xce\xc4")

    def test_source_utf8_is_strict_even_with_localized_stderr(self):
        from unittest.mock import patch
        result = subprocess.CompletedProcess([], 0, b"/* \xff */", b"\xd6\xd0")
        with patch.object(self.checker.subprocess, "run", return_value=result):
            with self.assertRaises(UnicodeDecodeError):
                self.checker.source_at("HEAD", "src/invalid.c")

    def test_valid_utf8_source_ignores_diagnostic_encoding(self):
        from unittest.mock import patch
        source = "/* 中文注释 */\n"
        result = subprocess.CompletedProcess([], 0, source.encode("utf-8"), b"\xd6\xd0")
        with patch.object(self.checker.subprocess, "run", return_value=result):
            self.assertEqual(self.checker.source_at("HEAD", "src/valid.c"), source)

    def check_locale(self, name, expected_codec):
        if os.name == "nt":
            self.skipTest("POSIX locale encodings do not apply on Windows")
        import codecs
        import sys
        env = dict(os.environ, LC_ALL=name, LANG=name,
                   PYTHONUTF8="0", PYTHONCOERCECLOCALE="0",
                   ARCHITECTURE_BASE_REF="refactor")
        env.pop("GITHUB_BASE_REF", None)
        # Explicit setlocale prevents silently testing the fallback C locale.
        probe = subprocess.run([sys.executable, "-c",
            "import locale,sys; locale.setlocale(locale.LC_ALL, sys.argv[1]); "
            "print(locale.getpreferredencoding(False)); print(sys.flags.utf8_mode)", name],
            env=env, capture_output=True)
        if probe.returncode:
            self.skipTest(f"locale {name} unavailable: {probe.stderr!r}")
        lines = probe.stdout.decode("ascii").splitlines()
        self.assertEqual(lines[1], "0", "Python UTF-8 mode must be disabled")
        self.assertEqual(codecs.lookup(lines[0]).name, codecs.lookup(expected_codec).name)
        for violation in (False, True):
            with self.subTest(locale=name, violation=violation):
                with tempfile.TemporaryDirectory(prefix="terra-encoding-") as tmp:
                    root = Path(tmp)
                    def git(*args):
                        return subprocess.run(["git", *args], cwd=root, env=env,
                                              check=True, capture_output=True)
                    git("init", "-q")
                    git("config", "user.name", "Encoding fixture")
                    git("config", "user.email", "fixture@example.invalid")
                    (root / "README").write_bytes(b"baseline\n")
                    git("add", ".")
                    git("commit", "-qm", "baseline")
                    git("update-ref", "refs/remotes/origin/refactor", "HEAD")
                    target = root / "src/new.c"
                    target.parent.mkdir()
                    target.write_text("/* 中文注释 */\n" +
                                      ("tx_get_world(h);\n" if violation else "int value;\n"),
                                      encoding="utf-8")
                    git("add", ".")
                    git("commit", "-qm", "candidate")
                    result = subprocess.run([sys.executable, str(CHECKER)], cwd=root,
                                            env=env, capture_output=True)
                    output = result.stdout.decode("utf-8")
                    self.assertEqual(result.returncode, int(violation), result.stderr)
                    self.assertIn("Architecture boundary violations:" if violation else
                                  "Architecture boundary ratchet passed:", output)
                    self.assertNotIn(b"Traceback", result.stderr)

    def test_new_file_utf8_locale(self):
        self.check_locale("C.utf8", "utf-8")

    def test_new_file_ascii_locale(self):
        self.check_locale("C", "ascii")

    def test_new_file_gb2312_locale(self):
        self.check_locale("zh_CN", "gb2312")


if __name__ == "__main__":
    unittest.main()
