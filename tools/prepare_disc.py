# Prepares your own Def Jam Vendetta (USA, SLUS-20639) disc for the recomp — the game's files never go in git.
#   disc/SLUS-20639.iso  a plain 2048-byte-sector image: the recompiled game reads the disc by raw sector number
#                        (sceCdRead — the ISO volume descriptor at sector 16, DATA.BIN by sector)
#   game/                every file of the disc; SLUS_206.39 is the executable the recompiler reads, and the runtime
#                        treats this folder as cdrom0: (IOP modules, file-name opens)
# usage: python tools/prepare_disc.py <disc.chd | disc.iso>
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'disc'))
SECTOR = 2048
SERIAL_FILE = 'SLUS_206.39'


class ChdSectors:
    """Sectors of a CHD (MAME) disc image, via the copied adjmk reader."""
    def __init__(self, path):
        from chdreader import CHD
        self.chd = CHD(path)
        self.frame = 2448
        self.fph = self.chd.frames_per_hunk
        self.total = self.chd.logicalbytes // self.frame

    def read(self, lba, count):
        out = bytearray()
        while count > 0:
            hunk = self.chd.read_hunk(lba // self.fph)
            fr = lba % self.fph
            while fr < self.fph and count > 0:
                out += hunk[fr * self.frame:fr * self.frame + SECTOR]
                fr += 1; lba += 1; count -= 1
        return bytes(out)


class IsoSectors:
    """Sectors of a plain .iso."""
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.total = os.path.getsize(path) // SECTOR

    def read(self, lba, count):
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)


def walk(src, lba, size, path=''):
    """Yield (path, lba, size, is_dir) for an ISO-9660 directory, recursively."""
    data = src.read(lba, (size + SECTOR - 1) // SECTOR)[:size]
    off = 0
    while off < len(data):
        rl = data[off]
        if rl == 0:
            off = (off // SECTOR + 1) * SECTOR
            continue
        e_lba = int.from_bytes(data[off + 2:off + 6], 'little')
        e_size = int.from_bytes(data[off + 10:off + 14], 'little')
        flags = data[off + 25]
        name = data[off + 33:off + 33 + data[off + 32]]
        off += rl
        if name in (b'\0', b'\1'):
            continue
        full = path + '/' + name.decode('latin1').split(';')[0]
        yield full, e_lba, e_size, bool(flags & 2)
        if flags & 2:
            yield from walk(src, e_lba, e_size, full)


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: python tools/prepare_disc.py <disc.chd | disc.iso>')
    path = sys.argv[1]
    src = ChdSectors(path) if path.lower().endswith('.chd') else IsoSectors(path)
    pvd = src.read(16, 1)
    if pvd[1:6] != b'CD001':
        sys.exit('not a PS2 disc image (no ISO-9660 volume descriptor at sector 16)')
    vol_blocks = int.from_bytes(pvd[80:84], 'little')
    root_lba = int.from_bytes(pvd[156 + 2:156 + 6], 'little')
    root_size = int.from_bytes(pvd[156 + 10:156 + 14], 'little')
    entries = list(walk(src, root_lba, root_size))
    if not any(p.upper() == '/' + SERIAL_FILE for p, _, _, d in entries if not d):
        sys.exit(f'{SERIAL_FILE} not on this disc — the recomp is for Def Jam Vendetta (USA, SLUS-20639) only')

    os.makedirs(os.path.join(ROOT, 'disc'), exist_ok=True)
    iso_out = os.path.join(ROOT, 'disc', 'SLUS-20639.iso')
    total = min(vol_blocks, src.total)
    if isinstance(src, IsoSectors) and os.path.abspath(path) != os.path.abspath(iso_out):
        shutil.copyfile(path, iso_out)
    elif isinstance(src, ChdSectors):
        with open(iso_out, 'wb') as f:
            step = 4096
            for lba in range(0, total, step):
                f.write(src.read(lba, min(step, total - lba)))
                print(f'\r{iso_out}: {min(lba + step, total)}/{total} sectors', end='', flush=True)
        print()
    print(f'disc image: {iso_out} ({total} sectors)')

    iso = IsoSectors(iso_out)
    game = os.path.join(ROOT, 'game')
    for full, lba, size, is_dir in walk(iso, root_lba, root_size):
        dst = os.path.join(game, full.lstrip('/'))
        if is_dir:
            os.makedirs(dst, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(dst) and os.path.getsize(dst) == size:
            continue
        with open(dst, 'wb') as f:
            left, cur = size, lba
            while left > 0:
                n = min(left, 4096 * SECTOR)
                f.write(iso.read(cur, (n + SECTOR - 1) // SECTOR)[:n])
                left -= n; cur += n // SECTOR
        print(full, size)
    print(f'game files: {game}')


if __name__ == '__main__':
    main()
