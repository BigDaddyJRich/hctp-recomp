"""Oct 28: Snuka Titantron (LoW 122.bik) + DX fix (SvR07 97.bik). Old DX row (video 224 -> host 7 45) was Anita's
tron: keep that file as a new 'Anita' row, point 224 at the real DX tron, add 'Jimmy Snuka' row + his default (0x46).
Trons = MPEG-1 256x256 59.94 fps, no audio (same as host 7 40-57). Backs up every file it changes."""
import hashlib, re, shutil, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, r'F:\WWE SmackDown! vs Raw 2011\Mod Maker Tools')
sys.path.insert(0, r'F:\hctp-project\tools')
import hctp_entrance as H
from build_imported_media import pac_write

GAME = Path(r'F:\HCTP MODS\Vanilla')
BACKUP = GAME / 'MODS' / 'backup_snuka_dx_trons_1028'
LIST = GAME / 'MODS' / 'PAC' / 'MOVE' / 'LIST'
SNUKA = Path(r'F:\HCTP MODS\Legends of Wrestlemania\WWE Legends of WrestleMania (USA) (En,Fr)\movies\titantron\122.bik')
DX = Path(r'F:\HCTP MODS\SVR 07\WWE SmackDown vs. Raw 2007 (USA, Europe)\movies\titantron\97.bik')


def convert(src):
    out = Path(tempfile.mkdtemp()) / 'o.mpg'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(src), '-an', '-vf', 'scale=256:256', '-r', '60000/1001',
                    '-c:v', 'mpeg1video', '-q:v', '2', '-f', 'mpeg', str(out)], check=True)
    data = out.read_bytes()
    rel = f'MODS/MEDIA/{hashlib.sha256(data).hexdigest()}.mpg'
    (GAME / rel).write_bytes(data)
    return rel


def main():
    BACKUP.mkdir(exist_ok=True)
    files = [GAME / n for n in ('HOST_MEDIA.txt', 'MEDIA_ROUTES.txt', 'MEDIA_DEFAULTS.txt')] + [LIST]
    for p in files:
        if not (BACKUP / p.name).exists():
            shutil.copy2(p, BACKUP / p.name)
    host = (GAME / 'HOST_MEDIA.txt').read_text().splitlines()
    routes = (GAME / 'MEDIA_ROUTES.txt').read_text().splitlines()
    defaults = (GAME / 'MEDIA_DEFAULTS.txt').read_text().splitlines()
    ch = dict(H.pac(LIST.read_bytes()))
    n = int.from_bytes(ch[0][:2], 'little')
    rows, tail = [ch[0][2+i*32:34+i*32] for i in range(n)], ch[0][2+n*32:]
    names = {r[2:].split(b'\0')[0].decode('latin1') for r in rows}
    assert 'Anita' not in names and 'Jimmy Snuka' not in names, 'already staged'
    used = {int.from_bytes(r[:2], 'little') for r in rows} | {int(l.split()[1]) for l in routes if l.startswith('V ')}
    free = [i for i in range(255, 15, -1) if i not in used]
    nxt = 1 + max(int(l.split()[2]) for l in host if l.startswith('V 7 '))
    snuka_id, anita_id = free[0], free[1]
    dx_rel, snuka_rel = convert(DX), convert(SNUKA)
    host += [f'V 7 {nxt} {dx_rel}', f'V 7 {nxt+1} {snuka_rel}']
    routes = [f'V 224 7 {nxt}' if l.split()[:2] == ['V', '224'] else l for l in routes]
    routes += [f'V {anita_id} 7 45', f'V {snuka_id} 7 {nxt+1}']
    rows += [anita_id.to_bytes(2, 'little') + b'Anita'.ljust(30, b'\0'),
             snuka_id.to_bytes(2, 'little') + b'Jimmy Snuka'.ljust(30, b'\0')]
    key = lambda r: r[2:32].split(b'\0')[0].decode('latin1').removeprefix('The ').casefold()
    other = re.compile(r'(Logo|RAW Opening|SmackDown! Opening)')
    w = sorted((r for r in rows if not other.fullmatch(r[2:32].split(b'\0')[0].decode('latin1'))), key=key)
    ch[0] = len(rows).to_bytes(2, 'little') + b''.join(w + [r for r in rows if r not in w]) + tail
    defaults = [f'70 -1 166 {snuka_id}' if l.split()[:2] == ['70', '-1'] else l for l in defaults]
    data = pac_write(sorted(ch.items())); data += bytes(-len(data) % 2048)
    LIST.write_bytes(data)
    for name, lines in (('HOST_MEDIA.txt', host), ('MEDIA_ROUTES.txt', routes), ('MEDIA_DEFAULTS.txt', defaults)):
        (GAME / name).write_text('\n'.join(lines) + '\n')
    print('DX 224 ->', nxt, dx_rel, '| Anita', anita_id, '-> 7 45 | Snuka', snuka_id, '->', nxt + 1, snuka_rel)


if __name__ == '__main__':
    main()
