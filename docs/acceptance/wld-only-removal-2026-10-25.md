# WLD-only production acceptance after TWLD retirement

Verified 2026-10-25 against clean producer `15f2dfeffda9ae638800760fae2d7cc57c07e9d0`. TWLD parsing, compatibility and sidecar storage are outside the product scope. This record does not enable or promise modded display compatibility.

## Removal and preserved contracts

The dedicated TWLD parser/header/session and production build entries were removed, along with exclusive sidecar state, custom color-pixel simulation, obsolete modded acceptance programs and sidecar download behavior. `streamingTwld` is explicitly false in both the manifest and compiled build identity.

The six-argument world-begin ABI, public structures/event/command values and ordinary phase numbers remain stable. Retired sidecar argument slots must be zero; nonzero values return `TCW_UNSUPPORTED` before resource acquisition/allocation. The new allocator-refusal contract checks this boundary. WLD SAVE does not create a sidecar, and READY's reserved word remains zero.

Preserved: normal WLD loading/saving/cancellation, original tile 445 PixelBox rules, raw-WLD 65534 routing contracts, shared zlib, and COB1 companion output via `aux_source_id`. The two reference implementations with obsolete sidecar guards are explicitly annotated narrow WLD-only adapters; their ordinary geometry/routing algorithms and assertions were retained. No production field was reintroduced merely to compile a retired feature test.

Two registered TWLD-specific C tests and one previously unregistered codec contract were retired together with the implementation; a zero-allocation unsupported-input contract was added. Remaining native tests are not a count-based substitute for the removed feature.

## Source and artifact verification

- Full native ASan+UBSan CTest: **55/55 passed**. `detect_leaks=0` was explicit because LSan fails under the execution environment's ptrace; this is not leak-sanitizer acceptance.
- Final clean actual Wasm, identity, manifest, tamper rejection, WLD/PLR, stream, owner/lease and low-memory contracts: **170/170 passed, zero skips**.
- All 101 native source/CMake hashes match the tested candidate; final worktree is clean.
- Exact clean Web/Node source identity, ABI/export set and original artifact budgets passed. Diagnostic dirty candidate artifacts were never delivered as clean artifacts; their expected strict-publication failures disappeared after the genuine clean build.

| Artifact | Bytes | SHA-256 | Size change from prior clean bundle |
| --- | ---: | --- | ---: |
| Web wrapper | 24469 | `97b24e58cc8dd350268cb364596fd522c11701b0849d31dcfe830eea029a1c38` | 0 |
| Web Wasm | 437466 | `78efb43f979082572f3bc8f1f560b2af74734f3752d81b551c739c9e465c6d12` | -37397 |
| Node wrapper | 89193 | `5c4d5e0068ea43ccc7e5813c39e04f229f13f140f46ed5b1564971350158fba5` | 0 |
| Node Wasm | 453929 | `1fe845baf4cc42dcd17a5b13e2e049eaf14b2e4c2d385ff377b120ff88549b06` | -37400 |

Manifest SHA-256: `20d3fbae1d86560b278dad029bddf21782983782b6e81f2df26229fa24233f02`. Original Web Wasm limit 524288 and warning 491520 are unchanged.

## Actual Computerraria WLD-only CPU and save/reopen

[Raw acceptance record](computerraria-wld-only-2026-10-25.json), produced by the unchanged `tests/test_computerraria_wld.js`, records a single final-artifact run: **3/3 passed, zero skips**, approximately 300 seconds. `TCW_WORLD` selected the actual pinned 405983441-byte WLD; `TCW_EXPECTED_COMMIT` selected the clean producer above. The original 160 MiB memory, 4096-clock and one-hour limits were not overridden. No TWLD input or source ID 3 was provided.

- Physical dimensions 15200x7200; 72939714 wired cells, 13641575 gates and 5121746 networks were imported, not replaced by a synthetic small circuit.
- 38 RV32I instruction kinds and 48 expected RAM words match the independent golden. The positive program finishes in 386 clocks.
- A deliberate ROM change changes the first RAM word from 0x80000000 to 0x7fffffff and preserves the other 47 expected words.
- The world was saved, closed, reopened and the CPU executed again, matching all 48 words in 386 clocks.
- Original PixelBox per-TripWire rules and 3072 display cells were checked through saving/reopening. The published mod world's screen remains off under original vanilla timing; this is explicitly not a modded/color-display compatibility claim and is not altered to force a desired image.
- Recorded Wasm heap 150536192 bytes and compiled peak 148438166 bytes remained below the original 167772160-byte ceiling. These are engine/linear-memory figures, not a physical-device process RSS measurement.
- Input SHA-256 before and after remains `55d0a24bd1f56d622003dbd30d52555e7d06d6d1bcacfc22ae506f2db5240c33`; temporary outputs were cleaned up. No source file or user world was overwritten.

## Remaining scope

These are native/Node-hosted Web Wasm results, not browser, WeChat-device, full viewer prepared/managed or hardware RSS acceptance. No main/master write, merge, automatic merge, release, deployment or manually started viewer Actions is part of this change. This documentation-only successor does not change the producer implementation or relabel the 15f2 artifact source identity.
