"""Hogan (3032) entrance lighting = legend 3078: host SEVP without 3032's trailing type-6 screen-effect row (gray/flat look).
Writes logs/hogan_legend_look/MODS/PAC/ENTR/SEVP; copy MODS into the game folder (game closed). Jordan-confirmed Oct 21."""
import sys, struct, os
sys.path.insert(0, r'F:\WWE SmackDown! vs Raw 2011\Mod Maker Tools')
import hctp_entrance as E
hs = E.read_evtent(r'F:\HCTP MODS\Vanilla\pac_vanilla_backup\evtEnt.pac')
s = bytearray(hs['sevp'])
cnt, tab = struct.unpack_from('<II', s, 24); tab += 8
for i in range(cnt):
    ev, n, ptr = struct.unpack_from('<HHI', s, tab + i * 8)
    if ev == 3032:
        last = struct.unpack_from('<6HI', s, ptr + 8 + (n - 1) * 16)
        assert n == 5 and last[0] == 6, (n, last)
        struct.pack_into('<H', s, tab + i * 8 + 2, 4)   # drop the trailing type-6 (gray screen effect) row
        print('3032 effect rows 5 -> 4, dropped', last)
out = r'F:\hctp-newbase\logs\hogan_legend_look\MODS\PAC\ENTR'
os.makedirs(out, exist_ok=True)
d = bytes(s) + bytes(-len(s) % 0x800)
open(os.path.join(out, 'SEVP'), 'wb').write(d)
print(len(d))
