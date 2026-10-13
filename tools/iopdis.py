"""Tiny R3000 disassembler for an IOP RAM dump. usage: iopdis.py dump.bin start end"""
import struct, sys

R = ['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
     's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']
ram = open(sys.argv[1], 'rb').read()
start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
w = lambda a: struct.unpack_from('<I', ram, a & 0x1FFFFF)[0]

# import stubs: 0x41e00000 magic, +12 name[8], then (jr ra, addiu zero,zero,N) pairs
stubs = {}
for a in range(0, len(ram) - 32, 4):
    if w(a) == 0x41e00000 and w(a + 4) == 0:
        name = ram[a + 12:a + 20].split(b'\0')[0].decode('latin1')
        p = a + 20
        while w(p) == 0x03e00008 and (w(p + 4) >> 16) == 0x2400:
            stubs[p] = f'{name}:{w(p + 4) & 0xffff}'
            p += 8

def s16(x): return x - 0x10000 if x & 0x8000 else x

for a in range(start, end, 4):
    i = w(a); op = i >> 26; rs = (i >> 21) & 31; rt = (i >> 16) & 31; rd = (i >> 11) & 31
    sh = (i >> 6) & 31; fn = i & 63; imm = i & 0xffff; si = s16(imm)
    t = f'.word {i:08x}'
    if i == 0: t = 'nop'
    elif op == 0:
        n = {0x21:'addu',0x23:'subu',0x24:'and',0x25:'or',0x26:'xor',0x27:'nor',0x2a:'slt',0x2b:'sltu',0x20:'add',0x22:'sub'}.get(fn)
        if n: t = f'{n} {R[rd]}, {R[rs]}, {R[rt]}'
        elif fn in (0, 2, 3): t = f'{["sll","","srl","sra"][fn]} {R[rd]}, {R[rt]}, {sh}'
        elif fn == 8: t = f'jr {R[rs]}'
        elif fn == 9: t = f'jalr {R[rd]}, {R[rs]}'
        elif fn in (0x10, 0x12): t = f'{"mfhi" if fn == 0x10 else "mflo"} {R[rd]}'
        elif fn in (0x18, 0x19, 0x1a, 0x1b): t = f'{["mult","multu","div","divu"][fn-0x18]} {R[rs]}, {R[rt]}'
        elif fn in (4, 6, 7): t = f'{["sllv","","srlv","srav"][fn-4]} {R[rd]}, {R[rt]}, {R[rs]}'
        elif fn == 0xc: t = 'syscall'
    elif op in (2, 3):
        tgt = (a & 0xf0000000) | ((i & 0x3ffffff) << 2)
        t = f'{"j" if op == 2 else "jal"} 0x{tgt:x}' + (f'   <{stubs[tgt]}>' if tgt in stubs else '')
    elif op in (4, 5): t = f'{"beq" if op == 4 else "bne"} {R[rs]}, {R[rt]}, 0x{a + 4 + si * 4:x}'
    elif op in (6, 7): t = f'{"blez" if op == 6 else "bgtz"} {R[rs]}, 0x{a + 4 + si * 4:x}'
    elif op == 1: t = f'{["bltz","bgez"][rt & 1]}{"al" if rt & 16 else ""} {R[rs]}, 0x{a + 4 + si * 4:x}'
    elif op == 0xf: t = f'lui {R[rt]}, 0x{imm:x}'
    elif op in (8, 9): t = f'addiu {R[rt]}, {R[rs]}, {si}'
    elif op in (0xa, 0xb): t = f'{"slti" if op == 0xa else "sltiu"} {R[rt]}, {R[rs]}, {si}'
    elif op in (0xc, 0xd, 0xe): t = f'{["andi","ori","xori"][op-0xc]} {R[rt]}, {R[rs]}, 0x{imm:x}'
    elif op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2b):
        n = {0x20:'lb',0x21:'lh',0x23:'lw',0x24:'lbu',0x25:'lhu',0x28:'sb',0x29:'sh',0x2b:'sw'}[op]
        t = f'{n} {R[rt]}, {si}({R[rs]})'
    elif op == 0x10: t = f'cop0 {i:08x}'
    print(f'{a:06x}: {i:08x}  {t}')
