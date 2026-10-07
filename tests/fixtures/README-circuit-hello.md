# Exported HELLO display

`circuit-hello.json` is a synthetic sparse circuit exported by viewer-app's real
`createDemo('hello')` → `snapshotCircuitWithSupports(..., 'all')` path. It contains
no uploaded world data. The fixture stores the SHA-256 of its source generator,
`features/circuit/domain/hello-example.mjs`. The native test additionally pins the
exact JSON bytes; viewer's integration test regenerates and compares those bytes.

Regenerate both copies from a viewer-app checkout with TerraWasm beside it:

```sh
node --import ./scripts/node-alias-register.mjs scripts/generate-hello-circuit-fixture.mjs test/fixtures/circuit/hello.json ../TerraWasm/tests/fixtures/circuit-hello.json
sha256sum ../TerraWasm/tests/fixtures/circuit-hello.json
```

After intentional circuit changes, update `fixtureHash` in `test_hello_display.js`.
The native helper encodes the exported cells into a standard 326 WLD with a
four-cell empty margin. Expected 5×5 letter pixels are independently declared in
the test. Only native HitSwitch, UpdateMech and physical wire/gate propagation
advance the display; no special display or clock behavior is supplied by the host.

`build.ps1` runs this test in all/WLD profiles. The independent quality acceptance
job repeats it on the uploaded Web artifact, checks its compiled commit identity,
and uploads a report with the artifact hash. A disconnected-clock negative control
checks that the display depends on the exported wires. The viewer integration also
verifies actual full/selected transactional writes into another WLD.
