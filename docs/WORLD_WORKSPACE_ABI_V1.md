# World workspace checkpoint ABI v1

The optional manifest descriptor is `abi.worldWorkspace = { "version": 1, "checkpoint": true, "rollback": true }`. Compiled build identity contains `worldWorkspaceAbiVersion: 1` for WLD/all and zero for PLR-only. Consumers must verify descriptor, complete export set and compiled marker together; function presence alone is insufficient.

Exports:

- `terra_world_workspace_abi_version()` returns 1.
- `terra_world_workspace_checkpoint_size(handle, out_bytes)` reports an upper bound for snapshot allocation. Requires the sole owner to be idle with no stream task or checkpoint.
- `terra_world_workspace_begin(handle, max_bytes, out_token)` admits the snapshot before any mutation. The nonzero token belongs to exactly this world handle. Nested checkpoints and stale handles/tokens fail.
- `terra_world_workspace_commit(handle, token)` keeps the candidate workspace and discards the rollback baseline without allocating.
- `terra_world_workspace_rollback(handle, token)` restores the exact prior workspace and original handle without allocating, including under allocation failure.
- `terra_world_workspace_save_to_buffer(handle, output, capacity, out_required)` uses the standard two-call buffer convention while a checkpoint is active. It serializes and parses/validates the candidate without closing/reopening the active world. Persist before checkpoint commit. Roll back after any pre-publication failure.

## Data and allocation contract

The checkpoint contains the `TxWorld` state and owned native allocation bytes, plus the persistent file/column roots of an adopted stream world. Existing section overrides, queued pixel art and metadata are included. Independent persistent pixel/player workspaces are excluded. Derived prepared-output caches and transient responses are discarded before capture; they are rebuildable and do not contain edits or undo authority.

Baseline roots are pinned. A free becomes deferred; realloc copies into a newly owned allocation. Rollback frees post-checkpoint roots and restores the pinned bytes and world structure. Commit frees only deferred baseline roots. The snapshot itself is tracked as live native-owned memory, including its descriptor overhead. Neither finalization branch allocates.

This is a single-owner transaction, not a cross-thread lock. While open, the computation host permits only serialized world operations/reads, serialization and finalization. It does not expose raw pointers, allocator operations, palette/global changes, player/pixel mutations or world lifecycle operations to the callback. Native close, destructive commit and stream constructors reject an active workspace transaction. New native integrations must preserve this restriction: unrelated persistent-owner mutation during a world checkpoint is not supported.

A source-backed dirty world saves through an independent streaming candidate, preserving its active workspace until durable publication and adoption. A buffered dirty world uses checkpoint + non-destructive serialization + publish + commit. The immutable committed file is never used as an automatic replacement for unsaved edits on failure.

## Tests

`python3 scripts/test-world-checkpoint-native.py --ubsan` builds with installed GCC/zlib, tests 100 native/stream allocator rollback cycles, stale/nested tokens, OOM admission and allocation-free finalization, exclusion of an independent 8 MiB persistent owner and a real native pixel workspace (palette, cell contents, all 24 stats words and undo/redo), real prior-unsaved header edits, OOM halfway through mutation, and save/readback without replacing the handle.

`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 python3 scripts/test-world-checkpoint-native.py --asan` exercises allocator lifetime/rollback under ASan+UBSan. LeakSanitizer is separately blocked by the current cloud ptrace environment; normal native allocation counters still return to baseline. Wasm32-pointer operation tests run in ordinary/UBSan builds, not LP64 ASan.

New Emscripten artifacts and browser/device runs are separate release gates. Source ABI tests do not establish that a pre-existing binary implements this ABI.
