# Computerraria acceptance with original WLD-only wiring rules

`tests/test_computerraria_wld.js` imports the complete published Computerraria
WLD into the Web Wasm circuit engine, loads a real RV32I program into its physical
ROM lamps, pulses the hardware clock wire, and reads the physical RAM lamps.
It then saves a WLD and repeats the CPU acceptance after reopening that file.

The test always passes `twld_source_id = 0` and `twld_size = 0`. No TWLD file is
opened, and every imported circuit must report the original pixel profile. A
`TCW_TWLD` environment variable left over from another test is ignored. The
test does not install or execute WireHead, use the colored display, or change
native rules to reproduce the mod's display output.

The independent RV32I decoder in `rv32i-golden.mjs` is test-only. It checks the
hand-authored main program's expected values before the DUT runs; it never
supplies CPU registers, memory contents or framebuffer results to the circuit.

## Run

Build a WLD/Web or combined Web artifact, then run from the repository root:

```sh
TCW_WORLD=/absolute/path/computerraria.wld \
TCW_REPORT=/absolute/path/computerraria-wld-acceptance.json \
node --test tests/test_computerraria_wld.js
```

Use `TCW_BUILD_DIR` for a different build directory containing
`terrax_world_wasm_web.js` and `terrax_world_wasm_web.wasm`. The recorded Wasm
hash is computed from the exact binary buffer supplied to the instantiated DUT.
`TCW_MEMORY_MIB` defaults to 160, and input/output bridge chunks are at most
1 MiB. The 405-MB world is read through file descriptors instead of being loaded
into a JavaScript array. The compiler scratch file and new WLD outputs need a
writable temporary directory; `TMPDIR` selects it.

Without `TCW_WORLD`, the program-golden and coordinate tests run, while the full
circuit integration is explicitly skipped. Unit-only success is not CPU
acceptance. The full test's default one-hour deadline and 4,096 hardware-clock
budget are test bounds, not wire-count or component-count restrictions.

## Fixed source and world

| Input | Revision or SHA-256 |
| --- | --- |
| Computerraria source | `0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8` |
| Original wiring source | `8255d34616c780af12079425ac92a0a7aed87d71` |
| Complete published WLD | `55d0a24bd1f56d622003dbd30d52555e7d06d6d1bcacfc22ae506f2db5240c33` |
| Main RV32I ROM | `0aa1dc4d61f35f2024c36e847f53fc527f02dc2fc92051371432bc715027f2c1` |

The WLD is 405,983,441 bytes, format 279, with dimensions 15,200 × 7,200. Its
72,939,714 wired cells and 13,641,575 gates are imported in full. The harness
checks the world hash and those physical counts. Network counts are recorded
from the actual compiler; they are not fixed to a count from a different pixel
profile.

Primary references:

