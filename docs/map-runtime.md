# TMRT v1: installed Terraria MAP palette

`txw_set_map_runtime(ptr, len)` installs a validated, owned snapshot of the
game's `MapHelper` lookup and color palette. The caller allocates `ptr` using
the TerraWasm bridge allocator, writes the complete binary, calls the setter,
then may release the bridge buffer. The runtime stays installed across world
close and `tx_reset_heap()`. Passing `(0, 0)` clears it only after all worlds
and pending open/stream tasks have closed or been cancelled. An identical
snapshot is accepted while they exist; a different snapshot or clear is
rejected until they are no longer using the runtime. The native test entry
`txw_set_map_runtime_from_buffer(data, len)` has the same copy semantics.

`txw_use_builtin_map_runtime()` explicitly selects the compiled palette while
preserving open world handles and unsaved edits. It releases the installed TMRT
only when no open/stream task is pending. Worlds do not retain palette pointers;
subsequent MAP and PNG operations read the selected palette. Selecting an already
active compiled palette is idempotent. The original clear and replacement guards
still apply to the setter. An adapter must discard derived media and validate its
resource lease before and after asynchronous rendering when switching sources.
Every changed palette invalidates the world's TXCI, icon atlas and pending
two-call media/operation response caches. Pixel edits and world metadata remain.

All integers are unsigned little-endian. The file is at most 1 MiB and has no
padding or trailing bytes. The 96-byte header has 16 `u32` words at offsets
0..63:

| Offset | Field |
| --- | --- |
| 0 | magic bytes `TMRT` |
| 4 | schema `1` |
| 8 | total byte length |
| 12 | Terraria MAP file format `33083` |
| 16 | `curRelease` (nonzero; currently 326) |
| 20, 24 | tile and wall lookup counts |
| 28 | palette RGBA entry count |
| 32, 36, 40 | `tilePos`, `wallPos`, `liquidPos` |
| 44, 48, 52, 56 | `skyPos`, `dirtPos`, `rockPos`, `hellPos` |
| 60 | paint RGB count |

Bytes 64..95 contain the raw 32-byte release SHA-256. The adapter must bind
that digest to its validated release; this module rejects an all-zero digest.
From offset 96, there are `tileCount` tile entries then `wallCount` wall
entries. Each is `{mapIndex:u16, optionCount:u8, reserved:u8=0}`. An absent
type is `(0,0)`; present entries have 1..255 contiguous palette indices,
starting at `tilePos` for tiles and `wallPos` for walls. Next are
`paletteCount` raw RGBA quadruples, indexed by MAP value. Entries 1 onward
must be opaque. The final section is `paintCount` RGB triples, indexed by
paint ID minus 1. The total length must equal
`96 + 4*(tileCount+wallCount) + 4*paletteCount + 3*paintCount`.

The accepted v1 MAP layout has `tilePos=1`, contiguous tile and wall options,
4 liquids, then 256 sky, 256 dirt, 256 rock gradient entries and one hell
entry. `paintCount=30`. Tile/wall counts can grow up to 65535 without
recompiling. Map format 33083 is independent of `curRelease`; a changed MAP
format or incompatible layout requires a new adapter/runtime schema.
The MAP header options bitmap marks every type whose option count differs from
one, including absent types with count zero; those counts are written after
the two bitmaps. This matches the native 1.4.5.8 header byte for byte.

When installed, the same lookup drives MAP type values and header counts,
PNG tile/wall/liquid/background colors, paint RGB, and marker nearest-color
mapping. Missing tile types fall through to liquid, wall, then background;
they do not use old built-in, borrowed, or grayscale colors. Renderers treat
a tile/wall with zero options as background for every row of
an RLE run, preserving the sky/dirt/rock gradient across the run. Valid walls
and liquids remain foreground even when an active tile has no lookup. Liquids
use their unpainted palette color; a liquid type outside the four supported
types falls through to a wall and retains that wall's paint.
The existing
frame-to-variant rules remain game-version-specific; when they select a
variant beyond the installed option count, variant zero is used in both MAP
and PNG. The host adapter should validate its CDN manifest and objects before
assembling this buffer. Uninstalled runtimes retain the compiled legacy data.
