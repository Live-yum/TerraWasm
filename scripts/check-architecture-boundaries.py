#!/usr/bin/env python3
"""Ratchet direct registry dependencies in changed C sources.

Compare lexical occurrences per file, not added diff lines: an existing call
whose formatting or adjacent field access changes is not a new dependency.
This is not a C preprocessor or a proof that legacy dependencies are removed.
"""
from __future__ import annotations
import os
import re
import subprocess
import sys
from collections import Counter

OWNER_PATHS = {"src/terra_registry.c", "src/terra_registry.h"}
API_PATHS = OWNER_PATHS | {"src/terra_api.c"}
# Preserve token separation/newlines while discarding comments and literals.
NON_CODE = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\'')

def dependencies(path: str, source: str) -> Counter:
    source = source.replace("\\\r\n", "").replace("\\\n", "")
    code = NON_CODE.sub(lambda m: "\n" * m.group().count("\n") + " ", source)
    counts = Counter()
    if path not in API_PATHS:
        counts["direct tx_get_world call/declaration outside API"] = len(re.findall(r"\btx_get_world\s*\(", code))
    if path not in OWNER_PATHS:
        counts["registry lookup redeclaration outside owner"] = len(re.findall(r"\bextern\s+TxWorld\s*\*\s*tx_get_world\s*\(", code))
        counts["world slot table access outside owner"] = len(re.findall(r"\bg_worlds\b|\btx_world_registry_slots\s*\(", code))
    if path != "include/terra_types.h":
        counts["duplicate TxWorld definition"] = len(re.findall(r"\btypedef\s+struct\s+TxWorld\b", code))
    return counts

def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], text=True)

def source_at(ref: str, path: str) -> str:
    result = subprocess.run(["git", "show", f"{ref}:{path}"], capture_output=True, text=True)
    if result.returncode == 0:
        return result.stdout
    # Only a genuinely absent path is an empty baseline; other Git errors fail.
    paths = git("ls-tree", "--name-only", ref, "--", path).splitlines()
    if not paths:
        return ""
    raise RuntimeError(result.stderr)

def main() -> int:
    base_ref = os.environ.get("GITHUB_BASE_REF") or os.environ.get("ARCHITECTURE_BASE_REF") or "refactor"
    base = f"origin/{base_ref}"
    try:
        merge_base = git("merge-base", base, "HEAD").strip()
        paths = git("diff", "--no-ext-diff", "--name-only", "--diff-filter=ACMR", merge_base, "HEAD", "--", "src", "include").splitlines()
        violations = []
        for path in paths:
            if not path.endswith((".c", ".h")):
                continue
            before = dependencies(path, source_at(merge_base, path))
            after = dependencies(path, source_at("HEAD", path))
            for kind, count in after.items():
                if count > before[kind]:
                    violations.append(f"{path}: {kind} increased from {before[kind]} to {count}")
    except (subprocess.CalledProcessError, RuntimeError) as error:
        print(f"architecture check could not compare against {base}: {error}", file=sys.stderr)
        return 2
    if violations:
        print("Architecture boundary violations:")
        for violation in violations:
            print(f" - {violation}")
        return 1
    print("Architecture boundary ratchet passed: no increased direct registry or TxWorld-definition dependencies.")
    print("Legacy call sites, macro-expanded dependencies and ownership encapsulation are not certified by this lexical check.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
