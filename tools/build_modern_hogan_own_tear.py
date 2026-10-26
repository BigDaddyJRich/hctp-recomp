"""Modern Hogan shirt tear on Modern Hogan's OWN entrance (stock EVET 3032 bank, Modern motion + camera).
StartScene 0x167c10 shows a char cast only in scenes whose SEVP slot 'on' byte is set; the appended dress cast
(slot 0xDD00) copies body 0's visibility. Tear = scene 5 (shirt prop 2105 comes on). So: body 0 off in scenes 5-7
(gear goes with it), extra body obj 6 (3200) on only in scenes 5-7 playing Modern's own motions x0/y5-7, shirt
parented to obj 6 at the tear. Input = MODS\\PAC\\ENTR\\SEVP.before3601 (rows 3032/3132 retired to 3698/3699)."""
import sys, os, copy, struct
sys.path.insert(0, r'F:\hctp-newbase\tools')
sys.path.insert(0, r'F:\WWE SmackDown! vs Raw 2011\Mod Maker Tools')
import hctp_entrance as E
import build_extra_entrance as B
V = 'F:/HCTP MODS/Vanilla/'
TEAR = 5
REPARENT = False   # Oct 26: reparenting props to the extra body hung the match load (unproven cause)
base = bytearray(open(V + 'MODS/PAC/ENTR/SEVP.before3601', 'rb').read())
hs = E.read_evtent(V + 'pac/evtEnt.pac')
objs = copy.deepcopy(E.parse_sevp(bytes(base))[3032]['objs'])
body = objs[0]
assert body['model'] == 3200 and body['nslots'] == 8
after = copy.deepcopy(body)
after['idx'] = len(objs)
for sl in body['slots']:
    if sl['slot'] >= TEAR: sl['on'] = 0
for sl in after['slots']:
    if sl['slot'] < TEAR: sl['on'] = 0
objs.append(after)
for o in objs:
    if REPARENT and o['model'] in (2105, 2019):
        for sl in o['slots']:
            if sl['slot'] >= TEAR and sl['parent'] == 0: sl['parent'] = after['idx']
count, table = struct.unpack_from('<II', base, 16)
table += 8
spare = iter((3698, 3699))
for i in range(count):
    n = struct.unpack_from('<H', base, table + i * 12)[0]
    if n in (3032, 3132):
        struct.pack_into('<H', base, table + i * 12, next(spare))
new = B.add_rows(bytes(base), {3032: objs, 3132: objs})
open(V + 'MODS/PAC/ENTR/SEVP', 'wb').write(B.pad(new))
for n in (3032, 3132):
    open(V + 'MODS/PAC/EVET/%d' % n, 'wb').write(B.pad(bytes(hs['banks'][3032])))
chk = E.parse_sevp(new)[3032]['objs']
for o in chk:
    print(o['idx'], o['model'], ''.join(str(s['on']) for s in o['slots']), [s['parent'] for s in o['slots']])
