"""Hogan entrance crowd fix (Nov 11, data only, no build).

Cause (file level):
 1. Every stock entrance THQ finished has op 192 crowd-cheer cues (SE 47/49 = /SFX/LIST bank 6, the BK6* cheer banks).
    The only stock entrances with NO op 192 are the cut/unfinished ones (Jeff 3013, Regal, Bradshaw, Hogan 3032/3232,
    Rosey, Jamal, Gunn, Molly, Spike, Kidman, Al Snow, Tazz, Legend Hogan 3078/3278, non-wrestlers). Their bank header
    even has the op-192 size byte T[192] = 0. So Hogan only ever gets the quiet MIDI base bed (SE 0x12D, bank 8) that
    CEventBGM::Start starts; under his theme it sounds like no crowd.
 2. Modern Hogan has no loose EVET 3232 and the host SEVP row 3232 is still the stock cut entrance (Hogan + camera only).
    GetEntranceEventNoFromSystem 0x2ad1b0 adds +200 on stage-plane arenas (IsStagePlane 0x2af240: arena byte
    0x4922C0 in A,B,E,F,12,13,30,31,34,35), so there he plays that unfinished entrance (no cues, no lights, no tear).

Fix, timing taken from THQ's finished copy of the same animation = SvR04 event 3158 ("Original 14", see
notes/SVR_PRESET_9_14_VERIFIED.md): SE 49 at frame 4 of its stage segment (slot 4) and SE 47 at frame 119 of the
199-frame posing segment (slot 19).
  3032/3132: y0 frame 294+4 (SvR slot 4 = 3032 y0 from frame 294, 247/293 poses byte-identical; also the camera cut +
             flash-bulb level 2->3 frame) and y6 frame 119 (same 199-frame track).
  3078/3278: obj 0 y0 frame 238+4 (its own flash-bulb level 2->3 frame; intro animation differs from SvR) and
             obj 6 y4 frame 119 (same 199-frame posing track).
  3232: loose bank = patched 3032 bank; SEVP row 3232 -> 3032's objects (his finished entrance on plane arenas).
Backups: Vanilla/MODS/backup_hogan_crowd_1111/.
"""
import os, shutil, struct, sys
sys.path.insert(0, r'F:\WWE SmackDown! vs Raw 2011\Mod Maker Tools')
import hctp_entrance as E, hctp_motion as H, ymks as Y

V = 'F:/HCTP MODS/Vanilla/MODS/'
BK = V + 'backup_hogan_crowd_1111/'
SE_START, SE_POSE = 49, 47
PLAN = {  # bank -> [(track id, x, y, frame, se)]
    3032: [(9950, 0, 0, 298, SE_START), (9950, 0, 6, 119, SE_POSE)],
    3078: [(9950, 0, 0, 242, SE_START), (9950, 6, 4, 119, SE_POSE)],
}
PLAN[3132] = PLAN[3032]
PLAN[3278] = PLAN[3078]


def tokens(data):
    """frame stream -> [('skip', byte) | ('rec', frame, bytes)]"""
    out, p, f = [], 0, 0
    while p < len(data):
        b = data[p]
        if b < 0x60:
            out.append(('skip', b)); f += b; p += 1; continue
        if b == 0x60:
            sz = data[p + 1]; p += 2
        else:
            sz = b - 0x60; p += 1
        out.append(('rec', f, data[p:p + sz])); p += sz; f += 1
    return out


def untokens(toks):
    out = bytearray()
    for t in toks:
        if t[0] == 'skip':
            out.append(t[1]); continue
        r = t[2]
        if 0 < len(r) <= 0x9F:
            out.append(0x60 + len(r))
        else:
            assert len(r) <= 0xFF, 'record too big'
            out += bytes([0x60, len(r)])
        out += r
    return bytes(out)


