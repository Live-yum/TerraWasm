#!/usr/bin/env python3
"""Generate placement predicates from pinned Main.cs, without a second solid catalog."""
import hashlib
import re
import sys
from pathlib import Path

SOURCE_COMMIT = '8255d34616c780af12079425ac92a0a7aed87d71'
PINS = {
    'Main.cs': 'daae97ba9e9a11c457e40cb2001657b08d4611ee1ee6ba9e446bced606e7d5f5',
    'TileID.cs': '9e766249c1a4ea56bc10dd1019a0502243be12177defc50c7febb6e4c1615807',
    'WorldGen.cs': '8de656a227fe6d250438078e865be97bae1c1819d77a8a5cd5c1b651c6e6fef5',
    'Wiring.cs': 'c05fac30c1e1d13720a30be89c957b4ea5cc84be18e44e4a2c44ea532ab89832',
}

def read_pinned(path):
    source = path.read_bytes()
    if hashlib.sha256(source).hexdigest() != PINS[path.name]:
        raise ValueError(f'{path.name} is not the pinned {SOURCE_COMMIT} source')
    return source

source = read_pinned(Path(sys.argv[1]))
world_gen_source = read_pinned(Path(sys.argv[1]).with_name('WorldGen.cs'))
text = source.decode('utf-8-sig')
values = [0] * 754
for name, mask in [('tileTable', 1), ('tileSolidTop', 2), ('tileNoAttach', 4), ('tileMoss', 32)]:
    for tile, state in re.findall(r'\b' + name + r'\[(\d+)\]\s*=\s*(true|false)\s*;', text):
        tile = int(tile)
        if tile >= len(values):
            raise ValueError('source exceeds the pinned tile catalog')
        if state == 'true':
            values[tile] |= mask
        else:
            values[tile] &= ~mask
# Main initializes team-platform properties in a numeric loop, not literals.
for loop in re.finditer(r'for \(int (\w+) = (\d+); \1 <= (\d+); \1\+\+\)\s*\{([^{}]*)\}', text):
    variable, start, end, body = loop.groups()
    for name, mask in [('tileTable', 1), ('tileSolidTop', 2), ('tileNoAttach', 4)]:
        assignment = re.search(r'\b' + name + r'\[' + variable + r'\]\s*=\s*(true|false)\s*;', body)
        if assignment:
            for tile in range(int(start), int(end) + 1):
                if assignment.group(1) == 'true':
                    values[tile] |= mask
                else:
                    values[tile] &= ~mask
tile_id_source = read_pinned(Path(sys.argv[1]).parents[1].joinpath('Terraria.ID/TileID.cs'))
tile_id = tile_id_source.decode('utf-8-sig')
for name, mask in [('IsBeam', 8), ('Platforms', 16), ('Falling', 64)]:
    match = re.search(r'bool\[\] ' + name + r' = Factory.CreateBoolSet\(([^)]*)\)', tile_id)
    if not match:
        raise ValueError('missing pinned tile property ' + name)
    for tile in map(int, re.findall(r'\d+', match.group(1))):
        values[tile] |= mask
lines = ['#ifndef TERRA_CIRCUIT_SUPPORT_MATERIALS_H', '#define TERRA_CIRCUIT_SUPPORT_MATERIALS_H',
    '/* Generated from pinned Terraria source ' + SOURCE_COMMIT + '.',
    ' * Main.cs SHA-256: ' + hashlib.sha256(source).hexdigest() + '.',
    ' * TileID.cs SHA-256: ' + hashlib.sha256(tile_id_source).hexdigest() + '.',
    ' * WorldGen.cs SHA-256: ' + hashlib.sha256(world_gen_source).hexdigest() + ' */',
    '#define CX_SUPPORT_TABLE 1u', '#define CX_SUPPORT_SOLID_TOP 2u', '#define CX_SUPPORT_NO_ATTACH 4u',
    '#define CX_SUPPORT_BEAM 8u', '#define CX_SUPPORT_PLATFORM 16u', '#define CX_SUPPORT_MOSS 32u', '#define CX_SUPPORT_FALLING 64u',
    'static const unsigned char cx_support_materials[754] = {']
for at in range(0, len(values), 32):
    lines.append('    ' + ','.join(map(str, values[at:at+32])) + ',')
lines.extend(['};', 'static unsigned cx_support_properties(unsigned type) {',
    '    return type < sizeof(cx_support_materials) ? cx_support_materials[type] : 0;', '}',
    '/* WorldGen.GetDesiredStalagtiteStyle: small coral alone accepts coralstone. */',
    'static int cx_stalactite_material(unsigned type, unsigned height) {',
    '    if (cx_support_properties(type) & CX_SUPPORT_MOSS) return 1;',
    '    if (type == 225u) return height == 1u;',
    '    switch (type) {',
    '        case 1: case 200: case 164: case 163: case 117: case 402: case 403:',
    '        case 25: case 398: case 400: case 203: case 399: case 401:',
    '        case 396: case 397: case 367: case 368: case 147: case 161: return 1;',
    '        default: return 0;', '    }', '}', '#endif', ''])
Path('include/terra_circuit_support_materials.h').write_text('\n'.join(lines))

wiring_source = read_pinned(Path(sys.argv[1]).with_name('Wiring.cs'))
wiring = wiring_source.decode('utf-8-sig')
def method(name):
    start = wiring.index(' void ' + name + '(')
    opening = wiring.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (wiring[end] == '{') - (wiring[end] == '}')
        end += 1
    return wiring[opening:end]

hit = method('HitWireSingle')
mechanisms = set(map(int, re.findall(r'^\t\tcase (\d+):', hit, re.M)))
mechanisms.update(map(int, re.findall(r'(?<![.\w])type\s*==\s*(\d+)', hit)))
mechanisms.update(map(int, re.findall(r'\.type\s*(?:==|!=)\s*(\d+)', method('HitSwitch'))))
mechanisms.update(range(255, 269))
# The two TileID sets dispatched by HitWireSingle at the pinned version, plus
# directional routers and lamps/gates handled by HitWire/LogicGatePass.
mechanisms.update([4, 215, 419, 420, 424, 445])
packed = [0] * 95
for tile in mechanisms:
    packed[tile >> 3] |= 1 << (tile & 7)
lines = ['#ifndef TERRA_CIRCUIT_MECHANISM_TILES_H', '#define TERRA_CIRCUIT_MECHANISM_TILES_H',
    '/* Wiring.HitSwitch/HitWireSingle at ' + SOURCE_COMMIT + '.',
    ' * Wiring.cs SHA-256: ' + hashlib.sha256(wiring_source).hexdigest() + '.',
    ' * Natural decorations merely crossed by a wire are not circuit devices. */',
    'static const unsigned char cx_wire_object_bits[95] = {']
for at in range(0, len(packed), 24):
    lines.append('    ' + ','.join(map(str, packed[at:at+24])) + ',')
lines += ['};', 'static int cx_wire_object_type(unsigned type) {',
    '    return type < 754u && (cx_wire_object_bits[type >> 3] & (1u << (type & 7u)));', '}', '#endif', '']
Path('include/terra_circuit_mechanism_tiles.h').write_text('\n'.join(lines))
