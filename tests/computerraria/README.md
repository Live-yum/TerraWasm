# Computerraria circuit acceptance

This fixture runs the imported **physical wiring graph in the Web Wasm module**.
The host streams files, changes ordinary logic-lamp frames in the physical ROM,
pulses the hardware clock wire, and reads physical RAM lamps. No CPU instruction
decoder, register file, or memory-mapped I/O emulator is used by the DUT.

`rv32i-golden.mjs` is a separate test-only instruction decoder. It checks the
hand-authored expected signatures before the circuit test runs; it never writes
DUT state or supplies a result to a native gate.

## Pinned inputs

| Input | Pin / SHA-256 |
| --- | --- |
| Computerraria source | `0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8` |
| WireHead source used to reproduce its circuit/display semantics | `e6009d010ca54ff43d04b44697accc7115807b9c` |
| Terraria source used for wiring order | `8255d34616c780af12079425ac92a0a7aed87d71` |
| `computerraria.tar.gz` | `31423b4f7ebbecceeaa54f982f02980efa5b8edccede5456456d892b0452ea1a` |
| `computerraria.wld` | `55d0a24bd1f56d622003dbd30d52555e7d06d6d1bcacfc22ae506f2db5240c33` |
| `computerraria.twld` | `c6de694b3d034701513dc1ba17311213561ec359d3ecddde7bc35ea3c9611ed8` |

The published WLD is 405,983,441 bytes, format 279, and measures 15,200 × 7,200
tiles. It contains 72,939,714 wired cells and 13,641,575 logic gates. The sidecar
is 427,712 gzip bytes but expands to 437,852,040 bytes; the native importer reads
it in two streaming passes without retaining that expanded array. It restores
16,896 `WireHead/ColorPixelBox` tiles, as well as preserving the other mod tags.
The standalone codec also identifies 121 training-dummy and four logic-sensor
entities. Identifying an entity does not simulate NPC physics.

Relevant upstream source:

