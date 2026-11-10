#!/usr/bin/env python3
"""Copy FFmpeg dylibs into the bundle, make all references local, then sign."""
import argparse
import shutil
import subprocess
from pathlib import Path


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True, type=Path)
    parser.add_argument('--ffmpeg', required=True, type=Path)
    parser.add_argument('--identity', default='-')
    args = parser.parse_args()
    bundle = args.bundle.resolve()
    frameworks = bundle / 'Contents/Frameworks'
    frameworks.mkdir(exist_ok=True)
    libs = {}
    # Copy real files under their ABI name, e.g. libavcodec.62.dylib.
    for src in sorted((args.ffmpeg / 'lib').glob('*.dylib')):
        real = src.resolve()
        name = run('otool', '-D', str(real)).splitlines()[-1].rsplit('/', 1)[-1]
        if name not in libs:
            dest = frameworks / name
            shutil.copy2(real, dest)
            dest.chmod(0o755)
            libs[name] = dest
    if not libs:
        raise RuntimeError('No FFmpeg dylibs found')
    executable = bundle / 'Contents/MacOS/AlphaFix'
    for binary in [*libs.values(), executable]:
        changes = []
        # otool -L's first line is the filename; remaining lines are dependencies.
        for line in run('otool', '-L', str(binary)).splitlines()[1:]:
            dep = line.strip().split(' (', 1)[0]
            name = dep.rsplit('/', 1)[-1]
            if name in libs:
                relative = '@loader_path/' if binary != executable else '@loader_path/../Frameworks/'
                changes.extend(['-change', dep, relative + name])
            elif not dep.startswith(('/usr/lib/', '/System/Library/')):
                raise RuntimeError(f'Unbundled dependency in {binary}: {dep}. '
                                   'Use scripts/build-ffmpeg-mac.sh for a self-contained build.')
        if binary != executable:
            changes.extend(['-id', '@rpath/' + binary.name])
        if changes:
            subprocess.run(['install_name_tool', *changes, str(binary)], check=True)
    for binary in libs.values():
        subprocess.run(['codesign', '--force', '--sign', args.identity, str(binary)], check=True)
    subprocess.run(['codesign', '--force', '--sign', args.identity, str(bundle)], check=True)
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(bundle)], check=True)
    # Fail CI if any Homebrew/build-machine library path escaped rewriting.
    for binary in [*libs.values(), executable]:
        for line in run('otool', '-L', str(binary)).splitlines()[1:]:
            dep = line.strip().split(' (', 1)[0]
            if binary != executable and dep == '@rpath/' + binary.name:
                continue  # A dylib's own install ID is not an external dependency.
            if not dep.startswith(('@loader_path/', '/usr/lib/', '/System/Library/')):
                raise RuntimeError(f'Non-relocatable dependency: {dep}')


if __name__ == '__main__':
    main()
