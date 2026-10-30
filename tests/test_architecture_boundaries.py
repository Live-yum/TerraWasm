#!/usr/bin/env python3
"""Exercise the real ratchet against isolated Git histories, without network."""
from pathlib import Path
import os
import subprocess
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
            result = subprocess.run(["python3", str(CHECKER)], cwd=root, env=env,
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


if __name__ == "__main__":
    unittest.main()
