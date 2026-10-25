# Independent real-world stream and artifact verification

Verified 2026-10-25 against `21c541f42dda1c261d8785c2423edbb76cf71cc1` on `refactor`. This is a bounded producer-side verification record, not full refactor acceptance or permission to merge.

## Source and artifacts

The only change from `f3544e3df3450c01abc445cec87e32e702cc5880` to this basis is the prior independent-review document. Both commits have the same 312-entry non-document Git inventory: SHA-256 `22605c750eec096c44fe0cdef4ecfc7e1198cc0f60c3b2c749438f3a9b92162d`. The inventory is the newline-terminated `git ls-tree -r` output with `docs/` paths excluded. No producer implementation was changed for this verification.

Fresh Emscripten 5.0.7 all-profile artifacts embed source `21c541f…`, `dirty:false`. Four-file source identity, ABI, exports, memory and hash verification passed. The viewer's separately pinned `f3544e3…` bundle is not relabeled or replaced by this build.

| Fresh artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| Node wrapper | 89,193 | `5c4d5e0068ea43ccc7e5813c39e04f229f13f140f46ed5b1564971350158fba5` |
| Node Wasm | 491,329 | `dfd626a254ac1c027404ff2148aa302f6fcb96c77fc38e9ad8b9b9c9a71c4750` |
| Web wrapper | 24,469 | `97b24e58cc8dd350268cb364596fd522c11701b0849d31dcfe830eea029a1c38` |
| Web Wasm | 474,863 | `df7dd14bd148d6d96501b57fdf9fc0dc5643fff995b0a3521ea9ec41b4ed4167` |

Manifest SHA-256: `4e77b2fd444f4cea77a899e3428fdf357cec8dd855b058766845fb98010a67d9`.
Original Web budgets remain 131,072 wrapper bytes, 524,288 Wasm bytes, warning at 491,520 Wasm bytes; all passed without changed limits.

## Independent regression results

- Native Debug contracts excluding Circuit/fuzz: **36/36 passed**.
- Same native subset with ASan+UBSan: **36/36 passed**, explicitly `detect_leaks=0`. This is not LSan acceptance.
- Selected actual Web/Node Wasm contracts: **82/82 passed**, zero skips, failures or cancellations.
- The native acquired-cleanup contract performs 16 continuous iterations after real resource acquisition, denies new allocations during cleanup and asserts zero cleanup allocation attempts and final native/bridge payload.

The selected Wasm list and native build commands are unchanged from the [prior independent review](refactor-independent-review-2026-10-25.md), with the exact source basis above. These subset results do not represent full Circuit regression.

## Real input import and cleanup

The supplied CLI harness independently parses each binary header, then runs the **actual Web Wasm module under Node**, with file reads at most 1 MiB and a fixed 1 MiB transfer buffer. It never materializes a whole input file into a Buffer. The original 160 MiB Wasm cap is unchanged; additional test ceilings are 120 seconds and 1,000,000 steps.

Each input passed three repeated early cancel/close cycles, full stream scan, suspended-step replay, duplicate-supply rejection, adoption, independent header dimension/name comparison, and final task/world close.

| Input | Format | Dimensions | File bytes | Result |
| --- | ---: | --- | ---: | --- |
| Checked-in real `tests/files/1.wld` | 326 | 8400×2400 | 11,956,596 | Passed |
| Privately supplied real world | 326 | 8400×2400 | 12,178,361 | Passed; input and identifying metadata not published |
| Viewer checked-in large-world fixture | 319 | 8400×2400 | 11,987,858 | Passed |
| Pinned public Computerraria | 279 | 15200×7200 | 405,983,441 | Passed |

All four stayed at **16 MiB linear Wasm heap**, returned **zero native and bridge charged payload**, and used reads no larger than 1 MiB. These are not real-browser/device results or complete application RSS/process-budget evidence.

Public fixture SHA-256 values:
- `tests/files/1.wld`: `605413b86efa40aa158b4910bfe14270c05a580b4d0f93c73e825153f599320f`.
- Computerraria: `55d0a24bd1f56d622003dbd30d52555e7d06d6d1bcacfc22ae506f2db5240c33`, matching the [public upstream revision](https://github.com/misprit7/computerraria/tree/0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8) and `tests/computerraria/mapping.mjs`.

## Actual streamed save and reopen

The checked-in 8400×2400 `1.wld` and actual 405,983,441-byte Computerraria sample additionally passed:

1. Stream open and adopt.
2. Actual `save` operation, writing bounded output chunks to a temporary file.
3. Adopt the saved source, compare the complete header, and close.
4. Reopen the saved file through the streaming API, compare the complete header, and close.
5. Verify output length and SHA-256 exactly equal the original input; reread the original hash to prove it remained unchanged.
6. Release every tracked allocation and assert zero native/bridge charged payload; delete temporary output.

| Input | Output events | Largest read/write | Peak Wasm heap | Elapsed |
| --- | ---: | --- | --- | --- |
| `1.wld` | 15 | 1 MiB / 1 MiB | 16 MiB | ~0.26–0.33 s |
| Computerraria | 391 | 1 MiB / 1 MiB | 16 MiB | ~4.83–6.20 s |

The Computerraria round trip read 1,218,096,122 cumulative bytes across open/save/reopen/hash-independent operation phases. Its output size and hash matched the original exactly. Cumulative output writes exceed final length slightly because the stream rewrites output ranges; final file length and whole-file SHA were checked, not inferred from write counts. Neither input was edited, and no Circuit engine/CPU program was executed. Both final formatted CLI scripts were rerun on the public large-world and Computerraria fixtures; missing-argument and missing-input negative checks failed as expected without leaving temporary directories.

## Reproduce safely

Build the exact producer basis using the prior review's all-profile command, substituting `21c541f42dda1c261d8785c2423edbb76cf71cc1` for the source identity. Keep the scripts below outside that exact build checkout or account for their untracked presence; do not falsely declare modified source clean.

```sh
node scripts/verify-real-world-stream.cjs /path/to/exact-producer-checkout /path/to/real-world.wld
node scripts/verify-real-world-save.cjs /path/to/exact-producer-checkout /path/to/real-world.wld
```

Input files are opened read-only. Save tests create a unique temporary directory under the operating system temporary directory and remove their output afterward. Open file descriptors, stream/world handles and allocated pointers are guarded by cleanup even when setup or verification fails. Reserve input size plus 1 MiB disk space per active save. No script downloads, uploads or publishes data. Output JSON includes input hashes; keep private-input logs private. Neither script runs Circuit operations, alters a budget, changes source samples or requires new credentials.

## Still unverified

This does **not** establish full Circuit CPU/program behavior, prepared production-region acceptance, physical RAM display compatibility, real browser/device behavior, Windows, complete fuzz coverage, or game-client load compatibility. A clean byte-identical save/reopen is not proof of Circuit simulation correctness.

The corresponding Computerraria `.twld` input with SHA-256 `c6de694b3d034701513dc1ba17311213561ec359d3ecddde7bc35ea3c9611ed8` was not available in the local evidence set. Full mod metadata/display acceptance remains unrun. Existing security restrictions were not bypassed.