- [Published world archive](https://github.com/misprit7/computerraria/blob/0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8/computerraria.tar.gz)
- [Terraria TripWire, PixelBoxPass and LogicGatePass](https://github.com/Live-yum/TerrariaDecompiledSource/blob/8255d34616c780af12079425ac92a0a7aed87d71/Terraria/Wiring.cs)
- [Current ROM/RAM layout and address map](https://github.com/misprit7/computerraria/blob/0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8/doc/notes.txt)
- [Upstream hardware controls](https://github.com/misprit7/computerraria/blob/0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8/tinterface/tinterface/__init__.py)
- [Published monochrome memory-mapped driver](https://github.com/misprit7/computerraria/blob/0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8/app/tdriver/src/graphics.rs)

No downloaded world, generated scratch file or user-provided private world is
committed by this fixture.

## Assertions in the actual Wasm integration

The same instantiated Wasm module runs a small independently encoded WLD and the
complete Computerraria WLD. The small world gives an unambiguous check of the
original monochrome PixelBox rules before CPU acceptance begins:

| Physical input | Required original-rule result |
| --- | --- |
| Two faulty gates fire in the same wave, one into the pixel's horizontal red wire and one into its vertical blue wire | Pixel remains off: the two outputs are separate `TripWire` calls |
| One external `TripWire` reaches both axes | Pixel frame changes from 0 to 18 |
| Later horizontal and vertical pulses arrive in separate `TripWire` calls | Pixel remains at frame 18 |
| Another `TripWire` reaches both axes | Pixel frame changes from 18 to 0 |
| Save a lit pixel to WLD and reopen it | Pixel remains at frame 18 |

These are assertions against native cell results. The test does not write the
expected output frame into the DUT. The two real gate firings are also checked
using native counters.

The full world then executes the 924-byte main image. All 48 RAM words must match
the independently checked expected values, including completion marker
`0x600dc0de`. The program covers these 38 instruction kinds:

| Family | Instructions |
| --- | --- |
| Immediate operations | ADDI, SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI, SRAI |
| Register operations | ADD, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND |
| Upper immediates and control flow | LUI, AUIPC, JAL, JALR, BEQ, BNE, BLT, BGE, BLTU, BGEU |
| Memory operations | LB, LH, LW, LBU, LHU, SB, SH, SW |
| Ordering | FENCE `iorw, iorw` |

The negative control flips a bit in the actual ROM instruction encoding,
changing `ADDI x3,x1,1` to `ADDI x3,x1,0`. Its first physical RAM result must change
from `0x80000000` to `0x7fffffff`; the remaining 47 results must remain equal.

A separate 44-byte program performs real CPU stores of `0x80000001` to the
monochrome buffer at `0x20e000`, then writes `1` to its update register at
`0x20e1fc`. It uses no color-device addresses. Completion is checked in physical
RAM, and the resulting 3,072 native monochrome pixel frames are recorded. The
world is saved and reopened with no sidecar, all those frames must survive
unchanged, and the main CPU image must again produce all 48 correct signatures.

The hardware clock is the yellow wire at `(3194,153)`, mask `8`. ROM/RAM lamp
coordinates come from `mapping.mjs`. RAM readout first reaches the ready lamp at
`(3199,156)`, then pulses both zero-data-bus `(3243,226)` and zero-memory-select
`(3404,350)`, mask `2`, to remove their physical bus parity from the lamp state.
One clock input can be only part of a CPU instruction. The reference decoder's
241 dynamic instructions are not presented as a hardware retirement counter.

## Monochrome display compatibility

Original PixelBox behavior is verified by the source-rule fixture above. The
published Computerraria world was developed with a mod whose pixel topology and
gate-wave timing differ from original Terraria. The main-world display capture
is therefore reported as an observation; a mod-specific two-pixel expected
image is not substituted for original semantics.

In the actual WLD-only run, the 44-byte display program reaches its completion
marker, while all 3,072 monochrome screen pixels remain off. This establishes
that the CPU program runs; it does **not** establish that the published mod
world's display controller works under original rules. Its wiring/timing would
need a separate source-compatible redesign for that result. The native engine
was not changed to make that controller appear compatible.

## Scope

This is physical circuit and program acceptance, not a claim of full RISC-V
architectural certification. ECALL and EBREAK are probed as an explicit hardware
boundary: after 16 clock inputs they remain unready and their following marker
store has not executed. Resetting recovers normal main-program execution.
Privileged traps, CSR state, interrupts, misaligned accesses, compressed
instructions, atomics, multiplication/division extensions and `FENCE.I` are not
covered by the main image.

The training-dummy/hoik clock depends on NPC motion and is not simulated here;
the test injects the real circuit's hardware clock wire. Exact assignment of
shared RNG samples to arbitrary probabilistic faulty gates also remains outside
this compact compiler's verified scope. None of these boundaries is concealed
by a software CPU or a framebuffer emulator.

All full-world sessions must release their native owners to zero tracked bytes
when closed. The exported report includes actual source and output hashes,
signatures, work counters, native memory, pixel-rule results and elapsed times.

## Recorded clean-artifact run

The complete WLD-only test passed on 2026-10-06 with Node.js v24.19.0, using the
clean WLD/Web release artifact built from TerraWasm commit
`09c871d8bdbf22b87c45ed880ad7cd3cbff95ded`. The exact instantiated Web Wasm was
401,956 bytes with SHA-256
`0da7f96398a7eaed709ec3014ab8e8b0dd6a13c5c832080cd5405d95accec86d`.
All three tests passed, with no integration test skipped.

| Measurement or check | Actual result |
| --- | --- |
| Complete world imported | 405,983,441 bytes; 15,200 × 7,200 cells |
| Wired cells / physical gates | 72,939,714 / 13,641,575 |
| Networks in this original-rule import | 5,121,746 |
| Initial import | 21,823 ms |
| Native retained bytes after import | 126,651,802 |
| Native peak bytes | 145,674,922 |
| Wasm heap bytes | 167,051,264, within the configured 160 MiB |
| Main RV32I image | 924 bytes; 38 instruction kinds; all 48 RAM signatures match |
| Main execution | 725 ms; 386 hardware-clock inputs; 146,712 network pulses; 127,341 gate firings |
| ROM mutation negative control | First signature becomes `0x7fffffff`; remaining 47 signatures unchanged |
| Original PixelBox source-rule fixture | All separate/simultaneous `TripWire` cases pass; lit frame 18 survives WLD save/reopen |
| Published world's monochrome MMIO program | Completion marker reached; all 3,072 display pixels remain off under original rules |
| Saved WLD | 405,983,441 bytes; no sidecar |
| Saved-world import | 21,989 ms; all 3,072 observed monochrome frames preserved |
| Main image after reopen and reset recovery | All 48 RAM signatures match again |
| ECALL / EBREAK boundary | Each remains unready after 16 inputs, without executing the following marker store |
| Native ownership after close | Zero tracked bytes |
| Complete integration elapsed time | 58,351 ms |

The saved WLD's SHA-256 was
`76d75280686cf5abae74cfb3eb888ecb70aa15eba12c039bf4d64aeae90c8bc7`.
The report records reads from WLD and compiler scratch sources only; no TWLD
source was registered or read. Times above are observations from this execution,
including actual Wasm circuit work, and do not represent a device-independent
performance guarantee.
