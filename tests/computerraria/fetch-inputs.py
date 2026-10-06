#!/usr/bin/env python3
"""Fetch the pinned public acceptance inputs; never fetch a user's world."""
import argparse
import hashlib
from pathlib import Path
import shutil
import tarfile
import urllib.request

PIN = '0379d5b0d89dbb7fd4342b3afff9c3be5e1ab9d8'
BASE = f'https://raw.githubusercontent.com/misprit7/computerraria/{PIN}/'
HASHES = {
    'computerraria.tar.gz': '31423b4f7ebbecceeaa54f982f02980efa5b8edccede5456456d892b0452ea1a',
    'computerraria.twld': 'c6de694b3d034701513dc1ba17311213561ec359d3ecddde7bc35ea3c9611ed8',
    'computerraria.wld': '55d0a24bd1f56d622003dbd30d52555e7d06d6d1bcacfc22ae506f2db5240c33',
}


def verified(file):
    digest = hashlib.sha256()
    with file.open('rb') as source:
        for chunk in iter(lambda: source.read(1048576), b''):
            digest.update(chunk)
    if digest.hexdigest() != HASHES[file.name]:
        raise ValueError(f'Pinned fixture hash mismatch: {file.name}')


def download(directory, name, cache):
    target = directory / name
    if target.exists():
        verified(target)
        return target
    temporary = target.with_suffix(target.suffix + '.download')
    try:
        if cache:
            source = (cache / name).open('rb')
        else:
            source = urllib.request.urlopen(BASE + name, timeout=60)
        with source, temporary.open('wb') as output:
            shutil.copyfileobj(source, output, length=1048576)
        # Keep the intended filename in the hash lookup without trusting paths
        # supplied by the network or archive.
        digest = hashlib.sha256()
        with temporary.open('rb') as data:
            for chunk in iter(lambda: data.read(1048576), b''):
                digest.update(chunk)
        if digest.hexdigest() != HASHES[name]:
            raise ValueError(f'Pinned fixture hash mismatch: {name}')
        temporary.replace(target)
        return target
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--cache', type=Path, help='Read already downloaded public inputs instead of the network')
    parser.add_argument('--wld-only', action='store_true', help='Fetch only the original world, without companion mod data')
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    archive = download(args.directory, 'computerraria.tar.gz', args.cache)
    if not args.wld_only:
        download(args.directory, 'computerraria.twld', args.cache)
    world = args.directory / 'computerraria.wld'
    if not world.exists():
        temporary = args.directory / 'computerraria.wld.extracting'
        try:
            with tarfile.open(archive, 'r:gz') as bundle:
                matches = [entry for entry in bundle.getmembers() if entry.name in ('./computerraria.wld', 'computerraria.wld')]
                if len(matches) != 1 or not matches[0].isfile() or matches[0].size != 405983441:
                    raise ValueError('Published archive does not contain the expected full world')
                with bundle.extractfile(matches[0]) as source, temporary.open('wb') as output:
                    shutil.copyfileobj(source, output, length=1048576)
            temporary.replace(world)
        finally:
            temporary.unlink(missing_ok=True)
    verified(world)
    kind = 'WLD' if args.wld_only else 'WLD/TWLD'
    print(f'Verified pinned {kind} in {args.directory.resolve()}')


if __name__ == '__main__':
    main()