- [Published world archive](https://github.com/misprit7/computerraria/blob/0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8/computerraria.tar.gz)
- [Published TWLD](https://github.com/misprit7/computerraria/blob/0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8/computerraria.twld)
- [ROM loader and traversal](https://github.com/misprit7/WireHead/blob/e6009d010ca54ff43d04b44697accc7115807b9c/Commands/LoadCommand.cs)
- [Hardware controls and reset sequence](https://github.com/misprit7/computerraria/blob/0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8/tinterface/tinterface/__init__.py)
- [RAM/ROM overhaul, microprograms, and address map](https://github.com/misprit7/computerraria/blob/0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8/doc/notes.txt)
- [Display and input driver](https://github.com/misprit7/computerraria/blob/0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8/app/tdriver/src/graphics.rs)
- [Terraria wiring](https://github.com/Live-yum/TerrariaDecompiledSource/blob/8255d34616c780af12079425ac92a0a7aed87d71/Terraria/Wiring.cs)
- [Terraria support and tile-removal predicates](https://github.com/Live-yum/TerrariaDecompiledSource/blob/8255d34616c780af12079425ac92a0a7aed87d71/Terraria/WorldGen.cs)
- [Terraria material and furniture sets](https://github.com/Live-yum/TerrariaDecompiledSource/blob/8255d34616c780af12079425ac92a0a7aed87d71/Terraria.ID/TileID.cs)
- [WireHead gate-wave and pixel boundaries](https://github.com/misprit7/WireHead/blob/e6009d010ca54ff43d04b44697accc7115807b9c/WiringWrapper.cs)
- [WireHead pixel grouping, repeated pulse parity, and color bits](https://github.com/misprit7/WireHead/blob/e6009d010ca54ff43d04b44697accc7115807b9c/Accelerator.cs)

The test requires the exact published hashes. It does not accept a cropped world,
an invented small CPU, or a user-provided private world as a replacement for this
public acceptance fixture. Downloaded worlds and temporary compiler files are
not committed to the repository.

## Run

Build the WLD or combined Web profile normally, then run from the repository root:

```sh
TCW_WORLD=/absolute/path/computerraria.wld \
TCW_TWLD=/absolute/path/computerraria.twld \
TCW_REPORT=/absolute/path/computerraria-acceptance.json \
node --test tests/test_computerraria.js
```

`TCW_BUILD_DIR` selects another directory containing
`terrax_world_wasm_web.js` and `terrax_world_wasm_web.wasm`. `TCW_MEMORY_MIB`
defaults to 160, matching the existing Web linear-memory ceiling. The host input
window is at most 1 MiB. A writable temporary directory is required for compiler
scratch and new WLD/TWLD output files. `TMPDIR` selects it;
`TCW_KEEP_SCRATCH=1` retains it for debugging. Source files are opened read-only.

The default timeout is one hour and the default program clock budget is 4,096.
These are **test execution bounds**, not world-size, wire-count, or gate-count
limits. `TCW_TIMEOUT_MS` and `TCW_CLOCK_BUDGET` can change them. The production
executor yields while work remains and has no 100,000-wire / 20,000-component
policy cutoff.

Without `TCW_WORLD`, the two fixture-unit tests run and the actual circuit test
is explicitly reported as **skipped**. A passing unit-only invocation does not
establish CPU acceptance. The integration report includes source and Wasm
hashes, graph counts, real native net/gate counters, physical signatures,
memory use, display outputs, saved-file hashes, and reimport results.

## Physical layout and controls

All positions below are tile coordinates. Wire masks are red `1`, blue `2`, green
`4`, and yellow `8`.

| Signal | Position | Mask | Original tile / frame |
| --- | --- | --- | --- |
| Hardware clock | `(3194,153)` | `8` | Empty cell with yellow wire |
| Reset execution latch | `(3198,156)` | `4` | Lever `132`, frame `(54,18)` |
| Ready / `inexec` indicator | `(3199,156)` | `4` | Ordinary lamp `419`, frame `(18,0)` |
| Zero data bus (`zdb`) | `(3243,226)` | `2` | Lever `132`, frame `(18,18)` |
| Zero memory selection (`zmem`) | `(3404,350)` | `2` | Lever `132`, frame `(0,18)` |
| Store PC (`lpc`) | `(3198,198)` | `8` | Lever `132`, frame `(18,0)` |

The named `inexec` lamp is on at an instruction boundary. To reset safely, pulse
the hardware clock up to three times until it is on; use the reset lever if it
does not become ready. Then pulse `zdb`, `zmem`, and `lpc`, in that order. This is
the upstream test driver's sequence and preserves RAM and general registers.
Programs in this fixture initialize all the registers they use.

One hardware clock pulse is not one RV32I instruction. The physical CPU uses
multiple microprogram steps for some instructions. The test advances 128 clock
inputs at a time, then reaches the next instruction boundary before inspecting
RAM. **Both `zdb` and `zmem` must be pulsed before reading the RAM lamps**: their
raw frames otherwise include the current common-bus parity. This is a physical
readout requirement, not a correction applied to the expected signatures.

ROM is 768 KiB at address zero. For a word address `a`, let
`word = a / 4` and `cell = word % 8192`. Bit `b`, where bit zero is the least
significant bit, is at:

```text
x = 2853 + cell + floor((cell + 1) / 2)
y = 1143 + floor(word / 8192) * 131 + (31 - b) * 3
```

This is the current WireHead loader's `2853+1g1ax8192g0x1` and
`1143+1g3x32g131x24` traversal. The first word's low bit is `(2853,1236)`.

RAM is 368 KiB at `0x100000` through `0x15BFFF`. Let
`word = (a - 0x100000) / 4`. Each bit has two mirrored lamps:

```text
x = 2853 + (word % 4096) * 3 + mirror     (mirror is 0 or 1)
y = 4287 + floor(word / 4096) * 125 + (31 - b) * 3
```

Both mirrors are patched when loading RAM. The first low bit is `(2853,4380)`;
the last word's mirrored low bit is `(15139,7130)`. The 125-row RAM bank pitch is
different from the 131-row ROM pitch. The older `tinterface` 96-KiB layout is
obsolete for this published world and is not used by the harness.

The 176 × 96 colored display occupies `(7371,1002)` through `(7546,1097)`.
Its memory begins at `0x200000`; writing `1` to `0x203FFC` updates the screen.
The 64 × 48 monochrome display occupies `(6485,800)` through `(6548,847)`.
Its memory begins at `0x20E000`, its update register is `0x20E1FC`, and its input
register is `0x20E1F8`. The upstream input bits are down, left, right, and up in
bits zero through three respectively.

## Program coverage and limits

The main image is 924 bytes, with SHA-256
`0aa1dc4d61f35f2024c36e847f53fc527f02dc2fc92051371432bc715027f2c1`.
It contains 231 static instruction words. The independent decoder follows 241
dynamic instructions before the halt loop and verifies 48 RAM words, ending in
`0x600dc0de`.

Covered instruction kinds:

| Family | Instructions / observations |
| --- | --- |
| Immediate ALU | ADDI, SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI, SRAI |
| Register ALU | ADD, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND |
| Upper immediates and PC | LUI, AUIPC, JAL, JALR including odd target bit clearing |
| Branches | BEQ, BNE, BLT, BGE, BLTU, BGEU; taken and not taken; backward loop |
| Memory | LB, LH, LW, LBU, LHU, SB, SH, SW; sign extension and untouched byte lanes |
| Ordering | FENCE `iorw, iorw`, observed between a store and subsequent load |
| Additional checks | x0 immutability, signed overflow, signed/unsigned comparisons, shift masking |

The negative control changes one bit in the **actual loaded ROM instruction**:
the first tested `ADDI x3,x1,1` becomes `ADDI x3,x1,0`. The first physical RAM
signature must change from `0x80000000` to `0x7FFFFFFF`; the other 47 must remain
equal. Passing the golden decoder alone cannot satisfy this assertion.

`io-program.mjs` is a separate 72-byte ROM that uses real RV32I stores to write a
packed color word and a monochrome word, then triggers their hardware update
registers. The harness reads native viewport cells, saves new `.wld` and `.twld`
files, reimports them, checks all display frames, executes a second ROM that
clears the screens, and reruns the main RV32I image after reimport.
Before the successful save, it also cancels a real paired save after partial
TWLD gzip output, verifies that the live display state is intact, discards the
two staged files, and saves again through the same native session.

This is circuit/program acceptance, not RISC-V architectural certification.
The published CPU has no SYSTEM-opcode microprogram, privileged CSR/trap bank,
or execution environment for ECALL/EBREAK. The harness probes those instructions
as an explicit unsupported boundary and then verifies that resetting recovers
normal program execution. It does not implement a JavaScript or native software
trap handler to conceal that hardware boundary. Misaligned loads/stores,
privileged instructions, interrupts, compressed instructions, atomics, multiply
and divide extensions, and `FENCE.I` are outside this program's coverage.

The automatic training-dummy/hoik clock relies on NPC movement and is separate
from the hardware clock wire used here. This acceptance does not claim complete
Terraria NPC simulation. All CPU and display effects exercised by the program
come from the imported native wiring graph and native pixel rules; installing
or executing the WireHead mod is not required.

### Original and WireHead pixel rules

The two sources have a material display difference. Original Terraria evaluates
pixel intersections at the end of **each individual TripWire**. The pinned
WireHead wrapper disables that pixel pass and retains its triggered-group list
until the end of the **whole current gate wave**. Its ordinary monochrome pixel
also connects a wire color in all directions and pairs different colors;
ColorPixelBox keeps separate horizontal and vertical networks of the same color.
Repeated network hits in a wave contribute by parity. A same-color loop that
connects both ColorPixelBox axes to one group is not a valid two-group pair.

The native importer enables this specific compatibility profile when the TWLD
identifies `WireHead/ColorPixelBox`. Ordinary worlds keep the original per-TripWire
rules. This source-backed distinction is required for Computerraria's display
controller: using vanilla pixel aggregation with the mod's circuit produces no
screen output even though the CPU completes its stores. Neither profile changes
the CPU instruction set or substitutes a software display controller.

### Ordering boundary of the compact compiler

The compact compiler preserves the gate-wave boundary and canonical physical
gate identity. It emits the candidates of each net in physical gate order. It
does **not** reconstruct the original breadth-first lamp visitation order for
every possible entry coordinate. Exact assignment of successive `Main.rand`
samples to multiple probabilistic faulty gates on the same net is therefore not
supported. The deterministic session sampler also cannot restore Terraria's
unsaved, shared gameplay RNG state from a WLD file.

This distinction has a concrete counterexample in
`probability_assignment_requires_source_order` in the native VM contract: a
wire entered from the right reaches the right faulty gate before the left one.
With one on lamp out of two and a sample tape `[0,1]`, reversing those candidates
reverses which physical gate fires. The VM preserves the order supplied to it;
the compact net compiler's physical ordering is not the same as seed-specific
Terraria FIFO. The main CPU and display acceptance use deterministic circuit
paths and do not establish arbitrary probabilistic-gameplay equivalence.

## Measured end-to-end acceptance

[acceptance-result.json](acceptance-result.json) contains the actual successful
run, including the full physical signatures. Its Wasm SHA-256 is
`3e5a966dd8cbc5169603a4949860c7f64c97d3d31638684a4853946119246a10`.
These measurements describe that tested artifact and execution environment;
device timings and future artifacts must be measured independently.

| Check | Observed result |
| --- | --- |
| Full WLD/TWLD import | 22,116 ms; all 109,440,000 world cells represented |
| Compiled graph in WireHead pixel profile | 72,939,714 wire cells; 13,641,575 gates; 5,116,686 nets |
| Main physical CPU program | All 48 signatures match; completion observed by 386 hardware clock inputs |
| Main-program native work | 146,712 net pulses; 127,341 gate firings |
| Main ROM loading, run, and signature read | 799 ms |
| Changed-ROM negative control | First result becomes `0x7FFFFFFF`; other 47 remain equal |
| Colored display | Eight pixels `(7371..7378,1002)` become state 15 / frame `(54,54)` |
| Monochrome display | `(6485,800)` and `(6516,800)` become frame `(18,0)` |
| Interrupted paired save | Cancel after reading 4,096 TWLD bytes and writing 10 gzip bytes; all live display frames remain intact; retry succeeds |
| Paired save | New WLD 405,983,441 bytes; new TWLD 427,751 bytes |
| Save/reimport | Every one of the 16,896 color and 3,072 mono display frames is preserved |
| Post-import display program | Real CPU stores clear both screens back to zero |
| ECALL and EBREAK probes | Each stays unready after 16 clock inputs; subsequent completion store is not executed |
| Recovery after SYSTEM probes | Reset followed by the main ROM again matches all 48 signatures |
| Peak native session allocation | 148,797,093 bytes |
| Web Wasm linear memory | 167,051,264 bytes, below the 160-MiB build ceiling |
| Full test time | 71,310 ms, including interrupted save, retry and full reimport |
| Native owners after each full-world close | 0 tracked bytes |

The clock count is a batch observation, not a claim that 386 is the minimum
cycle count. The 241 retired instructions are the independent reference trace's
count before its halt loop, not a software counter injected into the physical
CPU. The native runtime's reported work is net pulses and gate firings.

## Native transaction and codec checks

`tests/circuit_vm_contract.c` checks gate waves, repeated pulses, canonical
physical identity, all-color completion before evaluation, feedback smoke,
per-TripWire pixel intersections, operation-sized yields, callback rollback,
outer multi-pulse batch rollback, budget changes, 4,096-gate waves, and
deterministic allocation failures. It also checks that a gate-output TripWire
exposes its canonical physical gate identity and that cancellation clears that
identity, allowing the owner to apply the source's seed `SkipWire` rule.
Counters in the standalone allocator return
to zero after every case.

`tests/circuit_twld_unit.c` checks one-byte gzip fragments, metadata after tile
data, framed and unframed unknown mod tiles, entity extraction, lossless unknown
NBT, a fixed-size pixel-frame edit, changed-source replay rejection, truncation,
invalid lengths, budgets, and allocation failure cleanup.
Its replay checks cancel after a color-frame patch has begun, discard the
partial gzip, and replay successfully; budget and allocation admission failures
remain retryable. A changed gzip source is still rejected against the original
metadata pass's CRC after abort/restart.

`tests/circuit_twld_contract.c` streams the actual 437-MB-expanded sidecar. An
independent byte comparison of a one-pixel frame edit found exactly two changed
uncompressed bytes, with all other NBT preserved. The codec's measured peak for
the real metadata/read/write replay was 507,408 bytes. These native contracts
are useful on their own but do not replace the full Wasm circuit test above.

`tests/circuit_actuation_contract.c` checks the applicable `Wiring.DeActive`
and `WorldGen.CanKillTile` predicates against small source-derived fixtures:
solid/NotReallySolid/excluded materials, the Plantera/underground temple rule,
furniture support, tree/palm/cactus frames, and the two- or three-cell boulder
and teleporter support neighborhoods. Missing sparse neighborhood data is an
explicit unsupported result, never permission to deactivate. The tests preserve
the original empty-above short circuit and the distinction between `active`
and `inActive`; these affect protected walls and furniture. These predicates
do not implement item drops, furniture destruction or NPC movement.

The immutable material table records the exact source and input hashes. To
regenerate it after reviewing those pinned inputs:

```sh
python3 scripts/generate-circuit-materials.py \
  --catalog /path/to/viewer-app/features/circuit/domain/actuation-data.mjs \
  --tile-id /path/to/TerrariaDecompiledSource/Terraria.ID/TileID.cs \
  --out include/terra_circuit_materials.h
```

The generated table is committed; building or running Wasm does not require
the companion application's source tree or a network download.
