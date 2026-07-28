#!/usr/bin/env python3
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path.cwd()
REPORTS = ROOT / "reports"
DIST = ROOT / "dist"
REPORTS.mkdir(exist_ok=True)
DIST.mkdir(exist_ok=True)

steps = []

def run(name, command):
    started = datetime.now(timezone.utc).isoformat()
    proc = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    (REPORTS / f"{name}.log").write_text(
        proc.stdout + ("\n[stderr]\n" + proc.stderr if proc.stderr else ""),
        encoding="utf-8",
    )
    steps.append({
        "name": name,
        "command": " ".join(command),
        "startedAt": started,
        "finishedAt": datetime.now(timezone.utc).isoformat(),
        "status": proc.returncode,
    })
    print(f"[p0-wasm] {name}: {'passed' if proc.returncode == 0 else f'failed ({proc.returncode})'}")
    return proc.returncode == 0

configure_ok = run("configure", [
    "emcmake", "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=MinSizeRel",
])
build_ok = configure_ok and run("build", [
    "cmake", "--build", "build", "--target",
    "terrax_world_wasm", "terrax_world_wasm_web", "--parallel", "2",
])
lifecycle_ok = build_ok and run("lifecycle", ["node", "tests/test_buffer_lifecycle.js"])
size_ok = build_ok and run("size-budget", [sys.executable, "scripts/check_web_bundle_size.py"])

outputs = {}
if build_ok:
    for name in ("terrax_world_wasm_web.js", "terrax_world_wasm_web.wasm"):
        source = ROOT / "build" / name
        if source.is_file():
            target = DIST / name
            shutil.copy2(source, target)
            outputs[name] = target.stat().st_size

report = {
    "generatedAt": datetime.now(timezone.utc).isoformat(),
    "commit": os.environ.get("GITHUB_SHA", ""),
    "outputs": outputs,
    "steps": steps,
    "passed": bool(configure_ok and build_ok and lifecycle_ok and size_ok),
}
(REPORTS / "p0-wasm-validation.json").write_text(
    json.dumps(report, ensure_ascii=False, indent=2) + "\n",
    encoding="utf-8",
)

sys.exit(0 if report["passed"] else 1)
