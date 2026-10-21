"""Extra HCTP entrance from SYM (Shut Your Mouth) -> loose pac files for the native per-attire entrance hook.

  python -I build_extra_entrance.py <SYM number> <HCTP extra number 3600-3699> <out dir> [--base evtEnt.pac]

Writes <out>/MODS/PAC/EVET/<n> and <n+100> (stage-plane variant, same data) and <out>/MODS/PAC/ENTR/SEVP = the
stock HCTP SEVP with script rows for both numbers appended (table moved to the end, POF0 relocations rebuilt;
stock rows untouched). Copy <out>/MODS into the game folder (game closed) and add "<id> <attire> <n>" to
MODS/PAC/entrances.txt. Conversion = sym_entrance.analyse/synth (SYM keeps HCTP's motion codec; its SYDT script
becomes an HCTP SEVP script). SYM's event header (0x00-0xFF effect slots: filters, light show) is copied over the
built bank's header. EXPERIMENTAL: the Titantron movie / B&W filter path is unverified.
"""
import os
import struct
import sys

sys.path.insert(0, r'F:\WWE SmackDown! vs Raw 2011\Mod Maker Tools')
import hctp_entrance as E  # noqa: E402
import jboy  # noqa: E402
import sym_entrance as S  # noqa: E402

SYM = r'F:\HCTP MODS\SYM'
BASE = r'F:\HCTP MODS\Vanilla\pac\evtEnt.pac'   # stock (the Hogan-lights swap was never installed)


def add_rows(original, rows):
    """original HCTP SEVP + {number: [obj dict(idx, model, t0, t1, slots)]} -> SEVP with those rows added"""
    end = 8 + struct.unpack_from('<I', original, 12)[0]
    assert original[end:end + 4] == b'POF0'
    length = struct.unpack_from('<I', original, end + 4)[0]
    pointers = {v + 8 for v in jboy.pof_decode(original[end + 8:end + 8 + length])}
    data = bytearray(original[:end])
    count, table = struct.unpack_from('<II', data, 16)
    table += 8
    have = {struct.unpack_from('<H', data, table + i * 12)[0] for i in range(count)}
    assert not have & set(rows), 'numbers already in the stock SEVP'
    while len(data) % 16:
        data.append(0)
    new_table = len(data)
    data.extend(data[table:table + 12 * count])
    data.extend(bytes(12 * len(rows)))
    for i in range(count):                                  # moved stock rows keep their object pointers
        if table + i * 12 + 8 in pointers:
            pointers.add(new_table + i * 12 + 8)
    struct.pack_into('<II', data, 16, count + len(rows), new_table - 8)
    for t, (number, objects) in enumerate(sorted(rows.items())):
        row = new_table + 12 * (count + t)
        object_start = len(data)
        data.extend(bytes(28 * len(objects)))
        struct.pack_into('<HHII', data, row, number, len(objects), 0, object_start - 8)
        pointers.add(row + 8)
        for i, o in enumerate(objects):
            p = object_start + i * 28
            slots_start = len(data)
            struct.pack_into('<HH', data, p, o['idx'], o['model'])
            struct.pack_into('>II', data, p + 4, o['t0'], o['t1'])
            struct.pack_into('<IIHHI', data, p + 12, 0xffffffff, 0, len(o['slots']), 0, slots_start - 8)
            pointers.add(p + 24)
            for sl in o['slots']:
                data.extend(struct.pack('<HBBIII5fHH', sl['slot'], sl['on'], 0, sl['tid'], sl['tx'], sl['ty'],
                                        *sl['params'], sl['parent'], 0))
    if 0x14 in pointers or 0x10 + 4 + 8 in pointers:
        pass                                                # (header table field is relocated like the rest)
    new_size = len(data) - 8
    struct.pack_into('<I', data, 4, new_size)
    struct.pack_into('<I', data, 12, new_size)
    encoded = jboy.pof_encode([p - 8 for p in sorted(pointers)])
    assert all(0 <= struct.unpack_from('<I', data, p)[0] < new_size for p in pointers)
    data.extend(b'POF0' + struct.pack('<I', len(encoded)) + encoded)
    assert len(E.parse_sevp(bytes(data))) == count + len(rows)
    return bytes(data)


def pad(b):
    return b + bytes(-len(b) % 0x800)


def main(a):
    sym_num, number, out = int(a[0]), int(a[1]), a[2]
    base = a[a.index('--base') + 1] if '--base' in a else BASE
    assert 3600 <= number < 3700, 'extra entrances use 3600-3699 (stage-plane variant = +100)'
    sym = S.read_source(SYM)
    an = S.analyse(sym, sym_num)
    game, sevp, bank, plan, frames = S.synth(an)
    objs = E.parse_sevp(sevp)[sym_num]['objs']
    raw = sym['banks'][sym_num]
    bank = bytearray(bank)
    bank[:0x100] = raw[:0x100]                              # SYM's effect header (filter / light slots)
    hs = E.read_evtent(base)
    new_sevp = add_rows(hs['sevp'], {number: objs, number + 100: objs})
    for tag, key, blob in (('EVET', str(number), bank), ('EVET', str(number + 100), bank), ('ENTR', 'SEVP', new_sevp)):
        d = os.path.join(out, 'MODS', 'PAC', tag)
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, key), 'wb').write(pad(bytes(blob)))
    rep = ['SYM %d (%s) -> HCTP extra %d / %d' % (sym_num, sym['names'].get(sym_num, '?'), number, number + 100),
           'bank %d bytes, %d script objects, SEVP %d -> %d bytes' % (len(bank), len(objs), len(hs['sevp']), len(new_sevp)),
           'SYM header 0x10-0x1F: ' + raw[0x10:0x20].hex(' '), 'SYM header 0xC0: %02x' % raw[0xC0]]
    rep += plan + ['warning: ' + w for w in an['warnings']]
    open(os.path.join(out, 'report.txt'), 'w').write('\n'.join(rep) + '\n')
    print('\n'.join(rep))


if __name__ == '__main__':
    main(sys.argv[1:])
