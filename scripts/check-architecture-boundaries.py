#!/usr/bin/env python3
"""Reject new direct dependencies on the legacy world-handle registry.

This is a ratchet, not a claim that the current codebase is already layered:
existing tx_get_world call sites remain visible debt. New call sites must go
through the public API/application boundary until the registry is extracted.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

base_ref = os.environ.get("GITHUB_BASE_REF") or os.environ.get("ARCHITECTURE_BASE_REF") or "refactor"
base = f"origin/{base_ref}"

def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], text=True).strip()

try:
    merge_base = git("merge-base", base, "HEAD")
    diff = git("diff", "--no-ext-diff", "--unified=0", f"{merge_base}...HEAD", "--", "src", "include")
except subprocess.CalledProcessError as error:
    print(f"architecture check could not compare against {base}: {error}", file=sys.stderr)
    sys.exit(2)

violations: list[str] = []
current_path = ""
for line in diff.splitlines():
    if line.startswith("+++ b/"):
        current_path = line[6:]
        continue
    if not line.startswith("+") or line.startswith("+++"):
        continue
    added = line[1:]
    if re.search(r"\btx_get_world\s*\(", added) and current_path != "src/terra_registry.c":
        violations.append(
            f"{current_path}: new direct tx_get_world() use; resolve numeric handles at the API boundary"
        )
    if re.search(r"\bextern\s+TxWorld\s*\*\s*tx_get_world\s*\(", added) and current_path != "src/terra_registry.c":
        violations.append(
            f"{current_path}: do not redeclare the API registry lookup in feature/core modules"
        )
    if re.search(r"typedef\s+struct\s+TxWorld\b", added) and current_path != "include/terra_types.h":
        violations.append(
            f"{current_path}: TxWorld's document representation must have one canonical definition"
        )

if violations:
    print("Architecture boundary violations:")
    for violation in violations:
        print(f" - {violation}")
    sys.exit(1)

print("Architecture boundary ratchet passed: no new direct world-registry or TxWorld-definition dependencies.")
print("Legacy call sites remain and are not certified as migrated by this check.")
