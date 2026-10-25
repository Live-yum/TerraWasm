# Independent producer review — 2026-10-25

## Exact basis and scope

Reviewed [PR #44](https://github.com/Live-yum/TerraWasm/pull/44), `refactor` →
`master`, at commit **`f3544e3df3450c01abc445cec87e32e702cc5880`**, tree
`a769ac7b2182b771b767a4e03de1ef97db367575`. This was an isolated clean checkout;
no producer implementation changes were needed for the independently reviewed
subset. No main-branch merge, release or deployment was performed.

| Independent local check | Result |
| --- | --- |
| Native Debug contracts excluding Circuit and fuzz | **36/36 passed** |
| Same subset with ASan + UBSan, leak detection disabled | **36/36 passed** |
| Selected actual all-profile Node/Web contracts | **82/82 passed**, zero failures/skips/cancellations |
| Clean exact-source all-profile Web/Node build | Passed, Emscripten **5.0.7**, existing `-Oz`/LTO defaults |
| Four-file manifest, compiled identity, ABI, export and hash verification | Passed |
| Original artifact budgets | Passed |
| LeakSanitizer-enabled local attempt | **Environment blocked**: runtime reports that LeakSanitizer does not work under ptrace |

The LSan-enabled attempt failed all 36 selected processes with the environment
error; it is **not** a local leak-free result. The subsequent 36/36 ASan+UBSan
result explicitly disables leak detection, without changing test assertions.

The acquired-cleanup contract passes 16 consecutive cycles in one allocator
process. It acquires real pixel/workspace/READY resources before rejecting new
allocations, checks that cleanup makes no further allocation attempts, and
returns native/persistent-native and bridge payload to zero. This is native
cleanup evidence, not a substitute for consumer transport, publication or
physical-memory acceptance. Linux PLR Unicode/emoji publication and candidate
failure cleanup also passed; Windows was not locally executed.

## Reproduction

Use the exact commit above, materialize its Git LFS fixtures, and ensure CMake,
a native C compiler, zlib development files, Node and Emscripten 5.0.7 are
available. All build directories below are ignored by the repository.

```sh
SOURCE=f3544e3df3450c01abc445cec87e32e702cc5880
# Run in an isolated checkout of $SOURCE with its LFS payloads present.
test "$(git rev-parse HEAD)" = "$SOURCE"
test -z "$(git status --porcelain)"

cmake -S . -B build-native-review \
  -DTERRAWASM_NATIVE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-native-review -j4
ctest --test-dir build-native-review -E 'circuit|fuzz' --output-on-failure

cmake -S . -B build-native-review-asan \
  -DTERRAWASM_NATIVE_TESTS=ON -DTERRAWASM_SANITIZERS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-native-review-asan -j4
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-native-review-asan -E 'circuit|fuzz' --output-on-failure

# Activate the existing Emscripten 5.0.7 SDK before configuring.
emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DTERRAX_BUILD_COMMIT="$SOURCE" -DTERRAX_BUILD_DIRTY=false \
  -DTERRAX_BUILD_COMPILER=emscripten -DTERRAWASM_FEATURE_SET=all \
  -DTERRAX_ENABLE_LTO=ON
cmake --build build -j4
test "$(git rev-parse HEAD)" = "$SOURCE"
test -z "$(git status --porcelain)"
node scripts/generate-manifest.mjs --root . \
  --output build/terra.manifest.json --source-commit "$SOURCE" --dirty false
node scripts/verify-artifact-set.mjs build "$SOURCE"
node scripts/check-artifact-size.mjs build/terra.manifest.json

node --test \
  tests/test_build_identity.js tests/test_manifest_contract.js \
  tests/test_artifact_size_contract.js tests/test_feature_set.js \
  tests/test_stream_abi_manifest.mjs tests/test_workspace_abi_manifest.mjs \
  tests/test_production_artifact_set.mjs tests/test_world_outcome.js \
  tests/test_pixel_foreign_checkpoint.js tests/test_header_json_abi.js \
  tests/test_foreign_checkpoint_owners.js tests/test_plr.js \
  tests/test_plr_real_fixture.js tests/test_stream_world.js \
  tests/test_stream_lease.js tests/test_stream_operations.js
```

The 82 tests include rejected stale source identities, altered artifacts,
missing files, contradictory capabilities and omitted Node/Web exports.
The four-file verifier checks bytes before loading wrappers, then compares
runtime compiled identities and declared capabilities against the expected
source and manifest. It does not establish the consumer's full application
acceptance.

## Fresh artifact identity

All four fresh artifacts report the exact `f3544e3…` source above and
`dirty: false`.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| Node wrapper | 89,193 | `5c4d5e0068ea43ccc7e5813c39e04f229f13f140f46ed5b1564971350158fba5` |
| Node Wasm | 491,329 | `e9bb512459a41e9e1750d99302d1e47c261bb5ae4c02c6667f4bddefea3c0103` |
| Web wrapper | 24,469 | `97b24e58cc8dd350268cb364596fd522c11701b0849d31dcfe830eea029a1c38` |
| Web Wasm | 474,863 | `e07ec4982f45439943efe29f14caee44f6e657ed480a3680b46f9d42ce8962ef` |

Full manifest SHA-256:
`8eda32b3446483d7c8c309976cf3f077cd20ef665ca26f4c2446977914aa0dea`.
The unchanged all-profile Web limits are **131,072 bytes wrapper** and
**524,288 bytes Wasm**, with a **491,520-byte Wasm warning** threshold.

The earlier [producer acceptance record](refactor-final-validation-2026-10-24.md)
pins **`4d81e63cb4e8ca08c1c1b65b4c3a7977429b8928`**. That remains the source
identity of that earlier bundle; its results must not be relabeled as `f3544e3`.
Subsequent changes through this review basis affect documentation and manifest
verification/tests, not C implementation or build-profile source. The fresh
wrappers match the prior wrapper hashes, while the Wasm files contain the new
source identity. This review does not silently replace or relabel consumer
artifacts.

## Separately observed CI and limits

The PR-triggered [quality run 40153150433](https://github.com/Live-yum/TerraWasm/actions/runs/40153150433),
returned for the reviewed head, was independently observed completed/success,
with **7/7 jobs successful**: native, sanitizer-and-fuzz, wasm-release, WLD/PLR
feature matrix, windows-plr-filesystem and circuit-world-acceptance. The
Windows job's Unicode-path/real-replacement-failure step succeeded. These are
CI observations, not locally rerun Windows or Circuit coverage. A PR-triggered
integration run must not be treated as proof that its downloaded artifacts
embed the branch-head identity without separately checking their manifest.

This independent local review deliberately did not execute Circuit/fuzz
contracts, prepared-Circuit or browser behavior, separate WLD/PLR rebuilds,
consumer H5/Mini Program full acceptance, real-device RSS, private game
corpora or game-client loads. Existing browser/prepared-Circuit restrictions
remain unchanged. Successful producer checks do not mean all viewer refactoring
is complete or authorize merging the PR.
