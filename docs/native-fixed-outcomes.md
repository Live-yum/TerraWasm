# Fixed world operation outcomes (v2)

This additive ABI covers the world JSON mutation dispatcher (header patch,
chest replacement, bestiary replacement and tile batch update), its response
cache, and explicit world workspace commit/rollback. It does not make every
legacy native function transactional, nor acknowledge host publication,
persistence, a recovery journal, or a UI revision.

## Ownership and effects

`terra_op_execute_json_outcome` takes a caller-owned `TerraOutcomeV2`. The
record is written without allocation after every normal return when its control
pointer is valid. A trap or lost execution does not provide a receipt.

- `NONE`: this invocation did not apply a document mutation. This includes
  admission failures, queries, and response-cache fills.
- `APPLIED`: the mutation applied. `OPEN` is provisional in a caller checkpoint;
  `COMMITTED` has finished the native checkpoint, not host persistence.
- `ROLLED_BACK`: the entire admitted checkpoint was restored. When joining a
  caller checkpoint, this includes earlier provisional edits, not just the
  most recent operation.
- `UNKNOWN`: an internal checkpoint-owner invariant prevented confirmation.
  The owner must be quarantined rather than retrying or guessing rollback.

Joining an existing checkpoint requires `TERRA_OUTCOME_JOIN_CALLER`. A missing
join option rejects before mutation and preserves the caller checkpoint. Query
errors do not roll back caller edits. A successful prepared response cache is
retained; a later cache-fill call reports `NONE`, so the caller must retain the
probe's `APPLIED` evidence for the complete two-call operation.

The revision fields count successful workspace commits per native world handle.
They are not the WLD header revision, are not persisted into the file, and do
not fence legacy mutations outside a workspace. Empty commits also advance the
sequence. Counter exhaustion rejects admission instead of wrapping.

## Wire transport

`terra_op_execute_json_receipt`, `terra_world_workspace_commit_receipt`, and
`terra_world_workspace_rollback_receipt` encode exactly 40 little-endian bytes:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u32 | byte size (40) |
| 4 | u32 | version (2) |
| 8 | i32 | returned status |
| 12 | u32 | effect: NONE=0, APPLIED=1, ROLLED_BACK=2, UNKNOWN=3 |
| 16 | u32 | transaction: IDLE=0, OPEN=1, COMMITTED=2, ROLLED_BACK=3, INVALID=4 |
| 20 | u32 | reserved (zero) |
| 24 | u64 | revision before |
| 32 | u64 | revision after |

This encoding does not copy a native struct or depend on pointer size. Controls
must be live bridge allocations outside rollback ownership. Strings must start
at registered allocation roots and terminate within their allocation. Output,
required-size and receipt spans are checked before execution, including writable
buffer overlap and required-size alignment. Invalid receipt storage rejects
without execution and cannot promise to write a receipt. Valid receipt storage
with invalid other arguments yields `NONE`; it may overwrite aliased caller
input storage, but does not execute the document operation. No input or output
storage may alias the receipt in a valid call.

The manifest declares `worldOutcome` only with all four compiled exports and a
matching v2 marker/build identity. Standalone PLR does not expose this contract.
A host restricted to safe JS integers must reject larger revision values, not
round them. The caller binds the receipt to its operation/call identifier;
there is no unbounded string or identifier allocation in the native record.

## Validation scope

The native contract sweeps 320 allocation-failure positions across owned/joined
and prepared/unprepared mutation paths, checks byte-identical public-fixture
rollback, explicit join rejection, query isolation, cached response reads,
allocation-free terminal receipts, counter exhaustion and deliberately broken
checkpoint ownership. Wire tests cover invalid/overlapping controls, bounded
strings, unaligned receipt bytes, exact little-endian u64 encoding and both
Node/Web Wasm targets. Sanitizer results are recorded separately; ASan/UBSan
must not be described as LeakSanitizer coverage.
