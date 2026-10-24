# Native pointer spans and the Wasm32 boundary

The existing Wasm export names, control layouts, versions and numeric offsets
are unchanged. Native LP64 callers must not put a real pointer into a `uint32_t`
wire field. `terra_native_span.h` defines non-owning, platform-native pointer and
`size_t` length/capacity spans; their C layout is not a wire format.

## Native bridge allocation

- Use `tx_bridge_alloc_native` / `tx_bridge_free_native` for native bridge and
  stream control buffers. Range and exact-root size queries preserve the full
  pointer. Payload sizes remain bounded by the allocator's existing uint32 limit.
- `tx_wasm_span32` checks both the pointer and complete byte range before encoding
  an offset. `tx_wasm_offset32` is the zero-length convenience adapter.
- Legacy `tx_malloc` either returns a representable Wasm32 range or frees the
  candidate allocation and returns zero. It never truncates a native address.

## Native streaming and circuit adapters

- `terra_world_stream_step_native` returns the unchanged event control record
  with `data_ptr == 0` and a separate `TerraConstSpan` for output bytes.
- `terra_world_stream_acquire_input_native` returns the lease control record with
  `data_ptr == 0` and a writable `TerraMutableSpan`. Fill exactly the requested
  length and commit the existing lease ID/range. Duplicate/stale/growth rejection
  behavior is unchanged.
- `terra_circuit_world_step_native` returns circuit output/result spans in the
  same way. `terra_circuit_world_command_native` receives command data separately;
  its wire `data_ptr` must be zero, with an aligned span of `data_count * 16` bytes.
- Returned spans are borrowed. Do not free or retain them past ack, commit,
  cancel or close. Caller-owned control buffers are released separately.
- A wire adapter rejects an unrepresentable native output pointer without
  consuming the outstanding event or creating an input lease. Native callers
  can retry with the span adapter; there is no truncated-pointer fallback.

The decoded Wasm32 fields in legacy pixel/icon bridge specifications remain
wire offsets, not native pointers. Circuit command/result and stream internal
paths no longer narrow real pointers; pixel bulk forwarding also uses a real
pointer internally rather than a pointer-to-offset-to-pointer round trip.

## Verification boundary

Native allocation contracts exercise addresses above UINT32_MAX, full-range
boundary rejection, unchanged bridge accounting and C/C++ linkage. Stream and
header-string contracts use ordinary PIE executables and native spans, without
`-no-pie`, `-fno-pie` or glibc `mallopt` address accommodation. The stream/header
paths are enabled under ASan/UBSan rather than being excluded for high addresses.
The executor's separate LeakSanitizer limitation remains explicitly documented
in the refactor acceptance status; payload accounting is not a substitute for
claiming a LeakSanitizer run.
