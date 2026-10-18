# Optional bounded header JSON ABI v1

`terra_header_json_abi_version()` returns 1. `terra_header_get_json(uint32_t handle, char *buffer, uint64_t buffer_size, uint64_t *required_size)` returns the ordinary `terrax_world_status` values.

The required size includes NUL. NULL output or zero capacity probes without allocating output; required_size itself is optional. Every call first counts the existing header serializer, validates source strings and checks uint32 representation including NUL. A too-small caller buffer returns BUFFER_TOO_SMALL and writes no output. A sufficient buffer receives the same bytes as `terra_section_get_json(handle,"header",...)` directly, without a transient output allocation or grow operation. The uint64 capacity is compared before any narrowing; the fixed sink uses the representable counted length. Source errors retain their specific diagnostic; representational overflow returns INTERNAL_ERROR with TERRAX_SIZE_OVERFLOW. There is no additional native size policy cap.

The new API does not consult, populate or clear legacy section caches. Mutation between probe and write is supported by recounting; the caller must handle BUFFER_TOO_SMALL again. Worlds remain single-thread-owned, concurrent mutation during one call and overlapping source/destination are unsupported. required_size must not alias the output or world source. This API only removes native producer output allocations; JSON.parse graphs, downstream category ownership and UI caches are not changed.

Build identity declares headerJsonAbiVersion=1 for all/WLD (0 for PLR). Manifests optionally declare `abi.headerJson={version:1,allocationFreeProbe:true,directWrite:true}` and both exports. Old manifests without this capability remain valid; PLR must not declare it.

## Internal buffer audit

TxBuf now has explicit dynamic (zero/default), count and fixed modes. buf_init always sets dynamic mode. Count mode performs checked length arithmetic without touching output data. Fixed mode rejects growth before realloc. buf_u8 and buf_bytes guard writes in count mode; all other common writers compose these. The header serializer has no direct output-data access and is shared unchanged, preserving escaping, numbers, dates and field order.

All production declarations were checked: ordinary locals call buf_init; the API operation response was changed from individual member initialization to a zeroed aggregate; stream/task/marker embedded buffers are zeroed through their enclosing allocations; map result ownership-transfer aggregates retain zero/default dynamic mode. Borrowed stack tile buffers and circuit output windows now explicitly use fixed mode. PLR's separate buffer types are unaffected.

The native contract uses independently generated synthetic legacy/modern long-string fixtures, exact old/new output equality, active header overrides, allocation-attempt counters, canaries, mutation/recount, cache preservation, malformed source and uint32 arithmetic boundaries without huge allocations. Sanitizer execution uses ASan/UBSan; LeakSanitizer is unsupported under the execution sandbox's ptrace and must be run separately in a normal host if needed.
