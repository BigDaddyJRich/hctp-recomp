# CHD (MAME compressed disc) reader — copied from adjmk/tools (same author), the default disc path removed.
import struct, zlib, lzma
from chd import be, Bits, Huff, crc16 as _crc16
from chd import T0,T1,T2,T3,NONE,SELF,PARENT,RLE_S,RLE_L,SELF_0,SELF_1,PARENT_SELF,PARENT_0,PARENT_1

FRAME = 2448
SECTOR_DATA = 2352
SUBCODE = 96

class CHD:
    def __init__(self, path):
        self.f = open(path, 'rb')
        hdr = self.f.read(124)
        assert hdr[:8] == b'MComprHD'
        self.comps = [hdr[16+i*4:20+i*4] for i in range(4)]
        self.logicalbytes = be(hdr[32:40])
        self.mapoffset = be(hdr[40:48])
        self.metaoffset = be(hdr[48:56])
        self.hunkbytes = be(hdr[56:60])          # 19584
        self.unitbytes = be(hdr[60:64])          # 2448
        self.hunkcount = (self.logicalbytes + self.hunkbytes - 1)//self.hunkbytes
        self.frames_per_hunk = self.hunkbytes // FRAME  # 8
        self._lzma_filt = [{"id": lzma.FILTER_LZMA1, "dict_size": 24576, "lc": 3, "lp": 0, "pb": 2}]
        self._decode_map()
        self._cache = {}

    def _decode_map(self):
        self.f.seek(self.mapoffset)
        mh = self.f.read(16)
        mapbytes = be(mh[0:4]); firstoff = be(mh[4:10])
        self.lengthbits = mh[12]; self.selfbits = mh[13]; self.parentbits = mh[14]
        comp = self.f.read(mapbytes)
        bits = Bits(comp)
        dec = Huff(16, 8); dec.import_tree_rle(bits)
        n = self.hunkcount
        ctype = bytearray(n); lastcomp = 0; repcount = 0
        for h in range(n):
            if repcount > 0:
                ctype[h] = lastcomp; repcount -= 1
            else:
                v = dec.decode_one(bits)
                if v == RLE_S:
                    ctype[h] = lastcomp; repcount = 2 + dec.decode_one(bits)
                elif v == RLE_L:
                    ctype[h] = lastcomp
                    repcount = 2 + 16 + (dec.decode_one(bits) << 4)
                    repcount += dec.decode_one(bits)
                else:
                    lastcomp = v; ctype[h] = v
        self.comp = ctype
        self.offset = [0]*n; self.length = [0]*n; self.crc = [0]*n
        curoffset = firstoff; last_self = 0; last_parent = 0
        for h in range(n):
            t = ctype[h]; off = curoffset; length = 0
            if t in (T0,T1,T2,T3):
                length = bits.read(self.lengthbits); curoffset += length; self.crc[h] = bits.read(16)
            elif t == NONE:
                length = self.hunkbytes; curoffset += length; self.crc[h] = bits.read(16)
            elif t == SELF:
                off = bits.read(self.selfbits); last_self = off
            elif t == PARENT:
                off = bits.read(self.parentbits); last_parent = off
            elif t == SELF_0:
                self.comp[h] = SELF; off = last_self
            elif t == SELF_1:
                self.comp[h] = SELF; last_self += 1; off = last_self
            elif t == PARENT_SELF:
                self.comp[h] = PARENT; last_parent = (h*self.hunkbytes)//self.unitbytes; off = last_parent
            elif t == PARENT_0:
                self.comp[h] = PARENT; off = last_parent
            elif t == PARENT_1:
                self.comp[h] = PARENT; last_parent += 1; off = last_parent
            self.offset[h] = off; self.length[h] = length

    def _base_decompress(self, codec, data, outlen):
        if codec == 0:      # cdlz -> LZMA base
            d = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=self._lzma_filt)
            out = d.decompress(data, outlen)
            return out
        elif codec == 1:    # cdzl -> zlib base (raw deflate)
            return zlib.decompress(data, -15, outlen)
        else:
            raise ValueError("unhandled base codec %d" % codec)

    def read_hunk(self, h, _depth=0):
        if h in self._cache:
            return self._cache[h]
        t = self.comp[h]
        frames = self.frames_per_hunk
        if t in (T0, T1):
            blocklen = self.length[h]
            self.f.seek(self.offset[h]); src = self.f.read(blocklen)
            complen_bytes = 2 if self.hunkbytes < 65536 else 3
            ecc_bytes = (frames + 7)//8
            header_bytes = ecc_bytes + complen_bytes
            complen_base = be(src[ecc_bytes:ecc_bytes+complen_bytes])
            base = self._base_decompress(t, src[header_bytes:header_bytes+complen_base], frames*SECTOR_DATA)
            # subcode (zlib in both cdlz and cdzl); not needed for ISO user data
            dest = bytearray(self.hunkbytes)
            for fr in range(frames):
                dest[fr*FRAME:fr*FRAME+SECTOR_DATA] = base[fr*SECTOR_DATA:(fr+1)*SECTOR_DATA]
        elif t == T2:
            # CD-FLAC: raw FLAC frames (metadata stripped). On this disc the sole
            # FLAC hunk is zero-fill; reconstruct as zeros and verify against the
            # stored per-hunk CRC16 so any non-zero FLAC hunk fails loudly.
            dest = bytearray(self.hunkbytes)
            if _crc16(dest) != self.crc[h]:
                raise NotImplementedError("non-zero CD-FLAC hunk %d needs a FLAC decoder" % h)
        elif t == NONE:
            self.f.seek(self.offset[h]); dest = bytearray(self.f.read(self.hunkbytes))
        elif t == SELF:
            if _depth > 30: raise RuntimeError("self loop")
            dest = self.read_hunk(self.offset[h], _depth+1)
        else:
            raise ValueError("unhandled hunk type %d at %d" % (t, h))
        self._cache[h] = dest
        if len(self._cache) > 64:
            self._cache.pop(next(iter(self._cache)))
        return dest

    def read_sector_full(self, lba):
        """Return the full 2352-byte sector-data area for absolute frame lba."""
        h = lba // self.frames_per_hunk
        fr = lba % self.frames_per_hunk
        hunk = self.read_hunk(h)
        return bytes(hunk[fr*FRAME: fr*FRAME + SECTOR_DATA])

if __name__ == '__main__':
    chd = CHD()
    print("hunkcount", chd.hunkcount, "frames/hunk", chd.frames_per_hunk)
    # Decompress a low hunk and search for CD001 to learn layout
    for lba in range(14, 20):
        sec = chd.read_sector_full(lba)
        idx = sec.find(b'CD001')
        printable = sec[:8].hex()
        print(f"LBA {lba}: CD001 at offset {idx} within 2352 ; first8={printable}")
        if idx != -1:
            base = idx - 1  # descriptor type byte precedes 'CD001'
            print("   volume desc type byte:", sec[base])
