#!/usr/bin/env python3
"""
build_txci.py -- Build TXCI v3 color index from TerrariaServerHook output.

Usage:
    python scripts/build_txci.py [--colors PATH] [--out-dir PATH] [--name NAME]

Default:
    --colors  data/extracted/colors.generated.json
    --out-dir data
    --name    terraria_color_index

Prerequisites:
    pip install numpy scipy

The colors.generated.json file is produced by TerrariaServerHook's
metadata extraction (--extract-metadata). It contains tiles[] and walls[]
arrays with {id, variant, rgb} entries.
"""
import argparse
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="Build TXCI v3 color index")
    parser.add_argument(
        "--colors",
        type=Path,
        default=Path("data/extracted/colors.generated.json"),
        help="Path to colors.generated.json from TerrariaServerHook",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("data"),
        help="Output directory for .txci file",
    )
    parser.add_argument(
        "--name",
        default="terraria_color_index",
        help="Output file name (without extension)",
    )
    parser.add_argument(
        "--brick-size",
        type=int,
        default=8,
        choices=[4, 8, 16, 32],
        help="Brick size for TXCI compression (default: 8)",
    )
    parser.add_argument(
        "--variant-mode",
        choices=["all", "zero"],
        default="all",
        help="Variant mode: 'all' includes all variants, 'zero' only variant 0",
    )
    args = parser.parse_args()

    # Resolve paths relative to project root (parent of scripts/)
    project_root = Path(__file__).resolve().parent.parent
    colors_path = (project_root / args.colors).resolve()
    out_dir = (project_root / args.out_dir).resolve()

    if not colors_path.exists():
        print(f"Error: colors file not found: {colors_path}")
        print("Run TerrariaServerHook --extract-metadata first.")
        sys.exit(1)

    out_dir.mkdir(parents=True, exist_ok=True)

    # Find the builder script
    builder = Path(__file__).resolve().parent / "terrax_color_index_v3_builder.py"
    if not builder.exists():
        print(f"Error: builder script not found: {builder}")
        print("Copy terrax_color_index_v3_builder.py to scripts/")
        sys.exit(1)

    print(f"Colors:  {colors_path}")
    print(f"Output:  {out_dir}")
    print(f"Name:    {args.name}")
    print(f"Brick:   {args.brick_size}")
    print(f"Variant: {args.variant_mode}")
    print()

    cmd = [
        sys.executable,
        str(builder),
        "--colors", str(colors_path),
        "--out-dir", str(out_dir),
        "--name", args.name,
        "--variant-mode", args.variant_mode,
        "--brick-size", str(args.brick_size),
    ]

    result = subprocess.run(cmd, cwd=str(project_root))
    if result.returncode != 0:
        print(f"\nError: TXCI builder failed with exit code {result.returncode}")
        sys.exit(result.returncode)

    txci_path = out_dir / f"{args.name}.txci"
    if txci_path.exists():
        size_mb = txci_path.stat().st_size / (1024 * 1024)
        print(f"\nSuccess: {txci_path} ({size_mb:.2f} MB)")
    else:
        print(f"\nWarning: expected output not found at {txci_path}")


if __name__ == "__main__":
    main()
