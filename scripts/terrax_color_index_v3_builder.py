#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TerraX Color Index v3 builder.

Builds an exact full-RGB nearest-color index for Terraria pixel-art mapping.
It does NOT use the legacy quantized LUT format.

Output:
- .txci: compact block-compressed exact nearest-color index
- .raw.u16: optional raw 256^3 uint16 direct map for server-side memory mapping
- .manifest.json: artifact metadata and hashes

Dependencies:
    pip install numpy scipy
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import math
import os
import struct
import time
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import numpy as np
from scipy import ndimage

MAGIC = b"TXCI"
VERSION = 3
HEADER_STRUCT = "<4sHHIIIIIIIII"
HEADER_SIZE = struct.calcsize(HEADER_STRUCT)
ITEM_STRUCT = "<HHBB"  # kind/type id, variant, paint, flags
ITEM_SIZE = struct.calcsize(ITEM_STRUCT)
DIR_STRUCT = "<BBHI"   # block_type, count, reserved, payload_offset
DIR_SIZE = struct.calcsize(DIR_STRUCT)

KIND_TILE = 0
KIND_WALL = 1
FLAG_NONE = 0


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def gzip_size(path: Path, compresslevel: int = 9) -> int:
    data = path.read_bytes()
    return len(gzip.compress(data, compresslevel=compresslevel))


def pad4(buf: bytearray) -> None:
    while len(buf) % 4:
        buf.append(0)


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8-sig") as f:
        return json.load(f)


def parse_id_list(value: Optional[str]) -> Optional[set[int]]:
    if not value:
        return None
    p = Path(value)
    if p.exists():
        raw = json.loads(p.read_text(encoding="utf-8"))
        if isinstance(raw, dict):
            raw = raw.get("ids", raw.get("tile_ids", raw.get("wall_ids", [])))
        return {int(x) for x in raw}
    return {int(x.strip()) for x in value.split(",") if x.strip()}


def get_paint_colors() -> Dict[int, Tuple[int, int, int]]:
    # ID 0 is handled as "no paint" separately.
    return {
        1: (255, 0, 0),
        2: (255, 127, 0),
        3: (255, 255, 0),
        4: (127, 255, 0),
        5: (0, 255, 0),
        6: (0, 255, 127),
        7: (0, 255, 255),
        8: (0, 127, 255),
        9: (0, 0, 255),
        10: (127, 0, 255),
        11: (255, 0, 255),
        12: (255, 0, 127),
        13: (255, 0, 0),
        14: (255, 127, 0),
        15: (255, 255, 0),
        16: (127, 255, 0),
        17: (0, 255, 0),
        18: (0, 255, 127),
        19: (0, 255, 255),
        20: (0, 127, 255),
        21: (0, 0, 255),
        22: (127, 0, 255),
        23: (255, 0, 255),
        24: (255, 0, 127),
        25: (75, 75, 75),
        26: (255, 255, 255),
        27: (175, 175, 175),
        28: (255, 178, 125),
        29: (25, 25, 25),
        30: (200, 200, 200),
    }


def blend_map_color(
    base_color: Tuple[int, int, int],
    paint_color: Tuple[int, int, int],
    paint_id: int,
    is_wall: bool = False,
) -> Tuple[int, int, int]:
    """Terraria-style MapColor blend, matching the existing process_and_pack.py logic."""
    r, g, b = base_color
    r_norm = r / 255.0
    g_norm = g / 255.0
    b_norm = b / 255.0

    brightness = r_norm
    if g_norm > brightness:
        brightness = g_norm
    if b_norm > brightness:
        brightness, b_norm = b_norm, brightness

    if paint_id == 29:
        darkness = b_norm * 0.3
        return (
            int(paint_color[0] * darkness),
            int(paint_color[1] * darkness),
            int(paint_color[2] * darkness),
        )

    if paint_id == 30:
        if is_wall:
            return (
                int((255 - r) * 0.5),
                int((255 - g) * 0.5),
                int((255 - b) * 0.5),
            )
        return (255 - r, 255 - g, 255 - b)

    return (
        int(paint_color[0] * brightness),
        int(paint_color[1] * brightness),
        int(paint_color[2] * brightness),
    )


