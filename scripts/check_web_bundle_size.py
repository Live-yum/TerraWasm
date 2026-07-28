#!/usr/bin/env python3
import os
import sys
from pathlib import Path

build_dir = Path(os.environ.get("TERRAWASM_BUILD_DIR", "build-ci"))
js_path = build_dir / "terrax_world_wasm_web.js"
wasm_path = build_dir / "terrax_world_wasm_web.wasm"

missing = [str(path) for path in (js_path, wasm_path) if not path.is_file()]
if missing:
    print("Missing build outputs:", ", ".join(missing), file=sys.stderr)
    sys.exit(2)

js_size = js_path.stat().st_size
wasm_size = wasm_path.stat().st_size
total = js_size + wasm_size

js_limit = int(os.environ.get("TERRAWASM_JS_LIMIT", 220 * 1024))
wasm_limit = int(os.environ.get("TERRAWASM_WASM_LIMIT", 1300 * 1024))
total_limit = int(os.environ.get("TERRAWASM_TOTAL_LIMIT", 1450 * 1024))

print(f"glue js : {js_size:,} bytes (limit {js_limit:,})")
print(f"wasm    : {wasm_size:,} bytes (limit {wasm_limit:,})")
print(f"combined: {total:,} bytes (limit {total_limit:,})")

failures = []
if js_size > js_limit:
    failures.append("glue JS")
if wasm_size > wasm_limit:
    failures.append("WASM")
if total > total_limit:
    failures.append("combined bundle")

if failures:
    print("Size budget exceeded: " + ", ".join(failures), file=sys.stderr)
    sys.exit(1)
