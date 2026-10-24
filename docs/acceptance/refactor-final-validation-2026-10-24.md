# Final producer validation, 2026-10-24

## Provenance

The consumer bundle was actually compiled from published refactor commit
`4d81e63cb4e8ca08c1c1b65b4c3a7977429b8928`, tree
`ba17c9b338361e6db278cd8347a617c965a9406b`, with Emscripten **5.0.7**.
The worktree was clean before configuration and after validation, and every
compiled build-info record and manifest reports `dirty: false`. All, WLD and
PLR were configured and built separately using the existing `build.ps1`
profile flags. All uses `-Oz` and LTO; WLD/PLR retain their published `-O3`
defaults and the existing compact WLD Web override.

Each retained artifact set contains the full manifest plus both Node and Web
wrapper/Wasm pairs. `scripts/verify-artifact-set.mjs` checks every declared byte
length and SHA-256 before executing a wrapper, then checks the actual compiled
identity, target, feature set, memory limits, capability markers, exports and
workspace ABI against the manifest and an explicit expected source SHA.
Negative tests reject a stale expected SHA, altered bytes, a missing pair and
contradictory declared capabilities. A Web profile is not required to add the
Node-only general ABI export merely to satisfy the verifier.

The [machine-readable record](refactor-final-validation-2026-10-24.json)
contains the actual hashes of all fifteen retained files, the unchanged size
limits, the original test summaries, and the CI run identities.

## Results

| Gate | Result |
| --- | --- |
| Linux native CTest | 56/56 |
| ASan + UBSan + LeakSanitizer CTest | 56/56 |
| All Node/Web suite | 242/242; zero failed, cancelled or skipped |
| WLD Node/Web suite | 218/218; zero failed, cancelled or skipped |
| PLR Node/Web suite | 69/69; zero failed, cancelled or skipped |
| Legacy, all and WLD | 38/38 for each |
| 8400×2400 MAP benchmark, all and WLD | Passed |
| World/player preview, applicable Node and Web profiles | Passed |
| Full artifact verification and original package budgets | Passed in every profile |
| Windows PLR file, allocator and native span contracts | 3/3 in actual Windows CI |

The additional native cleanup contract performs **16 consecutive cycles in
one process**, acquiring the pixel/workspace/READY resources first and then
refusing every later allocation attempt. Pixel rollback and owner close,
workspace rollback and commit, READY cancel and close must all reach their
documented terminal and return native/bridge payload to zero. Receipts and
other necessary caller capacity are acquired before pressure. This is native
cleanup coverage; it does not substitute for the consumer's transport and
publication outcome tests.

The PLR filesystem contract similarly keeps one player in one process for
16 Unicode-path cycles. It verifies short-write, corruption, truncation,
trailing-data, allocation and actual filesystem replacement failures, original
byte preservation and absence of abandoned candidates. Windows also exercises
a real open handle that denies delete sharing, followed by successful retry
only after that handle is closed. The Windows job exposed an actual MSVC C
alignment portability defect; the final commit fixes it rather than skipping
the job.

## CI and consumer delivery

- [Push run 40118470703](https://github.com/Live-yum/TerraWasm/actions/runs/40118470703):
  **7/7 jobs successful**, actually compiled source `4d81e63…`; the full five-file
  artifact is `terrawasm-4d81e63cb4e8ca08c1c1b65b4c3a7977429b8928`, artifact ID
  **12569768536**. Its reported all Web Wasm SHA-256 matches the delivered bytes.
- [PR run 40118473841](https://github.com/Live-yum/TerraWasm/actions/runs/40118473841):
  **7/7 jobs successful**. GitHub tested its temporary merge commit
  `5340169a542ed169b6611faa68798197e4d69322`; this independently verifies PR
  integration and is not mislabeled as the consumer bundle's source commit.
- Both runs include native, Windows, sanitizer/fuzz, all release, WLD and PLR
  matrix jobs, and the downloaded-artifact Circuit acceptance job. The latter
  uses the produced bundle, including the public large-world acceptance, rather
  than compiling a replacement binary in the downstream test job.
- [PR #44](https://github.com/Live-yum/TerraWasm/pull/44) targets `master` from
  `refactor`. No merge, force update or master write was performed.

The viewer sync command uses its existing verification path:

```sh
node scripts/sync-terrawasm-wld.mjs \
  --artifact-dir /path/to/verified/all-artifact-set \
  --source-commit 4d81e63cb4e8ca08c1c1b65b4c3a7977429b8928
npm run check:wasm-manifest
```

Both commands passed. The consumer receives the all Web pair, the full manifest
and its browser projection. The Node pair remains part of the producer's
verified bundle; it is not added to the Mini Program package.

| Delivered all artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| Node wrapper | 89,193 | `5c4d5e0068ea43ccc7e5813c39e04f229f13f140f46ed5b1564971350158fba5` |
| Node Wasm | 491,329 | `ba064d89a6e807bfb1e812e5325e5a26366482aaf1b9e5f1e955afe0a3d38367` |
| Web wrapper | 24,469 | `97b24e58cc8dd350268cb364596fd522c11701b0849d31dcfe830eea029a1c38` |
| Web Wasm | 474,863 | `07f00c021de9f4531bfa2cff537f3de95694f08c1c48186b961531e1be6e32f1` |

## Limits of this evidence

No existing package or cumulative-memory budget was increased. The original
native/Wasm suites retain their established test runner organization; the new
continuous native tests do not rebuild their owner between cycles. Consumer
same-realm/Worker continuity is a separate required gate in viewer-app.

Windows PLR local-file coverage does not establish Unicode behavior for all
other file APIs, remote-filesystem semantics, directory fsync or power-loss
durability. The candidate-validation contract does not promise a managed
publication receipt for an unknown filesystem outcome. Full private game
corpora, actual game-client loads and device physical-memory peaks require
their own environments. Browser ledger exhaustion is not physical device OOM.

Earlier 233-test and 54-contract reports remain historical checkpoints; this
record supersedes their producer counts and artifact identity only. It does
not declare every viewer work package, migration or platform acceptance gate
complete.