def generate_color_entries(
    colors_data: dict,
    tile_whitelist: Optional[set[int]] = None,
    wall_whitelist: Optional[set[int]] = None,
    variant_mode: str = "all",
    include_tiles: bool = True,
    include_walls: bool = True,
    include_painted: bool = True,
) -> List[Tuple[int, int, int, int, int, int, int]]:
    """
    Returns entries:
        (kind, type_id, variant, paint_id, r, g, b)
    kind: 0=tile, 1=wall
    """
    entries: List[Tuple[int, int, int, int, int, int, int]] = []
    paint_colors = get_paint_colors()

    def accept_variant(variant: int) -> bool:
        if variant_mode == "all":
            return True
        if variant_mode == "zero":
            return variant == 0
        raise ValueError(f"Unsupported variant_mode: {variant_mode}")

    if include_tiles:
        for item in colors_data.get("tiles", []):
            type_id = int(item["id"])
            variant = int(item.get("variant", 0))
            if tile_whitelist is not None and type_id not in tile_whitelist:
                continue
            if not accept_variant(variant):
                continue
            base = tuple(int(x) for x in item.get("rgb", [255, 255, 255]))
            entries.append((KIND_TILE, type_id, variant, 0, *base))
            if include_painted:
                for paint_id, paint_rgb in paint_colors.items():
                    rgb = blend_map_color(base, paint_rgb, paint_id, is_wall=False)
                    entries.append((KIND_TILE, type_id, variant, paint_id, *rgb))

    if include_walls:
        for item in colors_data.get("walls", []):
            type_id = int(item["id"])
            variant = int(item.get("variant", 0))
            if wall_whitelist is not None and type_id not in wall_whitelist:
                continue
            if not accept_variant(variant):
                continue
            base = tuple(int(x) for x in item.get("rgb", [255, 255, 255]))
            entries.append((KIND_WALL, type_id, variant, 0, *base))
            if include_painted:
                for paint_id, paint_rgb in paint_colors.items():
                    rgb = blend_map_color(base, paint_rgb, paint_id, is_wall=True)
                    entries.append((KIND_WALL, type_id, variant, paint_id, *rgb))

    return entries


def priority_key(entry: Tuple[int, int, int, int]) -> Tuple[int, int, int, int, int]:
    kind, type_id, variant, paint_id = entry
    # Default duplicate-color resolution:
    # tile before wall, unpainted before painted, lower type id, lower variant, lower paint id.
    return (kind, 1 if paint_id else 0, type_id, variant, paint_id)


def build_groups(
    entries: Sequence[Tuple[int, int, int, int, int, int, int]]
) -> Tuple[np.ndarray, List[List[Tuple[int, int, int, int]]]]:
    groups: Dict[Tuple[int, int, int], List[Tuple[int, int, int, int]]] = defaultdict(list)
    for kind, type_id, variant, paint_id, r, g, b in entries:
        groups[(r, g, b)].append((kind, type_id, variant, paint_id))

    # Sort by RGB for deterministic artifact identity.
    colors = sorted(groups.keys())
    options: List[List[Tuple[int, int, int, int]]] = []
    for rgb in colors:
        options.append(sorted(groups[rgb], key=priority_key))

    if len(colors) > 65535:
        raise ValueError(f"Too many unique RGB groups for uint16 direct map: {len(colors)}")

    return np.asarray(colors, dtype=np.uint8), options


