#!/usr/bin/env bash
# Prove the same-handle regression detects the audited stale-section readers.
# Run only after the fixed production Web semantics gate succeeds.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 /absolute/path/to/emsdk" >&2
  exit 2
fi
sdk=$(cd "$1" && pwd)
repo=$(git rev-parse --show-toplevel)
head=$(git -C "$repo" rev-parse HEAD)
old=87a3bc49b41aa85f679892d43cc020b15d80cfec
scratch=$(mktemp -d "${RUNNER_TEMP:-/tmp}/terrawasm-metadata-negative.XXXXXX")
worktree="$scratch/source"
added=0
cleanup() {
  if [[ $added -eq 1 ]]; then
    git -C "$repo" worktree remove --force "$worktree" || return 1
  fi
  rm -rf "$scratch"
}
trap cleanup EXIT

# Detached HEAD: no branch, commit, or remote write. Keep release build untouched.
git -C "$repo" worktree add --detach "$worktree" "$head"
added=1
git -C "$repo" show "$old:src/terra_wld.c" > "$worktree/src/terra_wld.c"
changed=$(git -C "$worktree" diff --name-only)
if [[ "$changed" != "src/terra_wld.c" ]]; then
  echo "negative control must change only the audited serializer source" >&2
  exit 1
fi
cmp "$repo/tests/test_pixel_art_semantics.js" "$worktree/tests/test_pixel_art_semantics.js"
# LFS fixture must be real WLD bytes, not an unresolved checkout pointer.
if head -c 128 "$worktree/tests/files/1.wld" | grep -aq 'version https://git-lfs.github.com/spec/v1'; then
  echo "negative-control WLD fixture was not hydrated" >&2
  exit 1
fi

# One isolated extra build, same SDK/profile, no full test suite and no upload.
# -AllowDirty is required because the deliberately old source is uncommitted.
pwsh -NoProfile -File "$worktree/build.ps1" -Target all -Features all -AllowDirty -EmsdkDir "$sdk"

(
  cd "$worktree"
  export PIXEL_ART_WEB_ARTIFACT="$worktree/build/terrax_world_wasm_web.js"
  node --test --test-reporter=tap --test-name-pattern '^replacement/fast:' \
    tests/test_pixel_art_semantics.js > "$scratch/replacement.tap" 2>&1
)
cat "$scratch/replacement.tap"

set +e
(
  cd "$worktree"
  export PIXEL_ART_WEB_ARTIFACT="$worktree/build/terrax_world_wasm_web.js"
  node --test --test-reporter=tap --test-name-pattern '^workspace/fast:' \
    tests/test_pixel_art_semantics.js > "$scratch/workspace.tap" 2>&1
)
status=$?
set -e
cat "$scratch/workspace.tap"
# A crash, build error, unrelated assertion, or unexpected pass is not evidence.
if [[ $status -ne 1 ]] ||
   ! grep -Eq '^not ok [0-9]+ - workspace/fast:' "$scratch/workspace.tap" ||
   ! grep -Fq 'same-handle signs after workspace commit must equal save/reopen' "$scratch/workspace.tap" ||
   ! grep -Fq "code: 'ERR_ASSERTION'" "$scratch/workspace.tap" ||
   ! grep -Eq '^# fail 1$' "$scratch/workspace.tap" ||
   ! grep -Eq '^# pass 0$' "$scratch/workspace.tap"; then
  echo "negative control did not fail at the expected stale-signs invariant" >&2
  exit 1
fi
echo "Negative control verified: replacement passes; original-handle workspace signs reject audited stale readers."
