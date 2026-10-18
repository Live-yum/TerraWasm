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

## Player, graph and process-palette checkpoint isolation — de11f72

Producer `de11f72ab18240979093135727207760cc39c4e9` extends foreign-owner
admission guards to player document creation/mutation/close/filesystem save,
circuit graph creation/load/compile/patch/traversal/cancel/close, and process
map-palette installation/clearing. A blocked graph step is an admission rejection,
not execution cancellation: an existing queued traversal remains unchanged.

Player reads remain available. Cold full-JSON queries serialize into scoped
scratch rather than publishing a cache. Buffer encoding uses a stack document
copy during a foreign checkpoint, borrows existing caches/original bytes, and
releases only newly encoded temporary bytes on every return path. The baseline
cold-query regression retains a cache root that a later world rollback frees;
the new regression checks zero retained query bytes and valid subsequent reads.

Native CTest: 36/36; standalone PLR native profile: 4/4. Clean Emscripten 5.0.7
Node/Web plus production-consumer tests: 220/220, zero skips. Tests cover cold
and warm player caches, short buffers, injected allocation failure, world commit
and rollback, and the full queued-graph state vector before resuming execution.
The two focused ASan/UBSan contracts pass with the prior explicit LeakSanitizer
limitation. Web wrapper is 21,832 bytes and Wasm is 456,866 bytes; budgets pass.
Fixed V2 receipts and the broader acceptance program remain incomplete.

## Native pointer-span boundary — 965d533

Producer `965d5336a437a3906e2b6d4aa296fc1b3d5d0e8f` introduces native pointer/
size_t spans for stream leases/events and circuit command/results, full-pointer
bridge allocation/range checks, and checked Wasm32 range encoding. An invalid
wire range is rejected rather than truncated; its pending event/lease admission
can be retried through the native adapter. Pixel bulk forwarding also no longer
narrows its internal pointer. Wire layouts, export names and versions remain
unchanged. See [native pointer spans](../native-pointer-spans.md).

The native stream/header tests and manual scripts no longer force low addresses
or adjust glibc allocation. Both executables are verified PIE; actual allocations
above UINT32_MAX pass the range/free contract. Native CTest 37/37 and full
ASan/UBSan CTest 37/37 pass, now including the formerly excluded stream/header
paths. Standalone PLR native 5/5 and separate C++ linkage smoke pass. The manual
stream normal/ASan and checkpoint ASan runners pass; the PLR differential codec
golden is unchanged. Leak detection remains disabled only for this executor's
previously documented limitation, not in the checked-in scripts.

Clean Emscripten 5.0.7 Node/Web plus production-consumer tests: 220/220, zero
skips. Web wrapper is 21,832 bytes; Wasm is 457,472 bytes; artifact budgets pass.
G06's LP64 response/roundtrip/free and Wasm32 layout/sanitizer scenario is verified
for this producer. This is not completion of fixed V2 outcome receipts or every
remaining work package.

## Fixed world outcome control records — fddd743

Producer `fddd743645cd08f7501665117abe229183411a10` adds caller-owned V2 native
outcomes and checked, explicit little-endian 40-byte wire receipts for the world
JSON mutation dispatcher and workspace completion. Existing caller checkpoints
must be joined explicitly. A joined mutation failure confirms rollback of the
entire caller checkpoint, including earlier provisional edits. A response-cache
fill reports no new effect; it must not erase the probe's applied evidence.
See [fixed outcome scope and wire contract](../native-fixed-outcomes.md).

Native CTest 38/38 and ASan/UBSan 38/38 pass; the previously documented executor
LeakSanitizer limitation still applies. Standalone PLR native remains 5/5. The
320-case allocation-fault sweep yields two pre-admission rejections, 20 confirmed
whole-checkpoint rollbacks and 298 applied/cache-fill cases. Tests additionally
cover bounded/overlapping wire buffers, allocation-free completion controls,
unsafe-address admission and complete 64-bit little-endian commit sequences.

Clean Emscripten 5.0.7 Node/Web plus production-consumer regression: 222/222,
zero skips. Web wrapper: 22,370 bytes; Wasm: 459,977 bytes; artifact budgets pass.
The manifest capability is checked against exports, the real version marker
and compiled identity; standalone PLR does not advertise world outcomes.

P05/G05 remain partial: these receipts cover the named world operations, not
all native mutation families or host publication/persistence. Their per-handle
workspace commit sequence is not a universal document revision. The consumer
must retain applied evidence across output allocation and transport failures;
its verification is tracked in viewer-app. No release, CI rerun or migration
activation is claimed.