def build_exact_direct_map(
    palette_rgb: np.ndarray,
    weights: Tuple[float, float, float] = (1.0, 1.0, 1.0),
) -> np.ndarray:
    """
    Exact nearest color group for every 24-bit RGB value.

    Uses scipy.ndimage.distance_transform_edt. Palette sites are background pixels in
    a 256x256x256 grid; return_indices gives the nearest palette coordinate for
    every RGB coordinate. sampling=sqrt(weights) supports weighted squared RGB.
    """
    if palette_rgb.ndim != 2 or palette_rgb.shape[1] != 3:
        raise ValueError("palette_rgb must have shape [N, 3]")

    grid = np.ones((256, 256, 256), dtype=bool)
    grid[
        palette_rgb[:, 0].astype(np.int16),
        palette_rgb[:, 1].astype(np.int16),
        palette_rgb[:, 2].astype(np.int16),
    ] = False

    sampling = tuple(math.sqrt(float(w)) for w in weights)
    indices = ndimage.distance_transform_edt(
        grid,
        sampling=sampling,
        return_distances=False,
        return_indices=True,
    )

    coord_to_group = np.full(256 * 256 * 256, 65535, dtype=np.uint16)
    codes = (
        (palette_rgb[:, 0].astype(np.uint32) << 16)
        | (palette_rgb[:, 1].astype(np.uint32) << 8)
        | palette_rgb[:, 2].astype(np.uint32)
    )
    coord_to_group[codes] = np.arange(len(palette_rgb), dtype=np.uint16)

    nearest_codes = (
        (indices[0].astype(np.uint32) << 16)
        | (indices[1].astype(np.uint32) << 8)
        | indices[2].astype(np.uint32)
    )
    direct = coord_to_group[nearest_codes.ravel()].reshape((256, 256, 256))
    if np.any(direct == 65535):
        raise RuntimeError("Direct map contains unresolved RGB cells")

    return direct.astype("<u2", copy=False)


def compress_blocks(direct: np.ndarray, brick_size: int = 8) -> Tuple[bytes, bytes, dict]:
    if brick_size not in (4, 8, 16, 32):
        raise ValueError("brick_size must be one of 4, 8, 16, 32")
    if direct.shape != (256, 256, 256):
        raise ValueError("direct map must have shape 256x256x256")

    directory = bytearray()
    payload = bytearray()
    type_counts = defaultdict(int)
    unique_counts: List[int] = []

    for r0 in range(0, 256, brick_size):
        for g0 in range(0, 256, brick_size):
            for b0 in range(0, 256, brick_size):
                block = direct[r0:r0+brick_size, g0:g0+brick_size, b0:b0+brick_size].ravel()
                uniq, inv = np.unique(block, return_inverse=True)
                k = int(len(uniq))
                unique_counts.append(k)

                offset = len(payload)
                if k == 1:
                    block_type = 0
                    count = 1
                    payload += struct.pack("<H", int(uniq[0]))
                    type_counts["uniform"] += 1
                elif k <= 16:
                    block_type = 1
                    count = k
                    payload += struct.pack("<B", k)
                    payload += np.asarray(uniq, dtype="<u2").tobytes()
                    inv_u8 = inv.astype(np.uint8)
                    if len(inv_u8) % 2:
                        inv_u8 = np.append(inv_u8, 0)
                    packed = (inv_u8[0::2] | (inv_u8[1::2] << 4)).astype(np.uint8)
                    payload += packed.tobytes()
                    type_counts["pal4"] += 1
                elif k <= 256:
                    block_type = 2
                    count = k if k < 256 else 0
                    payload += struct.pack("<H", k)
                    payload += np.asarray(uniq, dtype="<u2").tobytes()
                    payload += inv.astype(np.uint8).tobytes()
                    type_counts["pal8"] += 1
                else:
                    block_type = 3
                    count = 0
                    payload += np.asarray(block, dtype="<u2").tobytes()
                    type_counts["raw16"] += 1

                directory += struct.pack(DIR_STRUCT, block_type, count, 0, offset)

    stats = {
        "brick_size": brick_size,
        "brick_count": len(unique_counts),
        "block_types": dict(type_counts),
        "avg_unique_groups_per_brick": float(np.mean(unique_counts)),
        "max_unique_groups_per_brick": int(max(unique_counts)),
        "directory_bytes": len(directory),
        "payload_bytes": len(payload),
    }
    return bytes(directory), bytes(payload), stats


