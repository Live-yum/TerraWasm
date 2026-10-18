# Refactor implementation checkpoint — 2026-10-18

Implementation is ongoing on `refactor`. No PR, merge, release or deployment is
implied. `master` is unchanged. This is not completion of all 28 work packages
or all 52 target acceptance gates.

## Current native implementation

- Layout-changing header patches reject changes to dimensions, format version,
  file type and tile format fields before modifying the world. Ordinary workspace
  serialization rejects source-backed worlds rather than producing incomplete
  files. Format/type rejection preserves existing status contracts.
- Standalone JSON mutations own a checkpoint through response/request-cache
  publication. Allocation failure rolls back document changes and preserves the
  diagnostic. Existing caller checkpoints remain caller-owned.
- Successful JSON mutations and binary command batches preserve prepared output
  caches. Failures discard derived output before restoring document state. The
  one-scan PNG/list/MAP and regional replacement contracts pass.
- The response atomicity contract sweeps 160 allocation-failure cases, with and
  without prepared output: 12 rejected mutations roll back; 148 success/cache-fill
  cases complete. Closing releases native payload. The regression fails against
  the previous unsafe API implementation.
- Circuit binding hash-index allocation/complexity checks are integrated.
- Debug native builds link libm. The generated-world checkpoint contract is an
  explicit CTest entry rather than depending only on a private optional fixture.

## Verified producer and consumer

The clean native producer is `f05ae2547060ac0a4cd85df833465b278aa21cf2`, built with
pinned Emscripten 5.0.7 and the all feature set. Native CTest: **33/33** passed.
Current-source Wasm suite: **215/215** passed, zero skips. Web all is buffer-only;
standalone PLR retains its separate filesystem contract. Tests account for both
native and bridge payload and read initial linear memory from the manifest.

The four verified producer artifacts were synchronized into viewer-app commit
`5c857207e105bb052d558ed3d019ee92a04a584c`. Subsequent consumer fixes and scoped
validation are tracked in [viewer implementation status](https://github.com/Live-yan/viewer-app/blob/refactor/docs/refactor/execution-status.md).
This documentation-only update does not change the compiled C sources or replace
that producer identity.

## Remote CI boundary

[Quality run 39093978740](https://github.com/Live-yum/TerraWasm/actions/runs/39093978740)
passed native, WLD, PLR, sanitizer/fuzz and all-Web release jobs. The giant public
Computerraria acceptance fails during import with `TERRAX_CIRCUIT_MEMORY` under
the fixed 160 MiB budget. An import-only diagnostic reproduces that failure with
both the baseline production all-Web binary and this producer on the same pinned
public fixture. This establishes a pre-existing all-Web budget limitation, not
successful full physical CPU acceptance. The red gate remains; the budget was
not raised to hide it.

## Remaining acceptance boundaries

- The complete no-allocation fixed V2 receipt ABI and end-to-end owner receipt
  handling remain incomplete. Unknown outcomes must not be claimed as rollback.
- Cross-realm terminal memory ownership and real device/SDK validation remain
  separate work; native/Node tests are not device acceptance.
- Private fixture files, metadata, hashes and derived results are kept outside
  this repository.
- Storage migration and release rehearsals are consumer prerequisites. No release
  or migration activation is authorized by these results.
