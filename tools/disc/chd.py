# CHD v5 decoding helpers (header, map, Huffman) — copied from adjmk/tools (same author).
import struct, zlib, lzma, sys


def be(b): return int.from_bytes(b, 'big')

# ---- CRC16 (CCITT, poly 0x1021, init 0xffff, no reflection, no xorout) ----
_crc16_tab = []
for i in range(256):
    c = i << 8
    for _ in range(8):
        c = ((c << 1) ^ 0x1021) if (c & 0x8000) else (c << 1)
        c &= 0xffff
    _crc16_tab.append(c)
def crc16(data):
    crc = 0xffff
    for b in data:
        crc = ((crc << 8) & 0xffff) ^ _crc16_tab[((crc >> 8) ^ b) & 0xff]
    return crc

# ---- MSB-first bit reader ----
class Bits:
    def __init__(self, data):
        self.data = data; self.pos = 0; self.bits = 0; self.acc = 0; self.of = False
    def read(self, n):
        while self.bits < n:
            if self.pos < len(self.data):
                self.acc = (self.acc << 8) | self.data[self.pos]; self.pos += 1
            else:
                self.acc <<= 8; self.of = True
            self.bits += 8
        self.bits -= n
        v = (self.acc >> self.bits) & ((1 << n) - 1)
        self.acc &= (1 << self.bits) - 1   # keep only the unread bits (else the number grows without end)
        return v

# ---- MAME huffman decoder (import_tree_rle + canonical codes) ----
class Huff:
    def __init__(self, numcodes, maxbits):
        self.numcodes = numcodes; self.maxbits = maxbits
        self.numbits_of = [0]*numcodes
        self.code_of = [0]*numcodes
        self.lookup = {}
    def import_tree_rle(self, bits):
        nb = 5 if self.maxbits >= 16 else 4 if self.maxbits >= 8 else 3
        cur = 0
        while cur < self.numcodes:
            v = bits.read(nb)
            if v != 1:
                self.numbits_of[cur] = v; cur += 1
            else:
                nodebits = bits.read(nb)
                if nodebits == 1:
                    self.numbits_of[cur] = 1; cur += 1
                else:
                    rep = bits.read(nb) + 3
                    while rep and cur < self.numcodes:
                        self.numbits_of[cur] = nodebits; cur += 1; rep -= 1
        self.assign_canonical()
    def assign_canonical(self):
        hist = [0]*33
        for c in range(self.numcodes):
            n = self.numbits_of[c]
            if n <= 32: hist[n] += 1
        curstart = 0
        for L in range(32, 0, -1):
            nextstart = (curstart + hist[L]) >> 1
            hist[L] = curstart
            curstart = nextstart
        self.lookup = {}
        for c in range(self.numcodes):
            n = self.numbits_of[c]
            if n > 0:
                code = hist[n]; hist[n] += 1
                self.code_of[c] = code
                self.lookup[(n, code)] = c
    def decode_one(self, bits):
        code = 0; length = 0
        while length < self.maxbits + 1:
            code = (code << 1) | bits.read(1); length += 1
            s = self.lookup.get((length, code))
            if s is not None:
                return s
        raise ValueError("huffman decode fail")

# compression type constants
T0,T1,T2,T3,NONE,SELF,PARENT,RLE_S,RLE_L = 0,1,2,3,4,5,6,7,8
SELF_0,SELF_1,PARENT_SELF,PARENT_0,PARENT_1 = 9,10,11,12,13

def main():
    f = open(sys.argv[1], "rb")  # python chd.py <disc.chd> — header dump
    hdr = f.read(124)
    assert hdr[:8] == b'MComprHD'
    hlen = be(hdr[8:12]); ver = be(hdr[12:16])
    comps = [hdr[16+i*4:20+i*4] for i in range(4)]
    logicalbytes = be(hdr[32:40])
    mapoffset = be(hdr[40:48])
    metaoffset = be(hdr[48:56])
    hunkbytes = be(hdr[56:60])
    unitbytes = be(hdr[60:64])
    hunkcount = (logicalbytes + hunkbytes - 1)//hunkbytes
    print("ver",ver,"hlen",hlen,"comps",comps,"logical",logicalbytes,
          "mapoff",mapoffset,"metaoff",metaoffset,"hunkbytes",hunkbytes,
          "unitbytes",unitbytes,"hunkcount",hunkcount)

    f.seek(mapoffset)
    maphdr = f.read(16)
    mapbytes = be(maphdr[0:4]); firstoff = be(maphdr[4:10])
    mapcrc = be(maphdr[10:12]); lengthbits = maphdr[12]; selfbits = maphdr[13]; parentbits = maphdr[14]
    print("mapbytes",mapbytes,"firstoff",firstoff,"mapcrc",hex(mapcrc),
          "lengthbits",lengthbits,"selfbits",selfbits,"parentbits",parentbits)
    comp = f.read(mapbytes)

    bits = Bits(comp)
    dec = Huff(16, 8)
    dec.import_tree_rle(bits)

    rawmap = bytearray(12*hunkcount)
    lastcomp = 0; repcount = 0
    for h in range(hunkcount):
        if repcount > 0:
            rawmap[h*12] = lastcomp; repcount -= 1
        else:
            v = dec.decode_one(bits)
            if v == RLE_S:
                rawmap[h*12] = lastcomp; repcount = 2 + dec.decode_one(bits)
            elif v == RLE_L:
                rawmap[h*12] = lastcomp
                repcount = 2 + 16 + (dec.decode_one(bits) << 4)
                repcount += dec.decode_one(bits)
            else:
                lastcomp = v; rawmap[h*12] = v

    from collections import Counter
    print("type histogram:", Counter(rawmap[i*12] for i in range(hunkcount)))

    curoffset = firstoff; last_self = 0; last_parent = 0
    for h in range(hunkcount):
        o = h*12
        t = rawmap[o]
        offset = curoffset; length = 0; crc = 0
        if t in (T0,T1,T2,T3):
            length = bits.read(lengthbits); curoffset += length; crc = bits.read(16)
        elif t == NONE:
            length = hunkbytes; curoffset += length; crc = bits.read(16)
        elif t == SELF:
            offset = bits.read(selfbits); last_self = offset
        elif t == PARENT:
            offset = bits.read(parentbits); last_parent = offset
        elif t == SELF_0:
            rawmap[o] = SELF; offset = last_self
        elif t == SELF_1:
            rawmap[o] = SELF; last_self += 1; offset = last_self
        elif t == PARENT_SELF:
            rawmap[o] = PARENT; last_parent = (h*hunkbytes)//unitbytes; offset = last_parent
        elif t == PARENT_0:
            rawmap[o] = PARENT; offset = last_parent
        elif t == PARENT_1:
            rawmap[o] = PARENT; last_parent += 1; offset = last_parent
        else:
            raise ValueError("bad type %d at hunk %d" % (t,h))
        rawmap[o+1:o+4] = length.to_bytes(3,'big')
        rawmap[o+4:o+10] = offset.to_bytes(6,'big')
        rawmap[o+10:o+12] = crc.to_bytes(2,'big')

    got = crc16(rawmap)
    print("computed mapcrc", hex(got), "expected", hex(mapcrc), "MATCH" if got==mapcrc else "MISMATCH")

if __name__ == '__main__':
    main()