def add_se(data, frame, se):
    toks = tokens(data)
    recs = [i for i, t in enumerate(toks) if t[0] == 'rec']
    i = min(recs, key=lambda k: (abs(toks[k][1] - frame), toks[k][1]))  # nearest real record
    ev, pose = Y.split_record(toks[i][2])
    if bytes([0xC0, se]) in ev:
        return data, toks[i][1], False
    ev = ev + bytes([0xC0, se])
    assert len(ev) <= 0x7F
    toks[i] = ('rec', toks[i][1], bytes([0x80 | len(ev)]) + ev + pose)
    new = untokens(toks)
    assert untokens(tokens(data)) == data, 'stream re-encode mismatch'
    return new, toks[i][1], True


def patch_bank(d, plan):
    T, ents = H.read_bank(d)
    n = struct.unpack_from('<I', d, 0x100)[0]
    dirs = [list(struct.unpack_from('<IIII', d, 0x104 + 16 * i)) for i in range(n)]
    data = d[0x104 + 16 * n:]
    offs = sorted(set(e[1] for e in dirs if e[1] < len(data)))
    blocks = {o: data[o:(offs[k + 1] if k + 1 < len(offs) else len(data))] for k, o in enumerate(offs)}
    log = []
    for tid, x, y, frame, se in plan:
        key = (tid << 16) | (x << 8) | y
        e = [e for e in dirs if e[0] == key]
        assert len(e) == 1, 'track %s missing' % ((tid, x, y),)
        o = e[0][1]
        sharers = [hex(q[0]) for q in dirs if q[1] == o]
        blocks[o], got, did = add_se(blocks[o], frame, se)
        log.append('track %d,%d,%d SE %d at frame %d (wanted %d)%s%s' % (tid, x, y, se, got, frame,
                   '' if did else ' already there', '' if len(sharers) == 1 else ' shared by ' + ','.join(sharers)))
    newdata, remap = bytearray(data[:offs[0]]), {}  # keep the unreferenced leading bytes as they were
    for o in offs:
        remap[o] = len(newdata); newdata += blocks[o]
    head = bytearray(d[:0x104 + 16 * n])
    head[192] = 2  # op 192 'c0 id' = 2 bytes (stock finished banks: T[192] = 2)
    for i, e in enumerate(dirs):
        if e[1] in remap:
            struct.pack_into('<I', head, 0x104 + 16 * i + 4, remap[e[1]])
    out = bytes(head) + bytes(newdata)
    pad = (-len(out)) % 0x800
    return out + bytes(pad), log


def main():
    os.makedirs(BK, exist_ok=True)
    for f in ['PAC/EVET/3032', 'PAC/EVET/3132', 'PAC/EVET/3078', 'PAC/EVET/3278', 'PAC/ENTR/SEVP']:
        dst = BK + f.replace('/', '_')
        if not os.path.exists(dst):
            shutil.copy2(V + f, dst)
    for n, plan in PLAN.items():
        src = BK + 'PAC_EVET_%d' % n  # always patch from the backup (idempotent)
        out, log = patch_bank(open(src, 'rb').read(), plan)
        open(V + 'PAC/EVET/%d' % n, 'wb').write(out)
        print(n, len(out), *log, sep='\n  ')
    shutil.copy2(V + 'PAC/EVET/3032', V + 'PAC/EVET/3232')
    print('3232 = patched 3032 bank')
    # SEVP: row 3232 -> 3032's objects (pointer + count; both are POF0-relocated fields already)
    s = bytearray(open(BK + 'PAC_ENTR_SEVP', 'rb').read())
    count, table = struct.unpack_from('<II', s, 16)
    table += 8
    rows = {struct.unpack_from('<H', s, table + i * 12)[0]: table + i * 12 for i in range(count)}
    src, dst = rows[3032], rows[3232]
    s[dst + 2:dst + 4] = s[src + 2:src + 4]
    s[dst + 8:dst + 12] = s[src + 8:src + 12]
    open(V + 'PAC/ENTR/SEVP', 'wb').write(bytes(s))
    sv = E.parse_sevp(bytes(s))
    assert [o['model'] for o in sv[3232]['objs']] == [o['model'] for o in sv[3032]['objs']]
    print('SEVP 3232 objs ->', [o['model'] for o in sv[3232]['objs']])


if __name__ == '__main__':
    main()
