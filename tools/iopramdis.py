# Disassembles a raw IOP RAM dump (HCTP_IOPDUMP_SEC=N writes iop_ram.bin) between two addresses, labelling calls into
# IRX import stubs as library:ordinal (import tables: 0x41E00000 magic, 8-byte lib name, then "jr ra / addiu zero,N").
# usage: python tools\iopramdis.py <iop_ram.bin> <start hex> <end hex> [--calls]   (needs: pip install capstone)
import struct, sys, capstone

ram = open(sys.argv[1], 'rb').read()
start = int(sys.argv[2], 16); end = int(sys.argv[3], 16)
calls_only = '--calls' in sys.argv

# import stubs: after a 0x41E00000 word, +12 = 8-char name; stubs follow as pairs (jr $ra = 0x03E00008, 0x2400xxxx)
stubs = {}
for off in range(0, len(ram) - 64, 4):
    if struct.unpack_from('<I', ram, off)[0] != 0x41E00000:
        continue
    name = ram[off + 12:off + 20].split(b'\0')[0].decode('latin1', 'replace').strip()
    p = off + 20
    while p + 8 <= len(ram):
        a, b = struct.unpack_from('<II', ram, p)
        if a != 0x03E00008 or (b & 0xFFFF0000) != 0x24000000:
            break
        stubs[p] = '%s:%d' % (name, b & 0xFFFF)
        p += 8

md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
def insns():
    a = start
    while a < end:
        got = False
        for ins in md.disasm(ram[a:end], a):
            got = True
            a = ins.address + 4
            yield ins
        if not got or a < end:
            a += 0 if got else 4

for ins in insns():
    note = ''
    if ins.mnemonic in ('jal', 'j'):
        target = int(ins.op_str, 16)
        note = stubs.get(target, '')
    if calls_only and not note:
        continue
    print('%06x: %-8s %-30s %s' % (ins.address, ins.mnemonic, ins.op_str, note))
