#!/usr/bin/env python3
"""Extract a dependency archive and normalize an optional enclosing directory."""
import argparse
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('marker', help='Relative file identifying the dependency root')
    args = parser.parse_args()
    args.destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        if args.archive.suffix.lower() == '.zip':
            with zipfile.ZipFile(args.archive) as archive:
                archive.extractall(root)
        else:
            subprocess.run(['7z', 'x', str(args.archive.resolve()), f'-o{root}', '-y'], check=True)
        matches = [f for f in root.rglob(Path(args.marker).name)
                   if f.as_posix().endswith('/' + args.marker.replace('\\', '/'))]
        if len(matches) != 1:
            raise RuntimeError(f'Expected one {args.marker}, found {matches}')
        dependency = matches[0]
        for _ in Path(args.marker).parts:
            dependency = dependency.parent
        shutil.copytree(dependency, args.destination, dirs_exist_ok=True)
    print(f'Extracted {args.archive.name} to {args.destination}')


if __name__ == '__main__':
    main()
