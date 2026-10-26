#!/usr/bin/env bash
# Rebuild from an unchanged TerraWasm checkout; never edit an existing artifact.
set -euo pipefail
if [[ $# != 4 ]]; then echo 'Usage: build-h5-profile.sh <clean-terrawasm-source> <source-commit> <new-h5-output-root> <emsdk-root>' >&2; exit 2; fi
SOURCE=$(realpath "$1"); EXPECTED_SOURCE=$2; OUTPUT=$(realpath -m "$3"); EMSDK=$(realpath "$4")
[[ "$EXPECTED_SOURCE" =~ ^[0-9a-f]{40}$ ]] || { echo 'Expected full lowercase source commit SHA' >&2; exit 2; }
[[ "$(git -C "$SOURCE" rev-parse HEAD)" == "$EXPECTED_SOURCE" ]] || { echo 'Unexpected source commit' >&2; exit 1; }
[[ -z "$(git -C "$SOURCE" status --porcelain --untracked-files=normal)" ]] || { echo 'Source is dirty: refuse a clean identity' >&2; exit 1; }
# Explicitly preflight this pinned dependency before starting a build. A missing
# cache is a blocker, not authorization to fetch a different/alternate source.
export EM_CONFIG="$EMSDK/.emscripten"
export EM_CACHE=$(realpath -m "${EM_CACHE:-$EMSDK/upstream/emscripten/cache}")
CACHE=$EM_CACHE
# Canonical component-boundary checks run before creating any output. Resolving
# existing symlinks also rejects aliases back into source/toolchain/cache trees.
for PROTECTED in "$SOURCE" "$EMSDK" "$CACHE"; do
    if [[ "$OUTPUT" == "$PROTECTED" || "$OUTPUT" == "${PROTECTED%/}/"* ]]; then
        echo "Output must be outside source, SDK and cache trees: $PROTECTED" >&2
        exit 1
    fi
done
# Inspect SDK metadata as data only: never execute emcc/config/port Python here.
# The exact port definition pins VERSION=1.3.1, the original SHA-512 and official
# https://github.com/madler/zlib/archive/refs/tags/v{VERSION}.tar.gz fetch. Reject
# even benign port edits rather than silently widening the verified dependency.
node - "$EMSDK" <<'JS'
const fs=require('node:fs'), path=require('node:path'), crypto=require('node:crypto');
const sdk=path.join(process.argv[2],'upstream/emscripten');
let version;
try { version=JSON.parse(fs.readFileSync(path.join(sdk,'emscripten-version.txt'),'utf8')); }
catch { throw new Error('Missing or invalid Emscripten version metadata'); }
if(version!=='5.0.7') throw new Error('Only verified Emscripten 5.0.7 is supported');
const port=fs.readFileSync(path.join(sdk,'tools/ports/zlib.py'));
const digest=crypto.createHash('sha256').update(port).digest('hex');
if(digest!=='29e343eb83af6fa81d6e98ab09ec15261777822d1fde304376eead79b62eb751') {
  throw new Error('Unverified zlib port definition: require original 1.3.1 version, SHA-512 and official URL');
}
JS
[[ ! -e "$OUTPUT" && ! -L "$OUTPUT" ]] || { echo 'Output exists: refuse to overwrite any artifact' >&2; exit 1; }
[[ -d "$CACHE/ports/zlib/zlib-1.3.1" ]] || { echo 'Missing pinned Emscripten zlib 1.3.1 cache; provision only with authorized dependency access.' >&2; exit 1; }
CMAKE=${CMAKE:-cmake}
mkdir -p "$OUTPUT"
node - "$SOURCE" "$EXPECTED_SOURCE" "$OUTPUT" "$EMSDK" <<'JS'
const fs=require('node:fs'), path=require('node:path'), cp=require('node:child_process');
const [source,sourceCommit,output,emsdk]=process.argv.slice(2);
const mp=JSON.parse(fs.readFileSync(path.join(source,'build/terra.manifest.json')));
if(mp.sourceCommit!==sourceCommit || mp.dirty!==false) throw new Error('MP artifact source must match clean source commit');
const actual=cp.execFileSync('git',['-C',source,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
if(actual!==sourceCommit) throw new Error('Source HEAD changed before configuration');
fs.writeFileSync(path.join(output,'build-source.json'),JSON.stringify({sourceDirectory:source,sourceCommit,dirty:false,toolchain:emsdk,outputDirectory:output,profile:'h5-web-16-512',mpManifestSourceCommit:mp.sourceCommit},null,2)+'\n');
JS
sha256sum "$SOURCE"/build/terrax_world_wasm{,_web}.{js,wasm} "$SOURCE/build/terra.manifest.json" > "$OUTPUT/mp-before.sha256"
"$CMAKE" -S "$SOURCE" -B "$OUTPUT/build" \
 -DCMAKE_TOOLCHAIN_FILE="$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake" \
 -DCMAKE_BUILD_TYPE=Release -DTERRAWASM_FEATURE_SET=all \
 -DTERRAX_BUILD_COMMIT="$EXPECTED_SOURCE" -DTERRAX_BUILD_DIRTY=false -DTERRAX_BUILD_COMPILER=emscripten \
 -DTERRAX_WEB_INITIAL_MEMORY=16777216 -DTERRAX_WEB_MAXIMUM_MEMORY=536870912 \
 -DTERRAX_NODE_INITIAL_MEMORY=134217728 -DTERRAX_NODE_MAXIMUM_MEMORY=536870912 \
 -DTERRAX_OPTIMIZE_FLAG=-Oz -DTERRAX_ENABLE_LTO=ON
# CMake's generated cache independently records the source directory used.
grep -Fx "CMAKE_HOME_DIRECTORY:INTERNAL=$SOURCE" "$OUTPUT/build/CMakeCache.txt"
grep -Fx "TERRAX_BUILD_COMMIT:STRING=$EXPECTED_SOURCE" "$OUTPUT/build/CMakeCache.txt"
"$CMAKE" --build "$OUTPUT/build" --parallel 4
# The producer derives all limits, flags and identities by loading actual modules.
node "$SOURCE/scripts/generate-manifest.mjs" --root "$OUTPUT" --source-commit "$EXPECTED_SOURCE" --dirty false
node "$SOURCE/scripts/verify-artifact-set.mjs" "$OUTPUT/build" "$EXPECTED_SOURCE"
node "$(dirname "$0")/verify-memory-profiles.mjs" "$SOURCE/build" "$OUTPUT/build"
sha256sum -c "$OUTPUT/mp-before.sha256"
[[ -z "$(git -C "$SOURCE" status --porcelain --untracked-files=normal)" ]]
