# Rough VU1 disassembler for a code dump: python -I tools\vu1dis.py <code.bin> <from_hex> <to_hex>
# Prints address, upper op (dest fd fs ft) and lower op (vit vis vid imm11 / imm15), plus E/I/D/T bits.
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vu1_recomp as v  # noqa: E402

code = open(sys.argv[1], 'rb').read()
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
DESTS = 'xyzw'
for a in range(lo, hi, 8):
    lower, upper = struct.unpack_from('<II', code, a)
    bits = ''.join(ch for ch, b in (('I', 31), ('E', 30), ('M', 29), ('D', 28), ('T', 27)) if upper >> b & 1)
    dest = ''.join(DESTS[c] for c in range(4) if v.DEST(upper) & (8 >> c))
    up = '%s.%s vf%d,vf%d,vf%d' % (v.upper_name(upper), dest, v.FD(upper), v.FS(upper), v.FT(upper))
    if upper >> 31 & 1:
        lw = 'I=%08x' % lower
    else:
        imm11 = lower & 0x7FF
        if imm11 & 0x400:
            imm11 -= 0x800
        imm15 = ((lower >> 10) & 0x7800) | (lower & 0x7FF)
        ld = ''.join(DESTS[c] for c in range(4) if v.DEST(lower) & (8 >> c))
        lw = '%s.%s vit=%d vis=%d vid=%d imm11=%d imm15=%d tgt=%04x' % (
            v.lower_name(lower), ld, v.VIT(lower), v.VIS(lower), v.VID(lower), imm11, imm15, (a + 8 + imm11 * 8) & 0x3FFF)
    print('%04x %-5s %-36s | %s' % (a, bits, up, lw))
