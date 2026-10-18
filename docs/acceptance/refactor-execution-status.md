# Refactor handoff — 2026-10-18

This is an intentionally incomplete implementation handoff, not release approval.
The user stopped expansion after the current execution round. No PR, merge,
release or deployment was requested or performed. `master` was not changed.

## Inputs and provenance

Base viewer commit: `4e2cfcb7b6382321f7c95502a43feee64ec8dedd`.
Base Terra commit: `0a9e6dcb3c31148441b22d631c878ea7cf9d5fd2`.
Library source: `libfile_11cb1c5c49ac8191a171b48b91df50b8`,
`viewer-terrawasm-execution-kit-20261018(1).zip`.
The execution environment lacked Library prepare_materialize/read/search. It did
not receive or inspect the ZIP. The parent supplied verified text excerpts of
backlog.json, acceptance.json, contracts.ts, native outcome proposal, package
budget/auditor/collector material, and the reference review. These excerpts are
reference specifications, not proof of implementation or test success. The full
refactor-plan.md, every attachment file, and private map bytes were not locally
available. No personal map was committed.

## Work packages

| Package | Current result | Remaining boundary |
| --- | --- | --- |
| P00 | Partial: regression cases, contracts and status documentation | Full fixed native/RPC receipt contract baseline |
| P01 | Production single-envelope, principal fencing, safe read replay, transient retention | H5 IDB CAS/Web Lock refresh and device tests; H5 auto refresh remains off. Old unprovable three-key identity requires login |
| P02 | Production native layout/source-save guards with native regression | Current-source Wasm and full private-map validation |
| P03 | Production PLR same-directory exclusive candidate then rename | Platform durability/unknown outcomes/managed CAS and validation boundary |
| P04 | Internal typed pointers; tile response allocated before mutation | Complete no-allocation fixed receipt and outer response OOM |
| P05 | Command batch checkpoint atomicity; existing checkpoint explicitly rejected | Full owner guard and join support; receipt completeness |
| P06 | Production query errors no longer implicitly rollback | Full browser live/replay validation |
| P07 | Production pixel copy-before-admission fixed; retained in-flight byte cap | Complete transport failure/unknown ownership envelope |
| P08 | Production circuit admission/latest undo/baseline protections | Real browser/device validation |
| P09 | Selection and deferred page fencing integrated | All entry points consolidated into DocumentSession |
| P10 | Persisted operation revisions and exact version finalization | Durable transaction receipt end-to-end recovery |
| P11 | Reset management path and Worker invalidation; fail closed for dirty/unknown owner | Safe same-realm retirement/recreate |
| P12 | Partial production authority fence in store/H5 writes/recovery/destruction | All writer/GC/eviction paths and real cross-window lock-order proof; not a released A |
| P13 | Tested v3 repository kernel, not connected to live database | Migration/backfill and production integration; Release A actually released and v3-aware rollback REQUIRED |
| P14 | Repository CAS/receipt/exact draft finalization kernel | Full save orchestration and managed native adapter; production remains legacy |
| P15 | Worker/cooperative world/player exclusive ceilings and pending factory retention | Independent circuit pool lifecycle and complete cross-realm terminal receipts |
| P16 | Production composition root and pixel descriptor | Other namespace descriptors and complete consistency checks |
| P17 | Not completed | Public mutable writers remain |
| P18 | Production shared sparse history and deferred wire fill candidate | Other commands' chunking, all writer revisions, global backing accounting |
| P19 | Not enabled | Fusion full scan deliberately retained until writer closure |
| P20 | Production native binding hash index, allocation accounting, complexity test | Current-source Wasm/device performance |
| P21 | Production range digest and bounded Blob assembly; existing file transport | Backend idempotent chunk/finalize contract and total backing accounting. Existing upload limit retained |
| P22 | All Web core consumer/identity contract and CI wiring | New C source build -> sync -> H5/MP actual consumer validation |
| P23 | Handoff traceability only | Full cross-platform interruptions/release rehearsal NOT done |
| P24 | Scope revocation/drain/cancel/unknown outcome protections | Latest owner-reset tests blocked by dependency; fixed native receipt and real MP |
| P25 | Test discovery and required-artifact failure reporting | Successful full local build/CI; not all required gates executed |
| P26 | Engineering budgets and inventory checks integrated | Actual emitted build inventory and verified AppID compiled/platform caps |
| P27 | Explicit private fixture CMake entry | Private-map bytes not supplied, real corpus/device execution not done |

No package is declared to have passed all its cross-platform gates. G01–G52
remain required unless supported by the specifically scoped evidence below.

## Storage activation safety

`HISTORY_MIGRATION_ENABLED` is false and `assertHistoryMigrationEnabled` always
rejects. Live database version remains 2. The new repository requires an injected
already-open v3 database and has no production caller. Its IDB adapter tests are
not real multi-window browser acceptance. The compatibility fence uses the
`history-storage-authority-v1` control record and a short authority lock around
H5 mutations. Some legacy synchronous KV writers are not fully fenced across
windows; enabling migration would therefore be unsafe. No Release A or rollback
build has been released. Do not enable migration based on this branch.

## Executed validation

- Terra native CMake build succeeded, final CTest **31/31** after fetching the
  repository's existing LFS fixtures. Initial run was 29/31 with pointer files.
- Terra checkpoint normal + UBSan, PLR normal + ASan/UBSan, circuit actuation and
  index tests passed. Index 65,536 entries: 96,943 probes; OOM preserves index.
- Authentication/source helpers **39/39**; upload consumers **23/23** passed.
- World initial targeted regressions **26/26**; latest cancellation/reset test
  bodies did not run because `pako/index.js` was missing.
- Runtime expanded **56/58**; two dependency-load failures. Final composition
  and existing circuit Wasm tests **10/10** passed.
- Circuit focused **175/176**, one dependency-load failure; safety **11/11**.
  Earlier broad circuit/fusion run: **941 passed, 70 failed, 1 skipped**.
- Storage final **74/75**: one missing installed Uni transform dependency.
  The v3 repository tests use the existing in-memory IDB adapter, not a browser.
- Build-focused tests **9/9** passed. Their all Web binary is the existing
  producer `0ffa3e8d59ee5dc10a6bad3a51c133f1bda166bd`; it is NOT a rebuild of
  this branch's native changes.
- Architecture check passed. There is no repository lint script.
- Final typecheck failed before analysis (`tsc` unavailable). H5 and MP builds
  failed before compilation (`@dcloudio/vite-plugin-uni/bin/uni.js` unavailable).
  npm ci downloads did not finish; processes were stopped for handoff. No lockfile
  or credential changes were made to bypass dependency checks.
- Current-source Wasm build: not run (emcc/PowerShell unavailable).
- Real-map, real browser multi-window, WeChat SDK/AppID/device, backend and release
  gates: not run. No old result or proposal test substitutes for these gates.

Logs on this executor are under `/tmp`: `terra-native-validation.log`,
`terra-ctest.log`, `build-gates-tests.log`, `viewer-auth-validation.log`,
`viewer-upload-consumers.log`, `viewer-world-validation.log`,
`viewer-world-scope-validation.log`, `viewer-runtime-validation.log`,
`viewer-runtime-final-contracts.log`, `viewer-circuit-focused.log`,
`viewer-circuit-validation.log`, `viewer-storage-validation-final.log`,
`viewer-final-typecheck.log`, `viewer-final-build-h5.log`,
`viewer-final-build-mp.log`, `viewer-final-architecture.log`.
Remote SHA and CI are checked after commit/push and reported in the handoff.
