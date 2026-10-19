# Prepares your own WWE SmackDown! Here Comes the Pain (USA, SLUS-20787) for the recomp: the game's files go into
# <data>/game/ (the runtime treats that folder as cdrom0:, and the recompiler reads SLUS_207.87 from it). Nothing from
# the disc ever goes in git or in a release.
# usage: python tools/hctp_prepare_disc.py [--check] <disc.iso | disc.chd | folder with SLUS_207.87>
#   --check: only says whether it is the right game (prints "ok SLUS-20787"), writes nothing
#   HCTP_DATA=<folder>: where game/ goes (default: the repository)
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prepare_disc import ChdSectors, IsoSectors, walk  # noqa: E402

ROOT = os.environ.get('HCTP_DATA') or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_FILE = 'SLUS_207.87'
WRONG = f'{SERIAL_FILE} not found - the recomp is for WWE SmackDown! Here Comes the Pain (USA, SLUS-20787) only'


def from_folder(src, check_only):
    names = {n.upper(): n for n in os.listdir(src)}
    if SERIAL_FILE not in names:
        sys.exit(WRONG)
    if check_only:
        print('ok SLUS-20787 folder')
        return
    game = os.path.join(ROOT, 'game')
    if os.path.abspath(src) == os.path.abspath(game):
        print('the game files are already in place')
        return
    files = [(r, f) for r, _, fs in os.walk(src) for f in fs]
    for i, (r, f) in enumerate(files):
        rel = os.path.relpath(os.path.join(r, f), src)
        dst = os.path.join(game, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not (os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(os.path.join(r, f))):
            shutil.copyfile(os.path.join(r, f), dst)
        print(f'{i + 1}/{len(files)} files', flush=True)
    print(f'game files: {game}')


def from_image(path, check_only):
    src = ChdSectors(path) if path.lower().endswith('.chd') else IsoSectors(path)
    pvd = src.read(16, 1)
    if pvd[1:6] != b'CD001':
        sys.exit('not a PS2 disc image (no ISO-9660 volume descriptor at sector 16)')
    root_lba = int.from_bytes(pvd[156 + 2:156 + 6], 'little')
    root_size = int.from_bytes(pvd[156 + 10:156 + 14], 'little')
    entries = list(walk(src, root_lba, root_size))
    if not any(p.upper() == '/' + SERIAL_FILE for p, _, _, d in entries if not d):
        sys.exit(WRONG)
    if check_only:
        print('ok SLUS-20787 image')
        return
    game = os.path.join(ROOT, 'game')
    files = [e for e in entries if not e[3]]
    for i, (full, lba, size, _) in enumerate(files):
        dst = os.path.join(game, full.lstrip('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not (os.path.exists(dst) and os.path.getsize(dst) == size):
            with open(dst, 'wb') as f:
                left, cur = size, lba
                while left > 0:
                    n = min(left, 4096 * 2048)
                    f.write(src.read(cur, (n + 2047) // 2048)[:n])
                    left -= n
                    cur += n // 2048
        print(f'{i + 1}/{len(files)} files', flush=True)
    print(f'game files: {game}')


def main():
    args = sys.argv[1:]
    check_only = bool(args) and args[0] == '--check'
    if check_only:
        args = args[1:]
    if len(args) != 1:
        sys.exit('usage: python tools/hctp_prepare_disc.py [--check] <disc.iso | disc.chd | game folder>')
    path = args[0]
    if os.path.isdir(path):
        from_folder(path, check_only)
    else:
        from_image(path, check_only)


if __name__ == '__main__':
    main()
