# Disassembles part of an IOP module (.IRX) as it sits in IOP memory: applies the module's relocations at its load
# base (PS2Recomp prints it: "[IOP] loaded IRX id=N entry=... base=0x...") and labels calls into other modules'
# exports as library:ordinal (e.g. sifcmd:12 = sceSifSendCmd), matching VENDETTA_IOPLOG's [iop-call] lines.
# usage: python tools\irxdis.py <irx> <load base hex> <start hex> <end hex>
#   e.g. python tools\irxdis.py game\MODULES\SNDDRV.IRX 24200 2d600 2d650     (needs: pip install capstone)
import struct, sys, capstone
data = open(sys.argv[1], 'rb').read()
base = int(sys.argv[2], 16); start = int(sys.argv[3], 16); end = int(sys.argv[4], 16)
e_phoff, = struct.unpack_from('<I', data, 0x1C); e_shoff, = struct.unpack_from('<I', data, 0x20)
e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from('<HHHH', data, 0x2A)
img = None
for i in range(e_phnum):
    p_type, p_off, p_vaddr, _, p_filesz, p_memsz = struct.unpack_from('<IIIIII', data, e_phoff + i * e_phentsize)
    if p_type == 1:
        img = bytearray(data[p_off:p_off + p_filesz]) + bytearray(p_memsz - p_filesz); seg_vaddr = p_vaddr
assert img is not None
def r32(a): return struct.unpack_from('<I', img, a)[0]
def w32(a, v): struct.pack_into('<I', img, a, v & 0xFFFFFFFF)
secs = [struct.unpack_from('<IIIIIIIIII', data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
for s in secs:
    if s[1] != 9: continue  # SHT_REL
    off, size = s[4], s[5]; pending_hi = []
    for k in range(off, off + size, 8):
        r_off, r_info = struct.unpack_from('<II', data, k); t = r_info & 0xFF; a = r_off - seg_vaddr
        if t == 2: w32(a, r32(a) + base)
        elif t == 4:
            ins = r32(a); w32(a, (ins & 0xFC000000) | ((((ins & 0x3FFFFFF) << 2) + base) >> 2 & 0x3FFFFFF))
        elif t == 5: pending_hi.append(a)
        elif t == 6:
            lo = r32(a); lov = struct.unpack('<h', struct.pack('<H', lo & 0xFFFF))[0]
            for h in pending_hi:
                hi = r32(h); full = ((hi & 0xFFFF) << 16) + lov + base
                w32(h, (hi & 0xFFFF0000) | (((full + 0x8000) >> 16) & 0xFFFF))
            w32(a, (lo & 0xFFFF0000) | ((lov + base) & 0xFFFF)); pending_hi = []
# import stubs: table header 0x41e00000, 0, version, name[8], then (jr ra; addiu zero,zero,N) pairs
stubs = {}
for a in range(0, len(img) - 24, 4):
    if r32(a) == 0x41E00000 and r32(a + 4) == 0:
        name = img[a + 12:a + 20].split(b'\0')[0].decode('latin1'); p = a + 20
        while r32(p) == 0x03E00008:
            stubs[p + base] = f'{name}:{r32(p + 4) & 0xFFFF}'; p += 8
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata = True  # keep going past data words in the code
code = bytes(img[start - base:end - base])
for ins in md.disasm(code, start):
    note = ''
    if ins.mnemonic in ('jal', 'j'):
        t = int(ins.op_str, 16)
        if t in stubs: note = '   ; ' + stubs[t]
    print(f'{ins.address:08x}: {ins.mnemonic:8s} {ins.op_str}{note}')
