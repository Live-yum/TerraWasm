#!/usr/bin/env python3
"""Emit anchor material permissions from the pinned TileObjectData initializer.

Usage: python3 scripts/generate-circuit-anchor-permissions.py PATH_TO_SOURCE
The caller still supplies each saved frame's exact anchor geometry. This table
only supplies which material predicates can satisfy that side, including platform
alternates and native substyles; it never invents a different anchor position.
"""
from copy import deepcopy
from hashlib import sha256
from pathlib import Path
import re
import sys

COMMIT = '8255d34616c780af12079425ac92a0a7aed87d71'
DIGEST = '348b09f93e78a2e704a137fc39e871bb7da5e5ad89519e9f1c0a2705cc5cf345'
path = Path(sys.argv[1])
if path.is_dir():
    path /= 'Terraria.ObjectData/TileObjectData.cs'
raw = path.read_bytes()
if sha256(raw).hexdigest() != DIGEST:
    raise ValueError(f'TileObjectData.cs is not the pinned {COMMIT} source')
source = raw.decode('utf-8-sig')
start = source.index('public static void Initialize()')
body = source[start:source.index('readOnlyData = true;', start)]
objects = {name: {'Width': 1, 'Height': 1} for name in ['newTile', 'newSubTile', 'newAlternate']}
bases, tiles, variables = {}, {}, {}
fields = 'Width|Height|StyleWrapLimit|AnchorTop|AnchorBottom|AnchorLeft|AnchorRight|FlattenAnchors'
tokens = re.compile(
    rf'(?P<set>(?P<obj>newTile|newSubTile|newAlternate)\.(?P<field>{fields})\s*=\s*(?P<value>.*?);)'
    r'|(?P<copy>(?P<dest>newTile|newSubTile|newAlternate)\.(?P<copy_type>CopyFrom|FullCopyFrom)\((?P<src>\w+)\);)'
    r'|(?P<base>addBaseTile\(out (?P<base_name>\w+)\);)'
    r'|(?P<tile>addTile\((?P<tile_id>\w+)\);)'
    r'|(?P<sub>addSubTile(?P<range>Range)?\((?P<styles>[^)]*)\);)'
    r'|(?P<alt>addAlternate\((?P<alt_style>\d+)\);)'
    r'|(?P<var>int (?P<var_name>\w+)\s*=\s*(?P<var_value>.*?);)', re.S)


def value_of(value):
    value = value.strip()
    if '+' in value and 'AnchorType.' not in value:
        return sum(value_of(part) for part in value.split('+'))
    if value in variables:
        return variables[value]
    if re.fullmatch(r'\d+', value):
        return int(value)
    if value in ['true', 'false']:
        return value == 'true'
    if value in ['AnchorData.Empty', 'default(AnchorData)']:
        return [[], 0, 0]
    if match := re.fullmatch(r'(newTile|newSubTile|newAlternate)\.(Width|Height|StyleWrapLimit)', value):
        return objects[match[1]][match[2]]
    if match := re.fullmatch(r'new AnchorData\(([^,]+),\s*([^,]+),\s*([^,]+)\)', value):
        return [re.findall(r'AnchorType\.(\w+)', match[1]), value_of(match[2]), value_of(match[3])]
    raise ValueError(f'Unrecognized anchor expression: {value}')


for token in tokens.finditer(body):
    if token['set']:
        objects[token['obj']][token['field']] = value_of(token['value'])
    elif token['copy']:
        name = token['src']
        copied = deepcopy(tiles[int(name)] if name.isdigit() else objects.get(name, bases.get(name)))
        if copied is None:
            raise ValueError(f'Unknown source {name}')
        if token['copy_type'] == 'CopyFrom':
            copied.pop('subtiles', None)
        # An alternate's own alternatives are not recursively visited by
        # CanPlace. Retaining only its anchor fields avoids duplicating lists.
        if token['dest'] == 'newAlternate':
            copied.pop('alternates', None)
        objects[token['dest']] = copied
    elif token['base']:
        bases[token['base_name']] = deepcopy(objects['newTile'])
        objects['newTile'] = {'Width': 1, 'Height': 1}
    elif token['tile']:
        ids = range(435, 440) if token['tile_id'] == 'j' else [int(token['tile_id'])]
        for tile_id in ids:
            tiles[tile_id] = deepcopy(objects['newTile'])
        objects['newTile'] = {'Width': 1, 'Height': 1}
    elif token['sub']:
        values = [value_of(s) for s in token['styles'].split(',')]
        styles = range(values[0], values[0] + values[1]) if token['range'] else values
        for style in styles:
            objects['newTile'].setdefault('subtiles', {})[style] = deepcopy(objects['newSubTile'])
        objects['newSubTile'] = {'Width': 1, 'Height': 1}
    elif token['alt']:
        objects['newTile'].setdefault('alternates', []).append(deepcopy(objects['newAlternate']))
        objects['newAlternate'] = {'Width': 1, 'Height': 1}
    elif token['var'] and token['var_name'] in ['num', 'num2', 'styleWrapLimit']:
        variables[token['var_name']] = value_of(token['var_value'])

