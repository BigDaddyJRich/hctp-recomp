"""Modern Hogan shirt tear: Modern Hogan event 3032/3132 = Legend Hogan event 3078 (EVET bank + SEVP row) with the
after-tear body obj (literal 7800) set to 3200. Input = MODS\PAC\ENTR\SEVP.before3601 (the earlier host SEVP).
Old 3032/3132 rows are renumbered 3698/3699. Verified in a match Oct 22 (Jordan)."""
import sys, os, copy, struct
sys.path.insert(0, r'F:\hctp-newbase\tools')
sys.path.insert(0, r'F:\WWE SmackDown! vs Raw 2011\Mod Maker Tools')
import hctp_entrance as E
import build_extra_entrance as B
V = 'F:/HCTP MODS/Vanilla/'
base = bytearray(open(V + 'MODS/PAC/ENTR/SEVP.before3601', 'rb').read())
hs = E.read_evtent(V + 'pac/evtEnt.pac')
objs = copy.deepcopy(E.parse_sevp(bytes(base))[3078]['objs'])
for o in objs:
    if o['model'] == 7800: o['model'] = 3200
count, table = struct.unpack_from('<II', base, 16)
table += 8
spare = iter((3698, 3699))
for i in range(count):
    n = struct.unpack_from('<H', base, table + i * 12)[0]
    if n in (3032, 3132):
        struct.pack_into('<H', base, table + i * 12, next(spare))   # retire the old Modern Hogan rows
new = B.add_rows(bytes(base), {3032: objs, 3132: objs})
open(V + 'MODS/PAC/ENTR/SEVP', 'wb').write(B.pad(new))
for n in (3601, 3701):
    p = V + 'MODS/PAC/EVET/%d' % n
    if os.path.exists(p): os.remove(p)
for n in (3032, 3132):
    open(V + 'MODS/PAC/EVET/%d' % n, 'wb').write(B.pad(bytes(hs['banks'][3078])))
print('ok', sorted(E.parse_sevp(new)[3032]['objs'][i]['model'] for i in range(len(objs))))
