# Computerraria acceptance inputs

The supported acceptance path is **WLD-only**. See [README-wld.md](README-wld.md)
for the pinned real-world CPU, RAM signatures, negative control, memory limits,
and save/reopen verification. Run `tests/test_computerraria_wld.js`.

`fetch-inputs.py DIRECTORY` fetches and verifies only the pinned WLD archive and
extracts the WLD. `--wld-only` remains a compatibility no-op. No TWLD is fetched,
parsed, stored by the harness, or required for acceptance. Ordinary Terraria
PixelBox tests remain mandatory; WireHead ColorPixelBox compatibility is retired.

`acceptance-result.json` is a preserved **historical modded-runtime report**, not
a current acceptance result. Its old sidecar hashes and display results must not
be presented as evidence for the WLD-only runtime. Current reports must identify
the exact tested artifact and source hashes and clearly state skipped checks.
