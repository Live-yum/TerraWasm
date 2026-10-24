# Candidate G: exact ordering result cache within the existing optional arena

Local candidate only. Frozen F is unchanged. No acceptance or speed claim yet.

The geometry allocation remains exactly 2 MiB. A fixed, aligned tail of at most 384 KiB holds every cache record, key, input/output buffer and resumable cursor. Geometry uses fewer 144-byte block slots and a correspondingly smaller slot-owner table. The directory remains fixed. No separate cache allocation, pressure callback, export or budget change is introduced. Low-budget geometry-cold disable also disables this cache. Actual allocation failures remain fatal. Cache OFF control preserves this exact partition and charges but bypasses result caching.

The cache belongs to one immutable CxWorld lease. The conservative key contains source gate, original seed rectangle, effective command mask, count and the complete ordered group/offset sequence, including duplicates and invalid references. Exact field comparison establishes a hit. A hash, if introduced later as a filter, could never establish equality. No gate frame, parity, RNG or evaluation result is cached.

Existing order_begin resets and allocations happen before lookup. New internal phase 10 performs bounded input capture and exact lookup; a miss enters unchanged phase 8. A hit explicitly invalidates the shared eval cursor and enters bounded output-copy phase 11. Completed copy enters phase 12 and returns success to the existing VM, whose trip_end and evaluation logic still execute. Normal miss output is captured during original phase 7 and published only after all output has completed. Ordinary next order does not invalidate successful records. Explicit cancellation invalidates cached records without unbounded clearing. Partial output copy is not a completed VM command and remains subject to existing cancellation/transaction rollback.

This changes cache state, IO, callback counts and possible resource usage, not the required ordering result. It is not trace equivalence. Reduced geometry capacity can hurt misses; cache OFF with the same reduced arena must be measured separately from cache ON and frozen F.

Validation gates: standalone exact-key and bounded-state contracts; arena last-slot/wrap/canaries, real charge and disabled/OOM boundaries; real parser/evaluate/VM transaction integration; existing native ordering/replay/f9 oracle plus separately labelled historical codec references; actual Node/Web 16/160 builds and consumers; public import and bounded 180-second comparison. No full one-hour acceptance is authorized at this stage.