names = [
    ('SolidTile', 'SOLID_TILE'), ('SolidWithTop', 'SOLID_WITH_TOP'),
    ('Table', 'TABLE'), ('SolidSide', 'SOLID_SIDE'), ('Platform', 'PLATFORM'),
    ('SolidBottom', 'SOLID_BOTTOM'), ('PlatformNonHammered', 'PLATFORM_NON_HAMMERED'),
    ('PlanterBox', 'PLANTER_BOX'), ('AlternateTile', 'ALTERNATE_TILE'),
    ('Tree', 'TREE'), ('EmptyTile', 'EMPTY_TILE'), ('AllFlatHeight', 'ALL_FLAT_HEIGHT'),
]
bits = {name: 1 << index for index, (name, _) in enumerate(names)}


def permissions(tile):
    result = 0
    for side, shift in [('AnchorBottom', 0), ('AnchorTop', 12)]:
        anchor = tile.get(side)
        if anchor and anchor[1]:
            for name in anchor[0]:
                if name != 'None':
                    result |= bits[name] << shift
    if tile.get('FlattenAnchors'):
        result |= 1 << 24
    return result


values = [0] * 754
for tile_id, tile in tiles.items():
    variants = [tile, *tile.get('subtiles', {}).values()]
    for variant in variants:
        values[tile_id] |= permissions(variant)
        for alternate in variant.get('alternates', []):
            values[tile_id] |= permissions(alternate)

assert len(tiles) == 389
assert values[33] & 4095 == bits['Table']
assert not values[21] & bits['Table']
assert values[132] & bits['Table']
assert values[42] >> 12 & bits['Platform']
assert values[136] & 4095 == bits['SolidTile'] | bits['SolidSide']
# The 754 tile IDs use only a small number of distinct permission masks.
# Keep constant-time lookup while sharing masks instead of storing 754 u32s.
dictionary = sorted(set(values))
assert len(dictionary) <= 256
indices = [dictionary.index(value) for value in values]
assert [dictionary[index] for index in indices] == values
lines = ['#ifndef TERRA_CIRCUIT_ANCHOR_PERMISSIONS_H', '#define TERRA_CIRCUIT_ANCHOR_PERMISSIONS_H',
    '/* Generated by scripts/generate-circuit-anchor-permissions.py.',
    ' * TileObjectData.cs source ' + COMMIT + '.', ' * SHA-256: ' + DIGEST + '.',
    ' * Per-side material predicates; saved-frame anchor geometry comes from the caller. */']
for index, (_, name) in enumerate(names):
    lines.append(f'#define CX_ANCHOR_{name} {1 << index}u')
lines.append(f'static const unsigned int cx_anchor_masks[{len(dictionary)}] = {{')
for at in range(0, len(dictionary), 8):
    lines.append('    ' + ','.join(f'{value}u' for value in dictionary[at:at + 8]) + ',')
lines += ['};', 'static const unsigned char cx_anchor_mask_index[754] = {']
for at in range(0, len(indices), 32):
    lines.append('    ' + ','.join(map(str, indices[at:at + 32])) + ',')
lines += ['};', 'static unsigned cx_anchor_permissions_for(unsigned type) {',
    '    return type < 754u ? cx_anchor_masks[cx_anchor_mask_index[type]] : 0;', '}',
    'static unsigned cx_anchor_bottom_type(unsigned type) {',
    '    return cx_anchor_permissions_for(type) & 4095u;', '}',
    'static unsigned cx_anchor_top_type(unsigned type) {',
    '    return (cx_anchor_permissions_for(type) >> 12) & 4095u;', '}',
    'static int cx_anchor_flattens(unsigned type) {',
    '    return (cx_anchor_permissions_for(type) & (1u << 24)) != 0;', '}', '#endif', '']
output = Path(__file__).resolve().parents[1] / 'include/terra_circuit_anchor_permissions.h'
output.write_text('\n'.join(lines))
print(f'Wrote {len(tiles)} pinned anchor permission entries ({len(dictionary)} shared masks, '
      f'{len(indices) + len(dictionary) * 4} table bytes) to {output}')
