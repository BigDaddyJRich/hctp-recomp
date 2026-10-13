# Small R5900 (EE) disassembler for the HCTP ELF: python -I tools\eedis.py <elf> <from_hex> <to_hex>
# Covers the integer ops libgraph-style code uses (incl. 64-bit daddu/dsll/ld/sd/lq/sq); others print as .word.
import struct
import sys

elf = open(sys.argv[1], 'rb').read()
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
phoff = struct.unpack_from('<I', elf, 0x1c)[0]
phnum = struct.unpack_from('<H', elf, 0x2c)[0]
segs = [struct.unpack_from('<6I', elf, phoff + k * 32) for k in range(phnum)]


def word(va):
    for _, off, sva, _, fs, _ in segs:
        if sva <= va < sva + fs:
            return struct.unpack_from('<I', elf, off + va - sva)[0]
    return 0


R = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
     's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
SPECIAL = {0x00: 'sll', 0x02: 'srl', 0x03: 'sra', 0x04: 'sllv', 0x06: 'srlv', 0x07: 'srav', 0x08: 'jr', 0x09: 'jalr',
           0x0a: 'movz', 0x0b: 'movn', 0x10: 'mfhi', 0x12: 'mflo', 0x18: 'mult', 0x19: 'multu', 0x1a: 'div',
           0x1b: 'divu', 0x20: 'add', 0x21: 'addu', 0x22: 'sub', 0x23: 'subu', 0x24: 'and', 0x25: 'or', 0x26: 'xor',
           0x27: 'nor', 0x2a: 'slt', 0x2b: 'sltu', 0x2c: 'dadd', 0x2d: 'daddu', 0x2f: 'dsubu', 0x38: 'dsll',
           0x3a: 'dsrl', 0x3b: 'dsra', 0x3c: 'dsll32', 0x3e: 'dsrl32', 0x3f: 'dsra32'}
IMM = {0x08: 'addi', 0x09: 'addiu', 0x0a: 'slti', 0x0b: 'sltiu', 0x0c: 'andi', 0x0d: 'ori', 0x0e: 'xori', 0x0f: 'lui',
       0x19: 'daddiu'}
MEM = {0x1e: 'lq', 0x1f: 'sq', 0x20: 'lb', 0x21: 'lh', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu', 0x28: 'sb', 0x29: 'sh',
       0x2b: 'sw', 0x37: 'ld', 0x3f: 'sd', 0x31: 'lwc1', 0x39: 'swc1'}
BR = {0x04: 'beq', 0x05: 'bne', 0x06: 'blez', 0x07: 'bgtz', 0x14: 'beql', 0x15: 'bnel'}
for a in range(lo, hi, 4):
    w = word(a)
    op, rs, rt, rd, sh, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xffff
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if w == 0:
        t = 'nop'
    elif op == 0 and fn in SPECIAL:
        n = SPECIAL[fn]
        if n in ('sll', 'srl', 'sra', 'dsll', 'dsrl', 'dsra', 'dsll32', 'dsrl32', 'dsra32'):
            t = f'{n} {R[rd]}, {R[rt]}, {sh}'
        elif n in ('jr',):
            t = f'jr {R[rs]}'
        elif n == 'jalr':
            t = f'jalr {R[rd]}, {R[rs]}'
        elif n in ('mfhi', 'mflo'):
            t = f'{n} {R[rd]}'
        elif n in ('mult', 'multu', 'div', 'divu'):
            t = f'{n} {R[rd]}, {R[rs]}, {R[rt]}'
        else:
            t = f'{n} {R[rd]}, {R[rs]}, {R[rt]}'
    elif op in IMM:
        t = f'{IMM[op]} {R[rt]}, {R[rs]}, {simm if op not in (0x0c, 0x0d, 0x0e, 0x0f) else hex(imm)}'
    elif op in MEM:
        t = f'{MEM[op]} {R[rt]}, {simm}({R[rs]})'
    elif op in BR:
        t = f'{BR[op]} {R[rs]}, {R[rt]}, {a + 4 + simm * 4:#x}'
    elif op == 1:
        t = f'regimm{rt} {R[rs]}, {a + 4 + simm * 4:#x}'
    elif op in (2, 3):
        t = f'{"j" if op == 2 else "jal"} {((a + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2):#x}'
    elif op == 0x1c and fn == 0x18:
        t = f'mult1 {R[rd]}, {R[rs]}, {R[rt]}'
    else:
        t = f'.word {w:08x}'
    print(f'{a:06x}: {w:08x}  {t}')