def build_txci_blob(
    palette_rgb: np.ndarray,
    options: Sequence[Sequence[Tuple[int, int, int, int]]],
    directory: bytes,
    payload: bytes,
    flags: int = 0,
) -> bytes:
    color_count = int(len(palette_rgb))
    item_count = int(sum(len(x) for x in options))
    brick_count = len(directory) // DIR_SIZE

    blob = bytearray(b"\x00" * HEADER_SIZE)

    colors_offset = len(blob)
    blob += palette_rgb.astype(np.uint8, copy=False).tobytes()
    pad4(blob)

    group_offsets_offset = len(blob)
    group_offsets = [0]
    item_bytes = bytearray()
    for group_options in options:
        for kind, type_id, variant, paint_id in group_options:
            packed_type = (0x8000 if kind == KIND_WALL else 0) | (int(type_id) & 0x7FFF)
            item_bytes += struct.pack(ITEM_STRUCT, packed_type, int(variant), int(paint_id), FLAG_NONE)
        group_offsets.append(len(item_bytes) // ITEM_SIZE)

    blob += np.asarray(group_offsets, dtype="<u4").tobytes()
    pad4(blob)

    items_offset = len(blob)
    blob += item_bytes
    pad4(blob)

    directory_offset = len(blob)
    blob += directory
    pad4(blob)

    payload_offset = len(blob)
    blob += payload

    header = struct.pack(
        HEADER_STRUCT,
        MAGIC,
        VERSION,
        8,  # overwritten by caller if needed via directory stats not included in header struct
        color_count,
        item_count,
        brick_count,
        colors_offset,
        group_offsets_offset,
        items_offset,
        directory_offset,
        payload_offset,
        flags,
    )
    blob[:HEADER_SIZE] = header
    return bytes(blob)


def patch_brick_size(blob: bytes, brick_size: int) -> bytes:
    b = bytearray(blob)
    struct.pack_into("<H", b, 6, brick_size)
    return bytes(b)


def write_artifacts(
    out_dir: Path,
    name: str,
    colors_data_path: Path,
    entries: Sequence[Tuple[int, int, int, int, int, int, int]],
    palette_rgb: np.ndarray,
    options: Sequence[Sequence[Tuple[int, int, int, int]]],
    direct: np.ndarray,
    brick_size: int,
    weights: Tuple[float, float, float],
    write_raw: bool,
) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    directory, payload, block_stats = compress_blocks(direct, brick_size=brick_size)
    blob = build_txci_blob(palette_rgb, options, directory, payload)
    blob = patch_brick_size(blob, brick_size)

    txci_path = out_dir / f"{name}.txci"
    txci_path.write_bytes(blob)

    raw_path = None
    if write_raw:
        raw_path = out_dir / f"{name}.raw.u16"
        raw_path.write_bytes(direct.astype("<u2", copy=False).tobytes())

    kinds = defaultdict(int)
    ids = {"tile": set(), "wall": set()}
    for kind, type_id, variant, paint_id, r, g, b in entries:
        if kind == KIND_TILE:
            kinds["tile_records"] += 1
            ids["tile"].add(type_id)
        else:
            kinds["wall_records"] += 1
            ids["wall"].add(type_id)

    manifest = {
        "format": "TXCI",
        "version": VERSION,
        "name": name,
        "source_file": str(colors_data_path),
        "source_sha256": sha256_file(colors_data_path),
        "generated_at_unix": int(time.time()),
        "metric": {
            "type": "weighted_rgb_squared" if weights != (1.0, 1.0, 1.0) else "rgb_squared",
            "weights": list(weights),
        },
        "entry_count": len(entries),
        "tile_type_count": len(ids["tile"]),
        "wall_type_count": len(ids["wall"]),
        "tile_record_count": kinds["tile_records"],
        "wall_record_count": kinds["wall_records"],
        "unique_rgb_group_count": int(len(palette_rgb)),
        "duplicate_option_count": int(sum(len(x) for x in options)),
        "brick_size": brick_size,
        "block_stats": block_stats,
        "artifacts": {
            "txci": {
                "path": txci_path.name,
                "bytes": txci_path.stat().st_size,
                "gzip_bytes": gzip_size(txci_path),
                "sha256": sha256_file(txci_path),
            }
        },
    }

    if raw_path is not None:
        manifest["artifacts"]["raw_u16"] = {
            "path": raw_path.name,
            "bytes": raw_path.stat().st_size,
            "gzip_bytes": gzip_size(raw_path),
            "sha256": sha256_file(raw_path),
        }

    manifest_path = out_dir / f"{name}.manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description="Build TerraX exact color index v3")
    parser.add_argument("--colors", required=True, type=Path, help="colors-145.json path")
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--name", default="terrax_color_index_v3")
    parser.add_argument("--variant-mode", choices=["all", "zero"], default="all")
    parser.add_argument("--tile-whitelist", default=None, help="Comma-separated IDs or JSON file")
    parser.add_argument("--wall-whitelist", default=None, help="Comma-separated IDs or JSON file")
    parser.add_argument("--no-tiles", action="store_true")
    parser.add_argument("--no-walls", action="store_true")
    parser.add_argument("--no-painted", action="store_true")
    parser.add_argument("--brick-size", type=int, default=8, choices=[4, 8, 16, 32])
    parser.add_argument("--weights", default="1,1,1", help="Weighted RGB squared weights, e.g. 1,1,1 or 0.3,0.59,0.11")
    parser.add_argument("--raw", action="store_true", help="Also write raw 256^3 uint16 map")
    args = parser.parse_args()

    weights = tuple(float(x.strip()) for x in args.weights.split(","))
    if len(weights) != 3 or any(w <= 0 for w in weights):
        raise ValueError("--weights must contain three positive numbers")

    colors_data = load_json(args.colors)
    entries = generate_color_entries(
        colors_data,
        tile_whitelist=parse_id_list(args.tile_whitelist),
        wall_whitelist=parse_id_list(args.wall_whitelist),
        variant_mode=args.variant_mode,
        include_tiles=not args.no_tiles,
        include_walls=not args.no_walls,
        include_painted=not args.no_painted,
    )

    print(f"[1/5] entries: {len(entries):,}")
    palette_rgb, options = build_groups(entries)
    print(f"[2/5] unique RGB groups: {len(palette_rgb):,}; options: {sum(len(x) for x in options):,}")

    print("[3/5] building exact 24-bit nearest-color field via EDT...")
    direct = build_exact_direct_map(palette_rgb, weights=weights)

    print("[4/5] compressing blocks and writing artifacts...")
    manifest = write_artifacts(
        args.out_dir,
        args.name,
        args.colors,
        entries,
        palette_rgb,
        options,
        direct,
        brick_size=args.brick_size,
        weights=weights,  # type: ignore[arg-type]
        write_raw=args.raw,
    )

    print("[5/5] done")
    print(json.dumps({
        "txci_bytes": manifest["artifacts"]["txci"]["bytes"],
        "txci_gzip_bytes": manifest["artifacts"]["txci"]["gzip_bytes"],
        "raw_bytes": manifest["artifacts"].get("raw_u16", {}).get("bytes"),
        "unique_rgb_group_count": manifest["unique_rgb_group_count"],
        "entry_count": manifest["entry_count"],
        "block_stats": manifest["block_stats"],
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
