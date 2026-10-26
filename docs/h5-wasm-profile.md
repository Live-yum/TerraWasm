# H5-only Wasm memory profile

H5 can use a separately compiled Web profile with 16 MiB initial and 512 MiB
maximum linear memory. This is a compilation ceiling, **not** a reservation or
a granted device budget. The existing `memoryGrowthLimit` callback still clamps
every growth to the caller's remaining total-device budget. Allocations can fail
below the ceiling; callers must retain normal failure handling.

The default Mini Program Web build remains 16/160 MiB, Node remains 128/512 MiB,
and no native C code, ABI, source selection, TWLD behavior, or default CMake
memory setting changes. Mini Program's 200 MB total budget and native's 160 MiB
policy are unchanged consumer constraints.

## Build without replacing Mini Program artifacts

Keep the clean source checkout and its verified original `build/` artifacts.
Use a **new sibling output directory**, never the source's existing `build/`:

```sh
# CMake 3.27+ and Node must be available; CMAKE may select a specific executable.
scripts/build-h5-profile.sh /path/to/clean-source FULL_SOURCE_COMMIT \
  /path/to/new-h5-output /path/to/emsdk
```

The helper and `verify-memory-profiles.mjs` must be in the same directory. It
checks the requested commit against actual clean HEAD, requires original MP
artifacts from that same commit, rejects an existing output directory, records
`build-source.json`, and verifies actual CMake source-directory/commit settings.
Before creating any directory, canonical component-boundary checks reject output equal to or nested inside the source, SDK, or cache (including symlink aliases). Existing output paths are refused. It never checks out, resets or changes source files. Rebuilding from a later
commit must identify that later commit; never label changed source as an older
clean revision. Only use authorized, pinned toolchain dependencies. Before invoking any Emscripten executable or creating output, the helper reads version metadata (without running sanity checks) and accepts only Emscripten 5.0.7 plus the exact verified zlib port definition (SHA-256 `29e343eb83af6fa81d6e98ab09ec15261777822d1fde304376eead79b62eb751`). That definition fixes zlib 1.3.1, its original SHA-512, and the official GitHub URL; any SDK-version or port-definition change is refused, not treated as permission to download a replacement. This is a reproducibility guard, not a comprehensive SDK integrity/signature system. Canonical
EM_CONFIG and EM_CACHE paths avoid unnecessary SDK cache invalidation; normal
sanity checks remain enabled.

The actual native sources are shared with the original Node/Web build; the H5
Web profile only changes the explicit `TERRAX_WEB_MAXIMUM_MEMORY` argument.
CMake applies the existing BASIC runtime-history wrapper transformation after
linking. The producer then loads actual Node/Web modules to derive identity,
ABI, exports, flags, limits and SHA-256; no manifest-number editing or binary
patching is used.

Output is the usual producer contract under the new root's `build/`:

- `terrax_world_wasm_web.js`, `terrax_world_wasm_web.wasm`
- `terrax_world_wasm.js`, `terrax_world_wasm.wasm`
- `terra.manifest.json`, generated Node/Web export lists

The consumer must sync this root to a distinct H5 artifact destination and use
its corresponding manifest in every main/worker H5 loading path. Do not replace
Mini Program files or mix a wrapper, binary and manifest from different builds.

## Verification and provenance of the initial profile

The initial H5 profile was built from unchanged source commit
`15f2dfeffda9ae638800760fae2d7cc57c07e9d0`, the same native source as its original
MP/Node artifacts, using Emscripten 5.0.7. It was not built from the later
documentation-only successor while claiming an older commit. Two independent
build directories produced byte-identical wrappers, Wasm and manifest.

The included test directly decodes the Wasm memory section: H5 is 256/8192
pages and MP is 256/2560 pages. In isolated Node processes, a 170 MiB native
allocation succeeds for H5 under a 256 MiB caller grant and fails for MP despite
that same grant. A fresh Node child process additionally allocates exactly 300
MiB under a fixed 384 MiB caller grant, retaining an actual 315293696-byte linear
heap, directly proving growth beyond the old 200 MB total-policy threshold.
The exported `_tx_malloc` uses the C allocator but is accounted in the bridge
allocation domain: the test checks 300 MiB plus its 64 KiB sentinel in that
counter. Existing sentinel data and first/middle/last allocation bytes survive
growth and a rejected extra 64 MiB allocation after the grant is tightened.
Both native and bridge used counters become zero after free, while the linear
heap remains 315293696 bytes. No buffer-input or stream business guard changes
are implied by this known-budget module test. Tightened caller limits refuse extra growth without corrupting
existing data or allocation accounting. Freeing returns native/bridge used
counters to zero; it does **not** shrink linear memory (the H5 test retains
178978816 bytes). This is deterministic module validation, not a browser-device
OOM experiment or proof every device can afford that allocation.

Producer artifact/manifest/ABI validation, the 52 manifest/ABI/BASIC-history
contracts, four Web streaming/media/growth contracts, and artifact-size gates
passed for that H5 build. Existing stream media tests retained their 160 MiB
caller budget. Original MP/Node/manifest hashes remained unchanged, and the
newly compiled Node bytes matched the originals. Dedicated 160 MiB Node/WLD
native-size PNG, browser-device and native-platform suites were not run as
part of this producer profile verification; consumer entry-point checks are
separate evidence.

zlib is an existing dependency, not a TWLD or H5 addition: current PNG
checksums, MAP decompression, and TXCI gzip decoding still reference it. The
Emscripten 5.0.7 port pins official zlib 1.3.1 and verifies its SHA-512. Removing
that dependency is not a harmless way to avoid cache restoration.
