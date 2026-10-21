"""Victory-motion test entries for Create-a-Moveset -> loose pac files for the native victory hook.

  python -I build_victory_test_list.py <out dir> [--names names.txt]

Writes <out>/MODS/PAC/MOVE/WAZA = the stock moves.pac MOVE/WAZA (32-byte header, u32 count at +4, 64-byte
{name[32], data[32]: u16 id at +40}) with victory records APPENDED (cloned from Steve Austin's victory, id 1);
stock records stay byte-identical. The victory menu lists these records (ids 1-85 = each wrestler's victory,
event 6200 + table[id] in ConvertDoramockFromMoves 0x39dba0; the cut wrestlers' ids are missing). Test entry k
gets the next unused id from 6900 (free, < 0x1c34 so it takes the doramock path) and is called "Test <event>";
<out>/MODS/PAC/victory.txt maps id -> event for the hook. --names: lines "<event> <name>" rename identified entries (e.g.
"6332 Hulk Hogan (Champion)"); "<event> -" drops an entry. Copy <out>/MODS into the game folder with the game closed.
"""
import os
import struct
import sys

sys.path.insert(0, r'F:\WWE SmackDown! vs Raw 2011\Mod Maker Tools')
import hctp_entrance as E  # noqa: E402

MOVES = r'F:\HCTP MODS\Vanilla\pac\moves.pac'
EVENTS = [6202, 6208, 6220, 6223, 6226, 6236, 6240, 6243, 6258] + list(range(6261, 6276)) + list(range(6284, 6300)) + \
         [6300, 6301] + list(range(6314, 6324)) + [6328, 6329, 6331] + list(range(6332, 6343)) + list(range(6344, 6351))
FIRST = 6900


def build_pac(entries):
    """[(id, bytes)] -> HCTP inner 'PAC ' (E.pac's layout: u16 id, u24 offset, u24 size; data 4-aligned)"""
    n = len(entries)
    head, data = bytearray(b'PAC ' + struct.pack('<I', n)), bytearray()
    for i, b in entries:
        while len(data) % 4:
            data.append(0)
        head += struct.pack('<H', i) + len(data).to_bytes(3, 'little') + len(b).to_bytes(3, 'little')
        data += b
    return bytes(head + data)


def main(a):
    out = a[0]
    names = {}
    if '--names' in a:
        for line in open(a[a.index('--names') + 1], encoding='utf-8'):
            p = line.strip().split(None, 1)
            if len(p) == 2 and p[0].isdigit():
                names[int(p[0])] = p[1]
    d0 = open(MOVES, 'rb').read()
    waza = next(b for g, n, b in E.dpac(d0) if g == 'MOVE' and n == 'WAZA')
    count = struct.unpack_from('<I', waza, 4)[0]
    recs = [waza[0x20 + 64 * k:0x60 + 64 * k] for k in range(count)]
    ids = {struct.unpack_from('<H', r, 40)[0] for r in recs}
    tmpl = next(r for r in recs if struct.unpack_from('<H', r, 40)[0] == 1)
    free = [i for i in range(FIRST, 0x1c34) if i not in ids]   # unused ids below 0x1c34 (doramock path)
    vic = []
    k = 0
    body = bytearray(waza[:0x20 + 64 * count])
    for ev in EVENTS:
        name = names.get(ev, 'Test %d' % ev)
        if name == '-':
            continue
        n = free[k]
        k += 1
        r = bytearray(tmpl)
        r[0:32] = (name.encode('latin-1')[:31] + bytes(1)).ljust(32, b' ')   # stock: name, NUL, space padding
        struct.pack_into('<H', r, 40, n)
        body += r
        vic.append('%d %d  # %s' % (n, ev, name))
    struct.pack_into('<I', body, 4, count + k)
    new = bytes(body) + bytes(-len(body) % 0x100)
    d = os.path.join(out, 'MODS', 'PAC', 'MOVE')
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, 'WAZA'), 'wb').write(new)
    open(os.path.join(out, 'MODS', 'PAC', 'victory.txt'), 'w').write(
        '# <victory list number> <evtDora event>  (tools/build_victory_test_list.py)\n' + '\n'.join(vic) + '\n')
    print('WAZA: %d stock + %d added victory records; %d -> %d bytes' % (count, k, len(waza), len(new)))


if __name__ == '__main__':
    main(sys.argv[1:])
