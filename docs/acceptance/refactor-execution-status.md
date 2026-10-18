# Refactor implementation checkpoint — 2026-10-18

Implementation is ongoing on `refactor`. No PR, merge, release or deployment is
implied. This task does not write to `master`. This is not completion of all 28 work packages
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

## Earlier verified producer and consumer

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


## Circuit-world source lease — aadba3f

Producer `aadba3fe833b194ec6e349cdaf69eeabc8d0922e` pins the native source
world until every circuit-world session has confirmed close. Cancellation alone
does not release the lease. World retirement, in-place legacy pixel writes,
checkpoint admission and ordinary save materialization reject while pinned.
Circuit begin also rejects an active workspace checkpoint before allocation.

Detached stream output remains supported. Adoption preflights the current owner
before modifying the READY candidate and propagates activation failure instead
of ignoring close errors. After circuit close, the same candidate can be adopted.
The native regression failed against the prior implementation, which closed the
source world while a circuit still held its pointer.

Verification: native CTest 34/34; clean Emscripten 5.0.7 all-feature Node/Web
regression plus production-consumer contract 216/216, zero skips. Web wrapper
21,832 bytes; Wasm 455,930 bytes; artifact budget passes. The focused ASan/UBSan
contract passes with leak detection disabled: LeakSanitizer cannot initialize
in this executor's ptrace environment, so no LeakSanitizer pass is claimed.
The contracts independently assert final native and bridge payloads are zero.

This closes the circuit-world source lifetime boundary, not all foreign-owner
allocations during allocator checkpoints. The fixed V2 receipt and complete
owner guard matrix remain open. No CI rerun, release or migration activation is
implied by these local checks. Consumer artifact synchronization is tracked in
the viewer implementation status linked above.

## Pixel foreign-checkpoint admission — 539b322

Producer `539b322981b4e7b3b7788666953dad201f6062f3` rejects pixel workspace
creation, mutations, transaction completion, history changes and close while a
world allocator checkpoint is active. Rejection occurs before mutation argument
validation: an invalid foreign call must not invoke pixel rollback or free a
pixel owner's existing transaction. Stats, palette/cell queries and bounded
color matching remain available without retained allocations.

The prior implementation fails the creation-admission regression. Native tests
exercise both world commit and rollback with pending pixel edits, compare the
complete pixel state vector, and prove subsequent pixel commit/undo/redo work.
Native CTest 35/35; clean Emscripten 5.0.7 Node/Web and production-consumer tests
218/218 with zero skips. Focused ASan/UBSan passes with the same explicit
LeakSanitizer limitation above. Web Wasm is 456,152 bytes, wrapper 21,832 bytes;
artifact budgets pass. Other foreign-owner families and the fixed V2 receipt
remain separate incomplete work.
