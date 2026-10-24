# Candidate H: empty-colour-only traversal omission

Based on frozen G. Exact-order result cache, geometry partition, budgets, exports and original stimulus protocol remain unchanged.

One named eligible_mask field is reset at every order_begin. The existing bounded phase9 rank-reset pass, only after all candidate preparation and replay have completed, ORs a colour bit when the candidate first matching colour is 0..3, enabled by the original effective mask, and outside the final source seed rectangle. The colour check short-circuits invalid candidate coordinates. Duplicates naturally share a bit. Unreachable eligible candidates retain their bit. The original command mask and cache key are not narrowed or mutated. Numeric colour3 is yellow.

At phase1 only, skip absent colour bits before clearing visits or seeding traversal. All nonempty colours execute the original full traversal, including the tail after the last useful rank. There is no mid-colour stop, remaining-count pruning, deadline heuristic, or cache tuning. Existing import validation and immutable source lease underpin result equivalence; IO, operation counts, allocation requirements and yield traces intentionally need not match. No pending geometry read is abandoned because phase1 is entered only after completed preparation or a fully drained preceding colour.

Validation distinguishes adapted-candidate FIFO/oracle tests from inherited real general-parser/VM cache integration. The latter retains documented gaps for ordinary RNG, standard-parser cached hits and outer-command transaction coverage. Historical codec tests remain reference-only. Original 1h full public CPU acceptance remains outstanding; the authorized 180s run is only a bounded observation.

## Actual layout and charge

The named uint8_t eligible_mask occupies existing padding immediately after current_geometry. Native LP64 compiled measurement is G=376 / H=376 bytes; Emscripten wasm32 LLVM constants are G=320 / H=320. There is no additional mandatory CxOrder charge, including when the optional cold layer is disabled. All G geometry allocations and conservative F admission preflight are unchanged, retaining the 10924-byte slot-owner reduction versus F when enabled. Other ABI layouts must be measured rather than inferred.
