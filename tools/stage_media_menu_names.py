"""Rename + reorder the music (LIST sub 1) and TitanTron (sub 0) menus: wrestlers A-Z, then non-wrestler rows A-Z.
Rows keep their media ids, so saved/default choices are unchanged. Labels are plain names with 2/3 suffixes."""
import hashlib, re, sys
from pathlib import Path
sys.path.insert(0, r'F:\WWE SmackDown! vs Raw 2011\Mod Maker Tools')
import hctp_entrance as E
from build_imported_media import pac_write

SOURCE = Path(r'F:\HCTP MODS\Vanilla\MODS\PAC\MOVE\LIST')
OUT = Path(r'F:\hctp-project\media_port\media-menu-names\MOVE\LIST')

VIDEO_NAMES = {3: 'Undertaker', 235: 'The Undertaker', 219: 'Hulk Hogan', 247: 'Hulk Hogan 2'}
VIDEO_DROP = {208}  # stock Hogan tron: Jordan dropped it
MUSIC_NAMES = {58: 'Billy & Chuck', 228: 'Chris Jericho 2', 230: 'Matt Hardy 3', 229: 'The Rock 3',
               226: 'Triple H 2', 227: 'Triple H 3', 169: 'The Undertaker', 26: 'Taz', 199: 'Taz 2',
               192: 'nWo', 23: 'Hulk Hogan', 39: 'Hulk Hogan 2', 250: '3 Minute Warning',
               **{232 + k: f'BGM {17 + k}' for k in range(9)}}
NON_WRESTLER = re.compile(r'(BGM \d+|Original \d+|Free Slot \d+|No Music|RAW|SmackDown!|Logo|RAW Opening|SmackDown! Opening)')

def rows(blob):
    n = int.from_bytes(blob[:2], 'little')
    return [blob[2+i*32:2+(i+1)*32] for i in range(n)]
def rid(r): return int.from_bytes(r[:2], 'little')
def name(r): return r[2:32].split(b'\0', 1)[0].decode('latin1')
def mk(i, n): return i.to_bytes(2, 'little') + n.encode('latin1').ljust(30, b'\0')[:30]
def key(r):
    n = name(r).removeprefix('The ')
    return [int(t) if t.isdigit() else t.casefold() for t in re.split(r'(\d+)', n)]

def rebuild(blob, names, drop=frozenset()):
    rs = [mk(rid(r), names.get(rid(r), name(r))) for r in rows(blob) if rid(r) not in drop]
    have = {rid(r) for r in rs}
    rs += [mk(i, n) for i, n in names.items() if i not in have]
    for r in rs: assert '(' not in name(r), name(r)
    ids = [rid(r) for r in rs]; assert len(ids) == len(set(ids)) and len(rs) < 0xba
    w = sorted((r for r in rs if not NON_WRESTLER.fullmatch(name(r))), key=key)
    o = sorted((r for r in rs if NON_WRESTLER.fullmatch(name(r))), key=key)
    out = w + o
    print('\n'.join(f'{rid(r)}:{name(r)}' for r in out).replace('\n', ' | '), '\n')
    return len(out).to_bytes(2, 'little') + b''.join(out) + blob[2 + len(rows(blob)) * 32:]

ch = E.pac(SOURCE.read_bytes())
new = [(i, rebuild(b, VIDEO_NAMES, VIDEO_DROP) if i == 0 else rebuild(b, MUSIC_NAMES) if i == 1 else b) for i, b in ch]
data = pac_write(new); data += bytes(-len(data) % 2048)
OUT.parent.mkdir(parents=True, exist_ok=True); OUT.write_bytes(data)
print(len(data), hashlib.sha256(data).hexdigest())
