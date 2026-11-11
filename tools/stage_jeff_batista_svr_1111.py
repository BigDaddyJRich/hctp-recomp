"""Jeff Hardy + Batista -> their SvR04 entrances (Jordan Nov 11), data only, no build.

Jeff (0x0D) -> virtual 7059 = SvR04 3152 (Original 09), Batista (0x3C) -> virtual 7041 = SvR04 3060 ("Batista 2"),
for every attire, via MODS\\PAC\\entrances.txt (runner getEntranceEventNoPerAttire: plane arenas +100 -> 7159 / 7141;
Season story events that force their own entrance are left alone, ordinary Season matches use this too).
The Oct 26 converted entrances are removed from the game (moved to the backup folder):
  EVET 3060/3260 (Batista; SEVP rows were never changed -> stock retail Batista returns as the fallback/CAM row),
  EVET 3013/3213 (Jeff; SEVP rows 3013/3213 restored to the stock 2-object stub = same as rows 3049/3249).
Backups: Vanilla\\MODS\\backup_jeff_batista_svr_1111\\.
"""
import os, shutil, struct, sys
sys.path.insert(0, r'F:\WWE SmackDown! vs Raw 2011\Mod Maker Tools')
import hctp_entrance as E

V = 'F:/HCTP MODS/Vanilla/MODS/'
BK = V + 'backup_jeff_batista_svr_1111/'
ROUTE = {0x0D: (7059, 'Jeff Hardy -> SvR04 3152 Original 09'), 0x3C: (7041, 'Batista -> SvR04 3060')}
ATTIRES = range(0, 6)

os.makedirs(BK, exist_ok=True)
for f in ('PAC/entrances.txt', 'PAC/ENTR/SEVP'):
    dst = BK + f.replace('/', '_')
    if not os.path.exists(dst):
        shutil.copy2(V + f, dst)

# 1. entrances.txt: drop old lines for these ids, add every attire
lines = open(BK + 'PAC_entrances.txt', encoding='latin-1').read().splitlines()
keep = []
for l in lines:
    p = l.split()
    if len(p) >= 3 and p[0].isdigit() and int(p[0]) in ROUTE:
        continue
    keep.append(l)
for rid, (ev, why) in ROUTE.items():
    keep.append('# Nov 11 (Jordan): %s, all attires (plane arenas +100)' % why)
    keep += ['%d %d %d' % (rid, a, ev) for a in ATTIRES]
open(V + 'PAC/entrances.txt', 'w', encoding='latin-1', newline='\n').write('\n'.join(keep) + '\n')

# 2. old converted banks out of the game
for n in (3013, 3213, 3060, 3260):
    src = V + 'PAC/EVET/%d' % n
    if os.path.exists(src):
        shutil.move(src, BK + 'EVET_%d' % n)

# 3. SEVP rows 3013/3213 back to the stock stub (rows 3049/3249)
s = bytearray(open(V + 'PAC/ENTR/SEVP', 'rb').read())
count, table = struct.unpack_from('<II', s, 16)
table += 8
rows = {struct.unpack_from('<H', s, table + i * 12)[0]: table + i * 12 for i in range(count)}
for dst, src in ((3013, 3049), (3213, 3249)):
    s[rows[dst] + 2:rows[dst] + 4] = s[rows[src] + 2:rows[src] + 4]
    s[rows[dst] + 8:rows[dst] + 12] = s[rows[src] + 8:rows[src] + 12]
open(V + 'PAC/ENTR/SEVP', 'wb').write(bytes(s))
sv = E.parse_sevp(bytes(s))
print('SEVP 3013', [o['model'] for o in sv[3013]['objs']], '3232', [o['model'] for o in sv[3232]['objs']])
print(open(V + 'PAC/entrances.txt', encoding='latin-1').read())
