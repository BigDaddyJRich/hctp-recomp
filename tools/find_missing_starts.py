# Finds code addresses the game reaches through pointers in its data (function-pointer tables, constructor
# lists, vtables) that are not a function or label start in the Ghidra function map — the recompiler has no
# C++ for those, so a call through such a pointer stops the game ("guest-branch:missing-target").
# usage: python find_missing_starts.py <elf> <functions.csv> [--write extra.csv]
import csv, struct, sys

elf_path, csv_path = sys.argv[1], sys.argv[2]
out_path = sys.argv[sys.argv.index('--write') + 1] if '--write' in sys.argv else None
d = open(elf_path, 'rb').read()
shoff, = struct.unpack_from('<I', d, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2e)
secs = []
for i in range(shnum):
    name, typ, flags, addr, off, size = struct.unpack_from('<IIIIII', d, shoff + i * shentsize)
    secs.append((name, typ, flags, addr, off, size))
stroff = secs[shstrndx][4]
def nm(n): return d[stroff + n:d.index(b'\0', stroff + n)].decode()
code = [(a, a + s, o) for n, t, f, a, o, s in secs if f & 4 and t == 1]
data = [(nm(n), a, o, s) for n, t, f, a, o, s in secs if not f & 4 and f & 2 and t == 1]
def insn(a):
    for lo, hi, o in code:
        if lo <= a < hi: return struct.unpack_from('<I', d, o + a - lo)[0]
starts = set()
ends = {}
for row in csv.DictReader(open(csv_path)):
    s = int(row['Start'], 16); starts.add(s); ends[s] = int(row['End'], 16)
import re as _re
names = {int(r['Start'], 16): r['Name'] for r in csv.DictReader(open(csv_path))}
fun_starts = sorted(s for s, n in names.items() if n.startswith('FUN_') or not _re.match(r'(entry_|caseD_|switchD_|LAB_)', n))
fun_ranges = [(s, ends[s]) for s in fun_starts]
def inside_function(t): return any(s <= t < e for s, e in fun_ranges)
found = {}
for sname, a, o, size in data:
    for k in range(0, size - 3, 4):
        t = struct.unpack_from('<I', d, o + k)[0]
        if t & 3 or t in starts or insn(t) is None: continue
        prev2 = insn(t - 8)
        first = insn(t)
        # plausible function start: right after "jr ra; <delay>" or nop padding, or opens with addiu sp,sp,-N
        plausible = prev2 == 0x03e00008 or insn(t - 4) == 0 or (first >> 16) == 0x27bd and (first & 0x8000)
        # skip: already inside a known function (the recompiler makes an entry for it), or only named by the C++
        # exception tables (landing pads; only reached if the game throws)
        if plausible and not inside_function(t) and sname != '.gcc_except_table':
            found.setdefault(t, []).append((sname, a + k))
# the routine runs on past any label until the next real function
def end_of(t): return min((s for s in fun_starts if s > t), default=t + 4)
for t in sorted(found):
    end = end_of(t)
    print(f'0x{t:08X}  ends 0x{end:08X}  first={insn(t):08x}  from {", ".join(f"{s}@0x{x:X}" for s, x in found[t][:3])}')
print(len(found), 'missing starts')
if out_path:
    with open(out_path, 'w', newline='') as f:
        for t in sorted(found):
            end = end_of(t)
            f.write(f'FUN_{t:08x},0x{t:08X},0x{end:08X},{end - t}\n')
