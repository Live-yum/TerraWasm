# Buffered world replacement: candidate-before-swap v1

`terra_world_commit_to_buffer` retains its two-call signature. A successful fill
still returns independently owned output bytes and a fresh world handle. The
new `worldCommit` capability (`version: 1`, `candidateBeforeSwap: true`) is
verified against the compiled version marker and both required exports.

Previously, fill closed the old world before allocating/parsing its replacement.
An injected allocation failure then returned an error with neither the old nor
a replacement handle usable. The regression reproduces that loss on the prior
producer.

The replacement file is now copied into a persistent candidate root and parsed
before closing the old world. The ownership swap performs no allocation. An
allocation/parse/admission failure leaves the old world owner available. The
caller's output must still be ignored on failure; it is not a committed result.

For release-1-through-87 files, the parser's frame-important bitmap is native
scratch. The validated bitmap is copied into extra storage at the end of the
candidate's persistent file allocation before scratch rewind. `file_len` excludes
this ownership tail, so saved bytes remain unchanged. Normal world close releases
the one persistent root. Prepared preview/output caches transfer only after a
candidate is ready, and a failed adoption restores their old ownership.

This is a native owner-replacement guarantee, not a persistence acknowledgement
or a fixed V2 JSON receipt. Existing save preparation may materialize pending
pixel/section data before candidate construction; this change does not claim
that every preparation failure is byte-atomic. It does not permit joining an
existing checkpoint or bypassing source/circuit owner guards.

Candidate allocation temporarily overlaps the old world and output bridge.
The existing native ceiling remains unchanged. Lack of capacity rejects the
candidate rather than freeing the only usable world to force it to fit. Consumer
admission must account for the candidate and host output copy. Host allocation
or delivery failure after a successful swap is not proof of non-execution.

The native regression covers 96 allocation-fault positions across modern and
legacy files, with and without prepared output. Six candidate failures preserve
the old complete state and live-byte count; 90 replacements succeed. Repeated
replacement, bitmap reads, byte-exact reserialization and zero final payloads
are checked. Broader native/sanitizer/Wasm evidence is recorded in acceptance
status after the clean producer build.
