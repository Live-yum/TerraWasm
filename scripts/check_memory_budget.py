#!/usr/bin/env python3
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
WEB_EXPORTS = (ROOT / "exports.web.txt").read_text(encoding="utf-8").splitlines()

MiB = 1024 * 1024
LIMITS = {
    "web_initial": 16 * MiB,
    "web_max": 112 * MiB,
    "node_initial": 32 * MiB,
    "node_max": 192 * MiB,
}


def target_block(name: str) -> str:
    marker = f"add_executable({name} "
    start = CMAKE.find(marker)
    if start < 0:
        raise AssertionError(f"missing target {name}")
    next_target = CMAKE.find("add_executable(", start + len(marker))
    end_else = CMAKE.find("else()", start)
    candidates = [value for value in (next_target, end_else) if value >= 0]
    end = min(candidates) if candidates else len(CMAKE)
    return CMAKE[start:end]


def memory_value(block: str, option: str) -> int:
    match = re.search(rf'-s{option}=(\d+)', block)
    if not match:
        raise AssertionError(f"missing -s{option}")
    return int(match.group(1))


node = target_block("terrax_world_wasm")
web = target_block("terrax_world_wasm_web")
actual = {
    "node_initial": memory_value(node, "INITIAL_MEMORY"),
    "node_max": memory_value(node, "MAXIMUM_MEMORY"),
    "web_initial": memory_value(web, "INITIAL_MEMORY"),
    "web_max": memory_value(web, "MAXIMUM_MEMORY"),
}

for key, expected in LIMITS.items():
    value = actual[key]
    if value != expected:
        raise AssertionError(f"{key}: expected {expected}, got {value}")
    if value % 65536 != 0:
        raise AssertionError(f"{key} must be a WebAssembly page multiple")

if actual["web_max"] >= 200_000_000:
    raise AssertionError("web maximum memory must remain below 200 MB")
if actual["node_max"] >= 202 * MiB:
    raise AssertionError("node validation memory ceiling unexpectedly increased")
if '-sABORTING_MALLOC=0' not in CMAKE:
    raise AssertionError("OOM must be reported as a recoverable allocation failure")
if '-sFILESYSTEM=0' not in web:
    raise AssertionError("web target must remain buffer-only with FILESYSTEM=0")

exports = {line.strip() for line in WEB_EXPORTS if line.strip() and not line.lstrip().startswith('#')}
required = {
    "_terra_world_open_from_buffer",
    "_terra_world_save_to_buffer",
    "_terra_world_close",
    "_tx_memory_used",
    "_tx_heap_used",
    "_tx_bridge_heap_used",
    "_tx_native_heap_used",
    "_tx_reclaim_transients",
}
missing = sorted(required - exports)
if missing:
    raise AssertionError(f"web exports missing: {', '.join(missing)}")
for forbidden in ("_terra_world_open", "_terra_world_save"):
    if forbidden in exports:
        raise AssertionError(f"path API leaked into web exports: {forbidden}")

print("TerraWasm memory budget configuration")
for key in ("web_initial", "web_max", "node_initial", "node_max"):
    print(f"  {key}: {actual[key]} bytes ({actual[key] / MiB:.1f} MiB)")
print("  web filesystem: disabled")
print("  allocation failure: recoverable")
print("  buffer-only ABI: verified")
sys.exit(0)
