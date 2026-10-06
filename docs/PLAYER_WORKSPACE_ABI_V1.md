# Player workspace ABI v1

This is a source-side capability. It is not active in the viewer until a real
Emscripten build from the reviewed source is synchronized with matching hashes,
source identity and manifest. No checked-in compiled artifact is relabelled.

## Contract

PLR/all builds report `playerWorkspaceAbiVersion: 1` in build identity and export
`terra_plr_workspace_abi_version() == 1`. The producer emits exactly
`abi.playerWorkspace = {version:1, fieldPatches:true, rollbackJournal:true}` only
after instantiating the actual Node and Web artifacts, checking both identity
markers, and verifying required exports. WLD-only builds report zero and do not
advertise this capability. Old manifests remain supported without activation.

- `terra_plr_open_*` creates a persistent document. The owner must close it.
- `terra_plr_get_keys(handle,pointer,output,capacity,required)` returns the keys of
  one object, with the usual NUL-inclusive JSON two-call result convention.
- Existing `terra_plr_get` reads only a requested field or panel subtree.
- Existing `terra_plr_set` and `terra_plr_set_many` retain replaced subtrees in an
  ordered rollback journal. They no longer clone the entire document. Validation
  still examines the complete candidate model, without producing JSON or binary.
- On malformed paths, validation failure or allocation failure, reverse journal
  restoration preserves every field, original bytes and serialization caches.
  Repeated, parent/child and root replacements keep their supplied order.
- `terra_plr_release_caches(handle)` discards reconstructible JSON/encoded caches;
  it never releases the document or original encrypted bytes.
- Future-version handles retain existing read-only and original-byte safeguards.

Structured legacy `apply_patch_json` retains its previous implementation; the
new viewer workspace uses the optimized field patch APIs instead.

## Viewer ownership and interaction

The native document is authoritative. JS keeps a lazy read-only field facade,
a 64 KiB field LRU, bounded panel projections, and old/new leaf diff history
(maximum 50 steps / 4 MiB). Large single fields can be read but do not enter the
cache. No borrowed Wasm view survives a synchronous native call. UTF-8 inputs
are size-checked and encoded into a newly allocated Wasm buffer.

Dirty state compares workspace state identity with the exported checkpoint.
Undo to that state is clean; a new branch has a different identity. Opening,
restoring and closing dispose the prior native owner only after a replacement
is valid. Failed replacement keeps the previous workspace and history. Export
still encodes and fully decodes the candidate to report conversion losses.
Future-version exports still preserve exact input bytes. Draft persistence is
coalesced after edits and forced on hide/unmount; a failed write retains the live
session. Busy file/edit operations and dirty workspaces block app updates.

The old compiled player artifact uses the compatible stateless backend. Its
native field-edit performance is not represented as optimized by this change.

## Verification

Run `python scripts/test-player-native.py`. It executes the existing codec
contract and the workspace contract: 300 seeded valid edits, 100 malformed or
invalid transaction attempts, every allocation-failure position in a mixed-field
patch, overlap ordering, binary reopen, stale handles and tracked heap return.

The differential executable emits 256 seeded batches, 256 rollback failures and
16 encrypted exports. Its 5,783,283-byte transcript was compared byte-for-byte
with the unmodified `src/terra_plr.c` from PR #31 / source base
`42d7297fd909415e686d99d5d1639c3a3a9bd919` before establishing the golden:
`cbd7b51b0e149e578a0b500ae45048ba3a2a421c5e4bc9b9074e6e81d1f529c2`.
The script checks that golden on subsequent runs.

`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python scripts/test-player-native.py --sanitize`
checks ASan/UBSan in ptrace environments where LeakSanitizer cannot run; tracked
allocation baseline assertions remain active. This is GCC/native evidence,
not Wasm ABI/device performance evidence. Real Emscripten artifact, H5 UI,
WeChat developer-tool and Android/iOS interaction/memory checks remain required.
