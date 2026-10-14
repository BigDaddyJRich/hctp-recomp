# Dumps every HCTP VU1 program (tools/hctp_vu1_ranges.txt) as a readable listing: one file per program in logs/vu1map.
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vu1_recomp as R
elf, out = sys.argv[1], sys.argv[2]
ranges = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hctp_vu1_ranges.txt')).read().strip()
sets = []
for r in ranges.split(','):
    a, b = (int(x, 16) for x in r.split(':'))
    sets += R.program_sets(a, R.read_vaddr_range(elf, a, b))
def dest(i):
    d = R.DEST(i)
    return '.' + ''.join(c for b, c in ((8, 'x'), (4, 'y'), (2, 'z'), (1, 'w')) if d & b) if d else ''
def up_ops(u):
    n = R.upper_name(u)
    if n.startswith('NOP'):
        return ''
    return f'{dest(u)} d=vf{R.FD(u)} s=vf{R.FS(u)} t=vf{R.FT(u)}'
def lo_ops(l):
    if l == 0x8000033c:
        return ''
    op = l >> 25
    imm15 = (l & 0x7FF) | ((l >> 10) & 0x7800)
    if op == 0x40:  # lower special: register forms
        return f'{dest(l)} t={R.VIT(l)}/vf{R.FT(l)} s={R.VIS(l)}/vf{R.FS(l)} d={R.VID(l)}'
    # vf fields are 5 bits (FT / FS); vi fields 4 bits - both shown: LQ / SQ use ft / fs as vf, the integer ops vi
    return f'{dest(l)} vf_t={R.FT(l)} vf_s={R.FS(l)} it={R.VIT(l)} is={R.VIS(l)} imm11={R.IMM11(l)} imm15=0x{imm15:x}'

for n, st in enumerate(sets):
    code = st['code']
    with open(os.path.join(out, f'p{n:02d}.txt'), 'w') as f:
        f.write(f'program {n}: VU 0x{st["vu"]:04x}..0x{st["vu"] + len(code):04x} ({len(code) // 8} pairs), ELF 0x{st["source"]:06x}\n')
        for k in range(0, len(code), 8):
            lo, up = struct.unpack_from('<II', code, k)
            ib = (up >> 31) & 1; eb = (up >> 30) & 1
            lower = f'LOI 0x{lo:08x}' if ib else R.lower_name(lo)
            f.write(f'{st["vu"] + k:04x}: {R.upper_name(up) + up_ops(up):<44} | {lower}{"" if ib else lo_ops(lo)}{"   [E]" if eb else ""}\n')
    print(f'p{n:02d} vu=0x{st["vu"]:04x} pairs={len(code)//8} elf=0x{st["source"]:06x}')
