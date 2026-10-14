# VU1 recompiler: the game's VU1 microprograms (the MPG uploads in SLUS_206.39's .vutext) -> C++
# (generated\vu1_compiled.cpp), which PS2Recomp's VU1 runs instead of interpreting while VU1 code memory holds exactly
# that code (PS2Recomp ps2xRuntime/include/runtime/ps2_vu1_compiled.h). Each instruction pair becomes straight C++ with
# its registers, lanes, latencies and branch targets as constants and its branches as jumps; the timing model is the
# interpreter's fast mode, unchanged (same stalls, same Q / clip / store / XGKICK pipelines, same cycle count). The
# decode below is a port of VU1Interpreter's decodeUpperUsage / decodeLowerUsage / decodeInstructionPair; what it
# assumed per pair is written out too and checked against the interpreter's own decode before a piece is used.
# VENDETTA_VU1_VERIFY=1 compares every program with the original model; VENDETTA_VU1_COMPILED=0 turns it off.
# The timing bookkeeping is kept cheap: the cycle lives in a local (written through at every advance, reloaded after a
# stall), a pair's ready-cycle reads are a test of "any read lane not ready" (one 256-bit compare per register row
# with AVX2; the interpreter's own stall loop still does the stalling), and its write marks are whole-row stores.
# The VU maths runs on all four lanes at once (vu1v:: in ps2_vu1_compiled.h: per lane the same operations as the
# scalar vu1fast:: rules, in the same order) and every register is written whole.
# The output is made from the game's code: it goes to generated\ (never committed).
# Usage: python tools\vu1_recomp.py [elf] [out.cpp]   (recomp.bat runs it)
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get('VENDETTA_DATA') or ROOT, 'game', 'SLUS_206.39')
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'generated', 'vu1_compiled.cpp')
CODE_SIZE = 0x4000       # VU1 code memory
CHUNK_PAIRS = 256        # pairs per C++ function (jumps between functions go back through the VU1 loop)


# ---------------------------------------------------------------------------------------------------------------------
# The programs: .vutext is a VIF packet stream; MPG (0x4A) uploads n (0 = 256) instruction pairs to VU address imm * 8.
# Uploads that continue each other's VU address form one program set (one piece).
def read_vutext(path):
    d = open(path, 'rb').read()
    e_shoff = struct.unpack_from('<I', d, 0x20)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x2E)

    def sh(i):
        return struct.unpack_from('<10I', d, e_shoff + i * e_shentsize)

    strtab = sh(e_shstrndx)[4]

    def name(off):
        return d[strtab + off:d.index(b'\0', strtab + off)].decode('latin1')

    for i in range(e_shnum):
        s = sh(i)
        if name(s[0]) == '.vutext':
            return s[3], d[s[4]:s[4] + s[5]]
    sys.exit('vu1_recomp: no .vutext section in ' + path)


def read_vaddr_range(path, start, end):
    d = open(path, 'rb').read()
    e_phoff = struct.unpack_from('<I', d, 0x1C)[0]
    e_phnum = struct.unpack_from('<H', d, 0x2C)[0]
    for k in range(e_phnum):
        _, off, va, _, fs, _ = struct.unpack_from('<6I', d, e_phoff + k * 32)
        if va <= start and end <= va + fs:
            return d[off + start - va:off + end - va]
    sys.exit(f'vu1_recomp: range {start:#x}-{end:#x} not in a loadable segment')


def program_sets(vaddr, v):
    blocks, o = [], 0
    while o + 4 <= len(v):
        imm, num, cmd = struct.unpack_from('<HBB', v, o)
        c = cmd & 0x7F
        o += 4
        if c == 0x4A:
            n = num or 256
            blocks.append((vaddr + o, imm * 8, v[o:o + n * 8]))
            o += n * 8
        elif c in (0x50, 0x51):
            o += (imm or 65536) * 16
        elif c == 0x20:
            o += 4
        elif c in (0x30, 0x31):
            o += 16
        elif c >= 0x60:
            vn, vl, cnt = (c >> 2) & 3, c & 3, num or 256
            o += ((cnt * (vn + 1) * (32 >> vl) // 8) + 3) & ~3
    sets = []
    for source, vu, code in blocks:
        if sets and sets[-1]['vu'] + len(sets[-1]['code']) == vu:
            sets[-1]['code'] += code
        else:
            sets.append({'source': source, 'vu': vu, 'code': code})
    return sets


# ---------------------------------------------------------------------------------------------------------------------
# The decode (VU1Interpreter's, ported line for line)
NONE, FMAC, LSU, FDIV, EFU, IALU, BRANCH, XGKICK = range(8)
FMAC_LATENCY = 4


def lane(c):
    return 1 << (3 - c)


def DEST(i): return (i >> 21) & 0xF
def FT(i): return (i >> 16) & 0x1F
def FS(i): return (i >> 11) & 0x1F
def FD(i): return (i >> 6) & 0x1F
def VIT(i): return (i >> 16) & 0xF
def VIS(i): return (i >> 11) & 0xF
def VID(i): return (i >> 6) & 0xF


def IMM11(i):
    v = i & 0x7FF
    return v - 0x800 if v & 0x400 else v


class Usage:
    def __init__(self):
        self.vf_read = []        # [reg, lanes], at most 2
        self.vf_write = [0, 0]
        self.vi_read = self.vi_write = 0
        self.acc_read = self.acc_write = 0
        self.latency = self.vf_latency = self.vi_latency = 0
        self.pipeline = NONE
        self.wait_q = self.wait_p = self.delays_branch = self.reserved = False


def add_vf_read(u, reg, lanes):
    if lanes == 0:
        return
    for r in u.vf_read:
        if r[0] == reg:
            r[1] |= lanes
            return
    if len(u.vf_read) < 2:
        u.vf_read.append([reg, lanes])


def add_vf_write(u, reg, lanes):
    if reg == 0 or lanes == 0:
        return
    if u.vf_write[0] == 0:
        u.vf_write = [reg, lanes]
    elif u.vf_write[0] == reg:
        u.vf_write[1] |= lanes


def vf_read_lanes(u, reg):
    for r in u.vf_read:
        if r[0] == reg:
            return r[1]
    return 0


def upper_special(i):
    return (i & 3) | ((i >> 4) & 0x7C)


def decode_upper(upper):
    u = Usage()
    u.pipeline, u.latency = FMAC, FMAC_LATENCY
    op, dest, fs, ft, fd = upper & 0x3F, DEST(upper), FS(upper), FT(upper), FD(upper)
    if op <= 0x2F:
        add_vf_read(u, fs, dest)
        add_vf_write(u, fd, dest)
        if op <= 0x1B:
            add_vf_read(u, ft, lane(op & 3))
        elif op >= 0x28:
            add_vf_read(u, ft, 0xE if op == 0x2E else dest)
        if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E):
            u.acc_read = dest
        return u
    if op >= 0x3C:
        sp = upper_special(upper)
        writes_acc = sp <= 0x0F or 0x18 <= sp <= 0x1C or sp == 0x1E or 0x20 <= sp <= 0x2A or 0x2C <= sp <= 0x2E
        if writes_acc:
            add_vf_read(u, fs, dest)
            if sp <= 0x1B:
                add_vf_read(u, ft, lane(sp & 3))
            elif 0x28 <= sp <= 0x2E:
                add_vf_read(u, ft, 0xE if sp == 0x2E else dest)
            u.acc_write = dest
            if 0x08 <= sp <= 0x0F or sp in (0x21, 0x23, 0x25, 0x27, 0x29, 0x2D):
                u.acc_read = dest
        elif 0x10 <= sp <= 0x17 or sp == 0x1D:
            add_vf_read(u, fs, dest)
            add_vf_write(u, ft, dest)
        elif sp == 0x1F:
            add_vf_read(u, fs, 0xE)
            add_vf_read(u, ft, 0x1)
        elif sp not in (0x2F, 0x30):
            u.reserved = True
        return u
    u.reserved = True
    return u


def lower_special(i):
    return (i & 3) | ((i >> 4) & 0x7C)


def decode_lower(lower):
    u = Usage()
    if lower in (0, 0x8000033C):
        return u
    op = (lower >> 25) & 0x7F
    vft, vfs, vit, vis, vid, dest = FT(lower), FS(lower), VIT(lower), VIS(lower), VID(lower), DEST(lower)

    def rvi(r):
        if r:
            u.vi_read |= 1 << r

    def wvi(r):
        if r:
            u.vi_write |= 1 << r

    def set_(pipe, lat=0):
        u.pipeline, u.latency = pipe, lat

    if op == 0x00:
        set_(LSU, 4); rvi(vis); add_vf_write(u, vft, dest)
    elif op == 0x01:
        set_(LSU, 1); rvi(vit); add_vf_read(u, vfs, dest)
    elif op == 0x04:
        set_(LSU, 4); rvi(vis); wvi(vit)
    elif op == 0x05:
        set_(LSU, 1); rvi(vis); rvi(vit)
    elif op in (0x08, 0x09):
        set_(IALU, 1); u.delays_branch = True; rvi(vis); wvi(vit)
    elif op in (0x10, 0x12, 0x13):
        set_(IALU, 1); wvi(1)
    elif op == 0x11:
        set_(FMAC, FMAC_LATENCY)
    elif op in (0x14, 0x16, 0x17):
        set_(IALU, 1); wvi(vit)
    elif op == 0x15:
        set_(FMAC, FMAC_LATENCY)
    elif op in (0x18, 0x1A, 0x1B):
        set_(IALU, 1); rvi(vis); wvi(vit)
    elif op == 0x1C:
        set_(IALU, 1); wvi(vit)
    elif op == 0x20:
        set_(BRANCH)
    elif op == 0x21:
        set_(BRANCH, 1); wvi(vit)
    elif op == 0x24:
        set_(BRANCH); rvi(vis)
    elif op == 0x25:
        set_(BRANCH, 1); rvi(vis); wvi(vit)
    elif op in (0x28, 0x29):
        set_(BRANCH); rvi(vis); rvi(vit)
    elif op in (0x2C, 0x2D, 0x2E, 0x2F):
        set_(BRANCH); rvi(vis)
    elif op != 0x40:
        u.reserved = True
    else:
        direct = lower & 0x3F
        if direct in (0x30, 0x31, 0x34, 0x35):
            set_(IALU, 1); u.delays_branch = True; rvi(vis); rvi(vit); wvi(vid)
        elif direct == 0x32:
            set_(IALU, 1); u.delays_branch = True; rvi(vis); wvi(vit)
        elif direct < 0x3C:
            u.reserved = True
        else:
            sp = lower_special(lower)
            if sp in (0x30, 0x31):
                set_(FMAC, 4); add_vf_read(u, vfs, 0xF if sp == 0x31 else dest); add_vf_write(u, vft, dest)
            elif sp in (0x34, 0x36):
                set_(LSU, 4); u.vi_latency = 1; u.delays_branch = True; rvi(vis); wvi(vis); add_vf_write(u, vft, dest)
            elif sp in (0x35, 0x37):
                set_(LSU, 1); u.delays_branch = True; rvi(vit); wvi(vit); add_vf_read(u, vfs, dest)
            elif sp == 0x38:
                set_(FDIV, 7); add_vf_read(u, vfs, lane((lower >> 21) & 3)); add_vf_read(u, vft, lane((lower >> 23) & 3))
            elif sp == 0x39:
                set_(FDIV, 7); add_vf_read(u, vft, lane((lower >> 23) & 3))
            elif sp == 0x3A:
                set_(FDIV, 13); add_vf_read(u, vfs, lane((lower >> 21) & 3)); add_vf_read(u, vft, lane((lower >> 23) & 3))
            elif sp == 0x3B:
                set_(FDIV); u.wait_q = True
            elif sp == 0x3C:
                set_(IALU, 1); u.delays_branch = True; add_vf_read(u, vfs, lane((lower >> 21) & 3)); wvi(vit)
            elif sp == 0x3D:
                set_(FMAC, 4); rvi(vis); add_vf_write(u, vft, dest)
            elif sp == 0x3E:
                set_(LSU, 4); rvi(vis); wvi(vit)
            elif sp == 0x3F:
                set_(LSU, 1); rvi(vis); rvi(vit)
            elif sp in (0x40, 0x41, 0x64):
                set_(FMAC, 4); add_vf_write(u, vft, dest)
            elif sp in (0x42, 0x43):
                set_(IALU, 1); add_vf_read(u, vfs, lane((lower >> 21) & 3))
            elif sp in (0x68, 0x69):
                set_(IALU, 1); wvi(vit)
            elif sp == 0x6C:
                set_(XGKICK, 2); rvi(vis)
            elif 0x70 <= sp <= 0x7D and sp != 0x7B:
                lat = {0x70: 11, 0x71: 18, 0x72: 18, 0x77: 18, 0x73: 24, 0x74: 54, 0x75: 54, 0x7C: 54,
                       0x76: 12, 0x78: 12, 0x7A: 12, 0x79: 29, 0x7D: 44}[sp]
                set_(EFU, lat)
                if sp <= 0x73:
                    add_vf_read(u, vfs, 0xE)
                elif sp == 0x74:
                    add_vf_read(u, vfs, 0xC)
                elif sp == 0x75:
                    add_vf_read(u, vfs, 0xA)
                elif sp == 0x76:
                    add_vf_read(u, vfs, 0xF)
                else:
                    add_vf_read(u, vfs, lane((lower >> 21) & 3))
            elif sp == 0x7B:
                set_(EFU); u.wait_p = True
            else:
                u.reserved = True
    return u


class Pair:
    def __init__(self, address, lower, upper):
        self.address, self.index, self.lower, self.upper = address, address // 8, lower, upper
        self.i_bit, self.e_bit = bool(upper >> 31 & 1), bool(upper >> 30 & 1)
        self.d_bit, self.t_bit = bool(upper >> 28 & 1), bool(upper >> 27 & 1)
        self.up = decode_upper(upper)
        self.lo = decode_lower(lower) if not self.i_bit else Usage()
        self.lower_vi_write = next((r for r in range(1, 16) if self.lo.vi_write & (1 << r)), 0)
        w = self.up.vf_write[0]
        self.shadow = self.suppressed = 0
        if w and (vf_read_lanes(self.lo, w) or self.lo.vf_write[0] == w):
            self.shadow = w
            if self.lo.vf_write[0] == w:
                self.suppressed = w
        self.stall_vf = [tuple(r) for u in (self.up, self.lo) for r in u.vf_read if r[0] and r[1]][:4]
        self.stall_vi = (self.up.vi_read | self.lo.vi_read) & 0xFFFE
        self.stall_acc = self.up.acc_read | self.lo.acc_read
        lw = self.lo.vf_write
        self.mark_lower = (lw[0], lw[1]) if lw[0] and self.suppressed != lw[0] else (0, 0)
        self.mark_lower_lat = (self.lo.vf_latency or self.lo.latency) if self.mark_lower[0] else 0
        self.mark_upper = tuple(self.up.vf_write) if self.up.vf_write[0] else (0, 0)
        self.mark_upper_lat = (self.up.vf_latency or self.up.latency) if self.mark_upper[0] else 0
        self.mark_vi = self.lo.vi_write & 0xFFFE
        self.mark_vi_lat = self.lo.vi_latency or self.lo.latency
        self.mark_acc = self.up.acc_write
        # fast mode: when this pair's register writes would have landed (markPairWrites' settle cycle)
        settle = []
        if self.mark_lower[0]:
            settle.append(self.mark_lower_lat)
        if self.mark_upper[0]:
            settle.append(self.mark_upper_lat)
        if self.lower_vi_write:
            settle.append(self.mark_vi_lat)
        if self.mark_acc:
            settle.append(1)
        self.settle = max(settle) if settle else None
        self.reserved = self.up.reserved or self.lo.reserved
        self.branch = not self.i_bit and self.lo.pipeline == BRANCH

    def shape(self):
        reads = list(self.stall_vf) + [(0, 0)] * (4 - len(self.stall_vf))
        flags = ((1 if self.lo.wait_q else 0) | (2 if self.lo.wait_p else 0) | (4 if self.i_bit else 0) |
                 (8 if self.e_bit else 0) | (16 if self.d_bit else 0) | (32 if self.t_bit else 0) |
                 (64 if self.reserved else 0) | (128 if self.lo.delays_branch else 0))
        return ('{{' + ', '.join('{%d, %d}' % r for r in reads) + '}, %d, 0x%x, 0x%x, %d, 0x%x, {%d, %d}, %d, {%d, %d}, %d, '
                '0x%x, %d, 0x%x, %d, %d, %d}') % (
            len(self.stall_vf), self.stall_vi, self.stall_acc, self.lo.pipeline, flags,
            self.mark_lower[0], self.mark_lower[1], self.mark_lower_lat, self.mark_upper[0], self.mark_upper[1],
            self.mark_upper_lat, self.mark_vi, self.mark_vi_lat, self.mark_acc, self.lower_vi_write, self.shadow,
            self.suppressed)


# ---------------------------------------------------------------------------------------------------------------------
# Instruction names for the comments
UPPER_NAMES = {0x1C: 'MULq', 0x1D: 'MAXi', 0x1E: 'MULi', 0x1F: 'MINIi', 0x20: 'ADDq', 0x21: 'MADDq', 0x22: 'ADDi',
               0x23: 'MADDi', 0x24: 'SUBq', 0x25: 'MSUBq', 0x26: 'SUBi', 0x27: 'MSUBi', 0x28: 'ADD', 0x29: 'MADD',
               0x2A: 'MUL', 0x2B: 'MAX', 0x2C: 'SUB', 0x2D: 'MSUB', 0x2E: 'OPMSUB', 0x2F: 'MINI'}
UPPER_SPECIAL_NAMES = {0x10: 'ITOF0', 0x11: 'ITOF4', 0x12: 'ITOF12', 0x13: 'ITOF15', 0x14: 'FTOI0', 0x15: 'FTOI4',
                       0x16: 'FTOI12', 0x17: 'FTOI15', 0x1C: 'MULAq', 0x1D: 'ABS', 0x1E: 'MULAi', 0x1F: 'CLIP',
                       0x20: 'ADDAq', 0x21: 'MADDAq', 0x22: 'ADDAi', 0x23: 'MADDAi', 0x24: 'SUBAq', 0x25: 'MSUBAq',
                       0x26: 'SUBAi', 0x27: 'MSUBAi', 0x28: 'ADDA', 0x29: 'MADDA', 0x2A: 'MULA', 0x2C: 'SUBA',
                       0x2D: 'MSUBA', 0x2E: 'OPMULA', 0x2F: 'NOP', 0x30: 'NOP'}
LOWER_NAMES = {0x00: 'LQ', 0x01: 'SQ', 0x04: 'ILW', 0x05: 'ISW', 0x08: 'IADDIU', 0x09: 'ISUBIU', 0x10: 'FCEQ',
               0x11: 'FCSET', 0x12: 'FCAND', 0x13: 'FCOR', 0x14: 'FSEQ', 0x15: 'FSSET', 0x16: 'FSAND', 0x17: 'FSOR',
               0x18: 'FMEQ', 0x1A: 'FMAND', 0x1B: 'FMOR', 0x1C: 'FCGET', 0x20: 'B', 0x21: 'BAL', 0x24: 'JR',
               0x25: 'JALR', 0x28: 'IBEQ', 0x29: 'IBNE', 0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}
LOWER_DIRECT_NAMES = {0x30: 'IADD', 0x31: 'ISUB', 0x32: 'IADDI', 0x34: 'IAND', 0x35: 'IOR'}
LOWER_SPECIAL_NAMES = {0x30: 'MOVE', 0x31: 'MR32', 0x34: 'LQI', 0x35: 'SQI', 0x36: 'LQD', 0x37: 'SQD', 0x38: 'DIV',
                       0x39: 'SQRT', 0x3A: 'RSQRT', 0x3B: 'WAITQ', 0x3C: 'MTIR', 0x3D: 'MFIR', 0x3E: 'ILWR',
                       0x3F: 'ISWR', 0x40: 'RNEXT', 0x41: 'RGET', 0x42: 'RINIT', 0x43: 'RXOR', 0x64: 'MFP',
                       0x68: 'XTOP', 0x69: 'XITOP', 0x6C: 'XGKICK', 0x70: 'ESADD', 0x71: 'ERSADD', 0x72: 'ELENG',
                       0x73: 'ERLENG', 0x74: 'EATANxy', 0x75: 'EATANxz', 0x76: 'ESUM', 0x77: 'ERSQRT', 0x78: 'ESQRT',
                       0x79: 'ESIN', 0x7A: 'ERCPR', 0x7B: 'WAITP', 0x7C: 'EATAN', 0x7D: 'EEXP'}
BC = 'xyzw'


def upper_name(i):
    op = i & 0x3F
    if op < 0x1C:
        return ['ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL'][op >> 2] + BC[op & 3]
    if op < 0x3C:
        return UPPER_NAMES.get(op, 'reserved')
    sp = upper_special(i)
    if sp <= 0x0F:
        return ['ADDA', 'SUBA', 'MADDA', 'MSUBA'][sp >> 2] + BC[sp & 3]
    if 0x18 <= sp <= 0x1B:
        return 'MULA' + BC[sp & 3]
    return UPPER_SPECIAL_NAMES.get(sp, 'reserved')


def lower_name(i):
    if i in (0, 0x8000033C):
        return 'NOP'
    op = (i >> 25) & 0x7F
    if op != 0x40:
        return LOWER_NAMES.get(op, 'reserved')
    direct = i & 0x3F
    if direct < 0x3C:
        return LOWER_DIRECT_NAMES.get(direct, 'reserved')
    return LOWER_SPECIAL_NAMES.get(lower_special(i), 'reserved')


# ---------------------------------------------------------------------------------------------------------------------
# Code for the instructions (exactly what VU1Interpreter does in fast mode with no MAC / status flag readers:
# execUpperFast, else execUpper; execLower)
def lanes_of(dest):
    return [c for c in range(4) if dest & lane(c)]


def f32_normalised_bits(bits):
    exponent = bits & 0x7F800000
    if exponent == 0:
        return bits & 0x80000000
    if exponent == 0x7F800000:
        return (bits & 0x80000000) | 0x7F7FFFFF
    return bits


class Emitted:
    def __init__(self):
        self.lines = []
        self.writes_vf0 = self.writes_vi0 = False
        self.generic = None        # the instruction left to the interpreter, if any
        self.branch = None         # None, ('always', target), ('if', target), ('jump',)


FAST_UPPER = {0x1C: ('mul', 'q'), 0x1D: ('max', 'i'), 0x1E: ('mul', 'i'), 0x1F: ('mini', 'i'), 0x20: ('add', 'q'),
              0x21: ('madd', 'q'), 0x22: ('add', 'i'), 0x23: ('madd', 'i'), 0x24: ('sub', 'q'),
              0x25: ('msub', 'q'), 0x26: ('sub', 'i'), 0x27: ('msub', 'i'), 0x28: ('add', 'lane'),
              0x29: ('madd', 'lane'), 0x2A: ('mul', 'lane'), 0x2B: ('max', 'lane'), 0x2C: ('sub', 'lane'),
              0x2D: ('msub', 'lane'), 0x2F: ('mini', 'lane')}
FAST_UPPER_ACC = {0x1C: ('mul', 'q'), 0x1E: ('mul', 'i'), 0x20: ('add', 'q'), 0x21: ('madd', 'q'),
                  0x22: ('add', 'i'), 0x23: ('madd', 'i'), 0x24: ('sub', 'q'), 0x25: ('msub', 'q'),
                  0x26: ('sub', 'i'), 0x27: ('msub', 'i'), 0x28: ('add', 'lane'), 0x29: ('madd', 'lane'),
                  0x2A: ('mul', 'lane'), 0x2C: ('sub', 'lane'), 0x2D: ('msub', 'lane')}


THROUGHPUT = False   # HCTP_VU1_FAST variant: no vf/vi/acc stall/ready/settle bookkeeping, plain float MADD/MSUB


def emit_upper(e, address, upper):
    op, dest, fs, ft, fd = upper & 0x3F, DEST(upper), FS(upper), FT(upper), FD(upper)
    kind = None
    if op < 0x3C:
        if op <= 0x1B:
            kind, source, bc, to_acc = ['add', 'sub', 'madd', 'msub', 'max', 'mini', 'mul'][op >> 2], 'bc', op & 3, False
        elif op in FAST_UPPER:
            (kind, source), bc, to_acc = FAST_UPPER[op], 0, False
    else:
        sp = upper_special(upper)
        if sp in (0x2F, 0x30):
            return                                                      # NOP
        if 0x14 <= sp <= 0x17:                                          # FTOI0 / 4 / 12 / 15 -> vt
            scale = ['1.0f', '16.0f', '4096.0f', '32768.0f'][sp & 3]
            if not lanes_of(dest):
                return
            e.lines.append(f'vu1v::store<0x{components(dest):x}u>(s.vf[{ft}], vu1v::floatToInt(vu1v::operand(vu1v::load(s.vf[{fs}])), {scale}));')
            e.writes_vf0 |= ft == 0
            return
        if 0x10 <= sp <= 0x13:                                          # ITOF0 / 4 / 12 / 15 -> vt (execUpper)
            div = ['', ', 16.0f', ', 4096.0f', ', 32768.0f'][sp & 3]
            if not lanes_of(dest):
                return
            e.lines.append(f'vu1v::store<0x{components(dest):x}u>(s.vf[{ft}], vu1v::intToFloat(vu1v::load(s.vf[{fs}]){div}));')
            e.writes_vf0 |= ft == 0
            return
        if sp == 0x1D:                                                  # ABS -> vt (execUpper)
            if not lanes_of(dest):
                return
            e.lines.append(f'vu1v::store<0x{components(dest):x}u>(s.vf[{ft}], vu1v::abs(vu1v::operand(vu1v::load(s.vf[{fs}]))));')
            e.writes_vf0 |= ft == 0
            return
        if sp == 0x1F:                                                  # CLIP (execUpper)
            e.lines.append(f'Vu1Compiled::queueClip(vu, vu1fast::clipFlags(s.vf[{fs}], s.vf[{ft}]));')
            return
        if sp <= 0x0F:
            kind, source, bc, to_acc = ['add', 'sub', 'madd', 'msub'][sp >> 2], 'bc', sp & 3, True
        elif 0x18 <= sp <= 0x1B:
            kind, source, bc, to_acc = 'mul', 'bc', sp & 3, True
        elif sp in FAST_UPPER_ACC:
            (kind, source), bc, to_acc = FAST_UPPER_ACC[sp], 0, True
    if kind is None:                                                    # OPMSUB / OPMULA: the interpreter
        e.lines.append(f's.pc = 0x{address:04x}u;')
        e.lines.append(f'Vu1Compiled::runUpper(vu, 0x{upper:08x}u); // {upper_name(upper)}')
        e.generic = upper_name(upper)
        e.writes_vf0 = True
        return
    if not lanes_of(dest):
        return
    mask = components(dest)
    e.lines.append('{')
    e.lines.append(f'    const vu1v::V a = vu1v::operand(vu1v::load(s.vf[{fs}]));')
    if source == 'bc':
        e.lines.append(f'    const vu1v::V b = vu1v::splat(vu1fast::operand(s.vf[{ft}][{bc}]));')
    elif source == 'q':
        e.lines.append('    const vu1v::V b = vu1v::splat(vu1fast::operand(s.q));')
    elif source == 'i':
        e.lines.append('    const vu1v::V b = vu1v::splat(vu1fast::operand(s.i));')
    else:
        e.lines.append(f'    const vu1v::V b = vu1v::operand(vu1v::load(s.vf[{ft}]));')
    if kind in ('add', 'sub', 'mul'):
        result = f'vu1v::operand(vu1v::{kind}(a, b))'
    elif kind in ('max', 'mini'):
        result = f'vu1v::{"max" if kind == "max" else "min"}(a, b)'
    elif THROUGHPUT:
        result = f'vu1v::operand(vu1v::{"add" if kind == "madd" else "sub"}(vu1v::operand(vu1v::load(s.acc)), vu1v::mul(a, b)))'
    else:
        result = (f'vu1v::fused<0x{mask:x}u, {"false" if kind == "madd" else "true"}>'
                  f'(vu1v::operand(vu1v::load(s.acc)), a, b)')
    target = 's.acc' if to_acc else f's.vf[{fd}]'
    e.lines.append(f'    vu1v::store<0x{mask:x}u>({target}, {result});')
    e.lines.append('}')
    if not to_acc:
        e.writes_vf0 |= fd == 0


def load_lanes(e, reg, dest, src):
    ls = lanes_of(dest)
    if ls == [0, 1, 2, 3]:
        e.lines.append(f'    std::memcpy(s.vf[{reg}], {src}, 16);')
    elif ls:
        e.lines.append(f'    vu1v::store<0x{components(dest):x}u>(s.vf[{reg}], vu1v::loadBytes({src}));')
    e.writes_vf0 |= reg == 0 and bool(ls)


def branch_vi(reg):
    if reg == 0:
        return '0'
    return f'(bkValid && bkReg == {reg} ? bkValue : s.vi[{reg}])'


def emit_lower(e, address, lower, upper):
    if lower in (0, 0x8000033C):
        return
    op = (lower >> 25) & 0x7F
    vft, vfs, vit, vis, vid, dest = FT(lower), FS(lower), VIT(lower), VIS(lower), VID(lower), DEST(lower)
    imm = IMM11(lower)
    L = e.lines

    def first_lane():
        return 0 if dest & 8 else 1 if dest & 4 else 2 if dest & 2 else 3

    def generic():
        L.append(f's.pc = 0x{address:04x}u;')
        L.append(f'Vu1Compiled::execLower(vu, 0x{lower:08x}u, 0x{upper:08x}u); // {lower_name(lower)}')
        e.generic = lower_name(lower)
        e.writes_vf0 = e.writes_vi0 = True

    target = (address + 8 + imm * 8) & 0x3FFF
    if op == 0x00:                                                      # LQ
        L.append('{')
        L.append(f'    const uint32_t addr = (static_cast<uint32_t>(s.vi[{vis}] + ({imm})) * 16u) & 0x3FFFu;')
        load_lanes(e, vft, dest, 'data + addr')
        L.append('}')
    elif op == 0x01:                                                    # SQ
        L.append('{')
        L.append(f'    const uint32_t addr = (static_cast<uint32_t>(s.vi[{vit}] + ({imm})) * 16u) & 0x3FFFu;')
        L.append(f'    uint32_t w[4]; std::memcpy(w, s.vf[{vfs}], 16);')
        L.append(f'    Vu1Compiled::queueStore(vu, addr, w, {dest});')
        L.append('}')
    elif op == 0x04:                                                    # ILW
        if vit:
            L.append('{')
            L.append(f'    const uint32_t addr = (static_cast<uint32_t>(s.vi[{vis}] + ({imm})) * 16u) & 0x3FFFu;')
            L.append(f'    uint32_t v; std::memcpy(&v, data + addr + {first_lane() * 4}, 4);')
            L.append(f'    s.vi[{vit}] = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));')
            L.append('}')
    elif op == 0x05:                                                    # ISW
        L.append('{')
        L.append(f'    const uint32_t addr = (static_cast<uint32_t>(s.vi[{vis}] + ({imm})) * 16u) & 0x3FFFu;')
        L.append(f'    const uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>(s.vi[{vit}] & 0xFFFF));')
        L.append('    const uint32_t w[4] = {v, v, v, v};')
        L.append(f'    Vu1Compiled::queueStore(vu, addr, w, {dest});')
        L.append('}')
    elif op in (0x08, 0x09):                                            # IADDIU / ISUBIU
        imm15 = (lower & 0x7FF) | ((lower >> 10) & 0x7800)
        if vit:
            sym = '+' if op == 0x08 else '-'
            L.append(f's.vi[{vit}] = static_cast<int16_t>(s.vi[{vis}] {sym} {imm15});')
    elif op == 0x10:                                                    # FCEQ
        L.append(f's.vi[1] = ((s.clip & 0xFFFFFFu) == 0x{lower & 0xFFFFFF:x}u) ? 1 : 0;')
    elif op == 0x11:                                                    # FCSET
        L.append(f'Vu1Compiled::queueFcset(vu, 0x{lower & 0xFFFFFF:x}u);')
    elif op == 0x12:                                                    # FCAND
        L.append(f's.vi[1] = ((s.clip & 0x{lower & 0xFFFFFF:x}u) != 0u) ? 1 : 0;')
    elif op == 0x13:                                                    # FCOR
        L.append(f's.vi[1] = ((s.clip | 0x{lower & 0xFFFFFF:x}u) == 0xFFFFFFu) ? 1 : 0;')
    elif op == 0x1C:                                                    # FCGET
        if vit:
            L.append(f's.vi[{vit}] = static_cast<int32_t>(s.clip & 0x0FFFu);')
    elif op == 0x20:                                                    # B
        e.branch = ('always', target)
    elif op == 0x21:                                                    # BAL
        if vit:
            L.append(f's.vi[{vit}] = {(address + 16) // 8};')
        e.branch = ('always', target)
    elif op in (0x24, 0x25):                                            # JR / JALR
        # JR / JALR read the current VI (no branch VI-delay backup; PCSX2 _vuJR / mVUanalyzeJump)
        L.append(f'jumpTarget = (static_cast<uint32_t>(static_cast<uint16_t>({"0" if vis == 0 else f"s.vi[{vis}]"})) * 8u) & 0x3FFFu;')
        if op == 0x25 and vit:
            L.append(f's.vi[{vit}] = {(address + 16) // 8};')
        e.branch = ('jump',)
    elif op in (0x28, 0x29):                                            # IBEQ / IBNE
        sym = '==' if op == 0x28 else '!='
        L.append(f'taken = static_cast<int16_t>({branch_vi(vis)}) {sym} static_cast<int16_t>({branch_vi(vit)});')
        e.branch = ('if', target)
    elif op in (0x2C, 0x2D, 0x2E, 0x2F):                                # IBLTZ / IBGTZ / IBLEZ / IBGEZ
        sym = {0x2C: '<', 0x2D: '>', 0x2E: '<=', 0x2F: '>='}[op]
        L.append(f'taken = static_cast<int16_t>({branch_vi(vis)}) {sym} 0;')
        e.branch = ('if', target)
    elif op == 0x40 and (lower & 0x3F) in (0x30, 0x31, 0x32, 0x34, 0x35):
        direct = lower & 0x3F
        if direct == 0x32:                                              # IADDI
            imm5 = (lower >> 6) & 0x1F
            imm5 = imm5 - 32 if imm5 & 0x10 else imm5
            if vit:
                L.append(f's.vi[{vit}] = static_cast<int16_t>(s.vi[{vis}] + ({imm5}));')
        elif vid:
            if direct == 0x30:
                L.append(f's.vi[{vid}] = static_cast<int16_t>(s.vi[{vis}] + s.vi[{vit}]);')
            elif direct == 0x31:
                L.append(f's.vi[{vid}] = static_cast<int16_t>(s.vi[{vis}] - s.vi[{vit}]);')
            elif direct == 0x34:
                L.append(f's.vi[{vid}] = s.vi[{vis}] & s.vi[{vit}];')
            else:
                L.append(f's.vi[{vid}] = s.vi[{vis}] | s.vi[{vit}];')
    elif op == 0x40 and (lower & 0x3F) >= 0x3C:
        sp = lower_special(lower)
        if sp == 0x30:                                                  # MOVE
            if lanes_of(dest):
                L.append(f'vu1v::store<0x{components(dest):x}u>(s.vf[{vft}], vu1v::load(s.vf[{vfs}]));')
                e.writes_vf0 |= vft == 0
        elif sp == 0x31:                                                # MR32
            if lanes_of(dest):
                L.append(f'vu1v::store<0x{components(dest):x}u>(s.vf[{vft}], vu1v::rotate(vu1v::load(s.vf[{vfs}])));')
                e.writes_vf0 |= vft == 0
        elif sp in (0x34, 0x36):                                        # LQI / LQD
            L.append('{')
            if sp == 0x36 and vis:
                L.append(f'    s.vi[{vis}] = static_cast<int16_t>(s.vi[{vis}] - 1);')
            L.append(f'    const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[{vis}])) * 16u) & 0x3FFFu;')
            load_lanes(e, vft, dest, 'data + addr')
            if sp == 0x34 and vis:
                L.append(f'    s.vi[{vis}] = static_cast<int16_t>(s.vi[{vis}] + 1);')
            L.append('}')
        elif sp in (0x35, 0x37):                                        # SQI / SQD
            L.append('{')
            if sp == 0x37 and vit:
                L.append(f'    s.vi[{vit}] = static_cast<int16_t>(s.vi[{vit}] - 1);')
            L.append(f'    const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[{vit}])) * 16u) & 0x3FFFu;')
            L.append(f'    uint32_t w[4]; std::memcpy(w, s.vf[{vfs}], 16);')
            L.append(f'    Vu1Compiled::queueStore(vu, addr, w, {dest});')
            if sp == 0x35 and vit:
                L.append(f'    s.vi[{vit}] = static_cast<int16_t>(s.vi[{vit}] + 1);')
            L.append('}')
        elif sp == 0x38:                                                # DIV
            L.append(f'Vu1Compiled::div(vu, s.vf[{vfs}][{(lower >> 21) & 3}], s.vf[{vft}][{(lower >> 23) & 3}]);')
        elif sp == 0x39:                                                # SQRT
            L.append(f'Vu1Compiled::sqrt(vu, s.vf[{vft}][{(lower >> 23) & 3}]);')
        elif sp == 0x3A:                                                # RSQRT
            L.append(f'Vu1Compiled::rsqrt(vu, s.vf[{vfs}][{(lower >> 21) & 3}], s.vf[{vft}][{(lower >> 23) & 3}]);')
        elif sp == 0x3B:                                                # WAITQ (the stall check waits)
            pass
        elif sp == 0x3C:                                                # MTIR
            if vit:
                L.append('{')
                L.append(f'    uint32_t f; std::memcpy(&f, &s.vf[{vfs}][{(lower >> 21) & 3}], 4);')
                L.append(f'    s.vi[{vit}] = static_cast<int32_t>(static_cast<int16_t>(f & 0xFFFFu));')
                L.append('}')
        elif sp == 0x3D:                                                # MFIR
            if lanes_of(dest):
                L.append(f'vu1v::store<0x{components(dest):x}u>(s.vf[{vft}], '
                         f'vu1v::splatBits(static_cast<int32_t>(static_cast<int16_t>(s.vi[{vis}] & 0xFFFF))));')
                e.writes_vf0 |= vft == 0
        elif sp == 0x3E:                                                # ILWR
            if vit:
                L.append('{')
                L.append(f'    const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[{vis}])) * 16u) & 0x3FFFu;')
                L.append(f'    uint32_t v; std::memcpy(&v, data + addr + {first_lane() * 4}, 4);')
                L.append(f'    s.vi[{vit}] = static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));')
                L.append('}')
        elif sp == 0x3F:                                                # ISWR
            L.append('{')
            L.append(f'    const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[{vis}])) * 16u) & 0x3FFFu;')
            L.append(f'    const uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>(s.vi[{vit}] & 0xFFFF));')
            L.append('    const uint32_t w[4] = {v, v, v, v};')
            L.append(f'    Vu1Compiled::queueStore(vu, addr, w, {dest});')
            L.append('}')
        elif sp in (0x68, 0x69):                                        # XTOP / XITOP
            if vit:
                L.append(f's.vi[{vit}] = static_cast<int32_t>(s.{"top" if sp == 0x68 else "itop"} & 0x3FFu);')
        elif sp == 0x6C:                                                # XGKICK
            L.append(f'Vu1Compiled::startXgkick(vu, static_cast<uint32_t>(static_cast<uint16_t>(s.vi[{vis}])));')
        else:
            generic()
    else:
        generic()


# ---------------------------------------------------------------------------------------------------------------------
# The pairs: a pair's work, then the copies of it (as a normal pair, as a branch's delay slot, as the E-bit delay slot)
class Program:
    def __init__(self, number, vu, code):
        self.number, self.vu, self.code = number, vu, code
        self.pairs = {}
        for k in range(len(code) // 8):
            lower, upper = struct.unpack_from('<II', code, k * 8)
            self.pairs[vu + k * 8] = Pair(vu + k * 8, lower, upper)
        self.generic = {}

    def plain(self, address):
        """Can run as a delay slot here: no branch, no E / D / T bit, not reserved."""
        p = self.pairs.get(address)
        return p is not None and not (p.branch or p.e_bit or p.d_bit or p.t_bit or p.reserved)

    def compiled(self, address):
        """Gets a normal (enterable) copy."""
        p = self.pairs.get(address)
        return p is not None and not (p.reserved or p.d_bit or p.t_bit or (p.e_bit and p.branch))


def next_pc(address):
    return (address + 8) % CODE_SIZE


def components(lanes):
    """VU dest lanes (x = 8 ... w = 1) -> a component mask (bit c = component c, x = bit 0)."""
    return sum(1 << c for c in lanes_of(lanes))


def pair_work(prog, p, exit_stmt):
    """The pair from its budget check to its write marks (the interpreter's loop body up to the pc update)."""
    out = []
    out.append(f'if (cyc >= budgetEnd || stop) {exit_stmt}')
    # stall test: any read lane not ready at cyc (= calculatePairReadyCycle(...) > cycle); the stall itself is the
    # interpreter's loop (Vu1Compiled::stall), which advances the cycle — reloaded after it
    late = []
    for reg, lanes in ([] if THROUGHPUT else p.stall_vf):
        late.append(f'vu1fast::lanesAfter<0x{components(lanes):x}u>(vfReady + {reg * 4}, cyc)')
    for r in range(1, 16):
        if p.stall_vi & (1 << r) and not THROUGHPUT:
            late.append(f'static_cast<unsigned>(viReady[{r}] > cyc)')
    if p.stall_acc and not THROUGHPUT:
        late.append(f'vu1fast::lanesAfter<0x{components(p.stall_acc):x}u>(accReady, cyc)')
    if p.lo.pipeline == FDIV or p.lo.wait_q:
        late.append('static_cast<unsigned>(fdiv.valid && fdiv.readyCycle > cyc)')
    if p.lo.pipeline == EFU:
        late.append('static_cast<unsigned>(efuReady > cyc)')
    if p.lo.wait_p:
        late.append('static_cast<unsigned>((efu[0].valid && efu[0].readyCycle > cyc) || (efu[1].valid && efu[1].readyCycle > cyc))')
    if p.lo.pipeline == XGKICK:
        late.append('static_cast<unsigned>(kickActive)')
    if late:
        out.append(f'if (({" | ".join(late)}) != 0u)')
        out.append('{')
        out.append(f'    if (!Vu1Compiled::stall(vu, dec[{p.index}], budgetEnd)) {exit_stmt}')
        out.append('    cyc = cycle;')
        out.append('}')

    e_up, e_lo = Emitted(), Emitted()
    emit_upper(e_up, p.address, p.upper)
    if not p.i_bit:
        emit_lower(e_lo, p.address, p.lower, p.upper)
    for e in (e_up, e_lo):
        if e.generic:
            prog.generic[e.generic] = prog.generic.get(e.generic, 0) + 1
    backup = p.lower_vi_write and p.lo.delays_branch
    if backup:
        out.append(f'const int32_t oldVi = s.vi[{p.lower_vi_write}];')
    if p.i_bit:
        out += e_up.lines
        out.append(f's.i = vu1fast::fromBits(0x{f32_normalised_bits(p.lower):08x}u);')
    elif p.shadow:
        out.append(f'float shadowOld[4]; std::memcpy(shadowOld, s.vf[{p.shadow}], 16);')
        out += e_up.lines
        out.append(f'float shadowNew[4]; std::memcpy(shadowNew, s.vf[{p.shadow}], 16);')
        out.append(f'std::memcpy(s.vf[{p.shadow}], shadowOld, 16);')
        out += e_lo.lines
        out.append(f'std::memcpy(s.vf[{p.shadow}], shadowNew, 16);')
    else:
        out += e_up.lines + e_lo.lines
    out.append('bkValid = false;')
    for reg, lanes, lat in ((p.mark_lower[0], p.mark_lower[1], p.mark_lower_lat),
                            (p.mark_upper[0], p.mark_upper[1], p.mark_upper_lat)):
        if reg and lanes and not THROUGHPUT:
            out.append(f'vu1fast::markReady<0x{components(lanes):x}u>(vfReady + {reg * 4}, cyc + {lat}u);')
    for r in range(1, 16):
        if p.mark_vi & (1 << r) and not THROUGHPUT:
            out.append(f'viReady[{r}] = cyc + {p.mark_vi_lat}u;')
    if p.mark_acc and not THROUGHPUT:
        out.append(f'vu1fast::markReady<0x{components(p.mark_acc):x}u>(accReady, cyc + 1u);')
    if p.settle is not None and not THROUGHPUT:
        out.append(f'settle = std::max(settle, cyc + {p.settle}u);')
    if backup:
        out.append(f'bkValue = oldVi; bkReg = {p.lower_vi_write}; bkValid = true;')
    if e_up.writes_vf0 or e_lo.writes_vf0:
        out.append('{ static const float vf0[4] = {0.0f, 0.0f, 0.0f, 1.0f}; std::memcpy(s.vf[0], vf0, 16); }')
    if e_up.writes_vi0 or e_lo.writes_vi0:
        out.append('s.vi[0] = 0;')
    branch = e_lo.branch
    comment = (f'// 0x{p.address:04x}: {upper_name(p.upper)}{"[E]" if p.e_bit else ""}{"[I]" if p.i_bit else ""} | '
               + (f'LOI 0x{p.lower:08x}' if p.i_bit else lower_name(p.lower)))
    return comment, out, branch


def emit_chunk(prog, start, end, fn_name):
    """One C++ function: the normal copies of the pairs in [start, end) plus their delay-slot / end copies."""
    inside = [a for a in sorted(prog.pairs) if start <= a < end and prog.compiled(a)]
    labels = set(inside)
    body, tails = [], []

    def goto(target):
        return f'goto P_{target:04x};' if target in labels else f'VU1C_EXIT(0x{target:04x}u);'

    for a in inside:
        p = prog.pairs[a]
        comment, work, branch = pair_work(prog, p, f'VU1C_EXIT(0x{a:04x}u);')
        body.append(f'P_{a:04x}: {comment}')
        body.append('{')
        if branch and branch[0] == 'if':
            body.append('    bool taken;')
        body += ['    ' + x for x in work]
        nxt = next_pc(a)
        if p.e_bit:
            body.append('    s.ebit = true;')
            body.append('    VU1C_ADVANCE();')
            body.append(f'    goto E_{a:04x};')
            # the E-bit delay slot: runs, then the program is over
            tails.append(f'E_{a:04x}: // the E-bit delay slot')
            if prog.plain(nxt):
                comment2, work2, _ = pair_work(prog, prog.pairs[nxt], f'VU1C_EXIT(0x{nxt:04x}u);')
                tails.append('{ ' + comment2)
                tails += ['    ' + x for x in work2]
                tails.append(f'    s.pc = 0x{next_pc(nxt):04x}u;')
                tails.append('    VU1C_ADVANCE();')
                tails.append('    pairsRun += pairs;')
                tails.append('    return Vu1CompiledResult::ProgramEnded;')
                tails.append('}')
            else:
                tails.append(f'VU1C_EXIT(0x{nxt:04x}u);')
        elif branch:
            body.append('    VU1C_ADVANCE();')
            if branch[0] == 'if':
                body.append(f'    if (taken) goto D_{a:04x};')
                if nxt != end or nxt in labels:
                    body.append('    ' + goto(nxt))
                else:
                    body.append(f'    VU1C_EXIT(0x{nxt:04x}u);')
            else:
                body.append(f'    goto D_{a:04x};')
            # the delay slot, then the branch target
            dynamic = branch[0] == 'jump'
            tgt = 'jumpTarget' if dynamic else f'0x{branch[1]:04x}u'
            tails.append(f'D_{a:04x}: // the delay slot of the branch at 0x{a:04x}')
            if prog.plain(nxt):
                comment2, work2, _ = pair_work(prog, prog.pairs[nxt], f'VU1C_EXIT_DELAY(0x{nxt:04x}u, {tgt});')
                tails.append('{ ' + comment2)
                tails += ['    ' + x for x in work2]
                tails.append('    VU1C_ADVANCE();')
                if dynamic:
                    tails.append('    s.pc = jumpTarget;')
                    tails.append('    goto dispatch;')
                else:
                    tails.append('    ' + goto(branch[1]))
                tails.append('}')
            else:
                tails.append(f'VU1C_EXIT_DELAY(0x{nxt:04x}u, {tgt});')
        else:
            body.append('    VU1C_ADVANCE();')
            if nxt not in labels or nxt != (a + 8):
                body.append('    ' + goto(nxt))
        body.append('}')

    out = [f'static Vu1CompiledResult {fn_name}(VU1Interpreter &vu, uint64_t budgetEnd, uint64_t &pairsRun)', '{']
    out += ['    ' + x for x in [
        'VU1State &s = Vu1Compiled::state(vu);',
        'uint64_t &cycle = Vu1Compiled::cycle(vu);',
        'uint64_t cyc = cycle; // in a register: written through at every advance, reloaded after a stall',
        'const uint64_t &nextCommit = Vu1Compiled::nextCommitCycle(vu);',
        'const bool &kickActive = Vu1Compiled::xgkickActive(vu);',
        'const bool &stop = Vu1Compiled::stopRequested(vu);',
        'uint64_t *const vfReady = Vu1Compiled::vfReadyData(vu);',
        'uint64_t *const viReady = Vu1Compiled::viReadyData(vu);',
        'uint64_t *const accReady = Vu1Compiled::accReadyData(vu);',
        'uint64_t &settle = Vu1Compiled::settleCycle(vu);',
        'bool &bkValid = Vu1Compiled::branchBackupValid(vu);',
        'int32_t &bkValue = Vu1Compiled::branchBackupValue(vu);',
        'uint8_t &bkReg = Vu1Compiled::branchBackupReg(vu);',
        'const auto &fdiv = Vu1Compiled::fdiv(vu);',
        'const auto &efu = Vu1Compiled::efu(vu);',
        'const uint64_t &efuReady = Vu1Compiled::efuResourceReady(vu);',
        'uint8_t *const data = Vu1Compiled::vuData(vu);',
        'const auto *const dec = Vu1Compiled::decodedPairs(vu);',
        'uint64_t pairs = 0u;',
        'uint32_t jumpTarget = 0u;',
        '(void)fdiv; (void)efu; (void)efuReady; (void)data; (void)jumpTarget; (void)kickActive;']]
    out.append('dispatch:')
    out.append('    switch (s.pc)')
    out.append('    {')
    for a in inside:
        out.append(f'    case 0x{a:04x}u: goto P_{a:04x};')
    out.append('    default: VU1C_EXIT(s.pc);')
    out.append('    }')
    out += body + tails
    out.append('}')
    return out, inside


def main():
    if not os.path.exists(ELF):
        sys.exit(f'vu1_recomp: {ELF} not found (python tools\\prepare_disc.py "<your disc>" first)')
    ranges = os.environ.get('HCTP_VU1_RANGES')
    if not ranges and os.path.basename(ELF).upper().startswith('SLUS_207.87'):
        # HCTP: every VU1 MPG packet in the ELF (20 programs incl. the 9 overlays at VU 0x2a00), see hctp_vu1_ranges.txt
        ranges = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hctp_vu1_ranges.txt')).read().strip()
    if ranges:
        # HCTP (Metrowerks ELF, no .vutext): the VIF MPG packets sit inside the main section; take them by vaddr range
        # ("start:end,..."; HCTP SLUS_207.87 = 0x44c6e4:0x44eb70,0x4526a4:0x452d08 - 11 programs, found from VENDETTA_VULOG)
        sets = []
        for r in ranges.split(','):
            start, end = (int(x, 16) for x in r.split(':'))
            sets += program_sets(start, read_vaddr_range(ELF, start, end))
    else:
        vaddr, vutext = read_vutext(ELF)
        sets = program_sets(vaddr, vutext)
    lines = ['// Generated by tools/vu1_recomp.py from SLUS_206.39 (the game\'s VU1 microprograms) - do not edit, do not commit.',
             '// See PS2Recomp ps2xRuntime/include/runtime/ps2_vu1_compiled.h.',
             '#include "runtime/ps2_vu1_compiled.h"',
             '',
             '#include <algorithm>',
             '#include <cmath>',
             '#include <cstdint>',
             '#include <cstdlib>',
             '#include <cstring>',
             '',
             '#if defined(_MSC_VER)',
             '#pragma float_control(precise, on, push) // the VU maths must not be reordered or fused',
             '#endif',
             '',
             '#define VU1C_EXIT(pcValue) do { s.pc = (pcValue); pairsRun += pairs; return Vu1CompiledResult::Continue; } while (0)',
             '#define VU1C_EXIT_DELAY(pcValue, target) do { s.branchPending = true; s.branchTarget = (target); s.branchDelay = 0u; VU1C_EXIT(pcValue); } while (0)',
             '#define VU1C_ADVANCE() do { cycle = ++cyc; if (cyc >= nextCommit) Vu1Compiled::commit(vu); if (kickActive) Vu1Compiled::progressXgkick(vu); ++pairs; } while (0)',
             '',
             'namespace',
             '{']
    global THROUGHPUT
    for THROUGHPUT, sfx in ((False, ''), (True, '_t')):
      pieces = []
      total_pairs = total_compiled = 0
      for n, st in enumerate(sets):
          prog = Program(n, st['vu'], st['code'])
          fns = {}
          start = st['vu']
          end_all = st['vu'] + len(st['code'])
          chunk = 0
          while start < end_all:
              end = min(start + CHUNK_PAIRS * 8, end_all)
              name = f'vu1c_p{n}_c{chunk}{sfx}'
              code, inside = emit_chunk(prog, start, end, name)
              lines += code + ['']
              for a in inside:
                  fns[a] = name
              start, chunk = end, chunk + 1
          words = struct.unpack_from(f'<{len(st["code"]) // 4}I', st['code'])
          lines.append(f'const uint32_t vu1c_p{n}{sfx}_code[] = {{')
          for k in range(0, len(words), 8):
              lines.append('    ' + ', '.join(f'0x{w:08x}u' for w in words[k:k + 8]) + ',')
          lines.append('};')
          lines.append(f'const Vu1CompiledFn vu1c_p{n}{sfx}_fns[] = {{')
          addrs = sorted(prog.pairs)
          for k in range(0, len(addrs), 8):
              lines.append('    ' + ', '.join(fns.get(a, 'nullptr') for a in addrs[k:k + 8]) + ',')
          lines.append('};')
          lines.append(f'const Vu1PairShape vu1c_p{n}{sfx}_shapes[] = {{')
          for a in addrs:
              lines.append('    ' + prog.pairs[a].shape() + ',')
          lines.append('};')
          lines.append('')
          name = f'VU1 program {n} (.vutext 0x{st["source"] - 4:06x})'
          pieces.append(f'{{0x{st["vu"]:04x}u, {len(addrs)}u, vu1c_p{n}{sfx}_code, vu1c_p{n}{sfx}_fns, vu1c_p{n}{sfx}_shapes, "{name}"}}')
          total_pairs += len(addrs)
          total_compiled += len(fns)
          generic = ', '.join(f'{k} x{v}' for k, v in sorted(prog.generic.items())) or 'none'
          print(f'  program {n}: VU 0x{st["vu"]:04x}..0x{end_all:04x}, {len(addrs)} pairs, {len(fns)} compiled, '
                f'left to the interpreter: {generic}')
      lines.append(f'const Vu1CompiledPiece vu1c_pieces{sfx}[] = {{')
      lines += ['    ' + p + ',' for p in pieces]
      lines.append('};')
      lines.append('')
    lines.append('struct Vu1CompiledRegistrar')
    lines.append('{')
    lines += ['    Vu1CompiledRegistrar()',
              '    {',
              '        const char *fast = std::getenv("HCTP_VU1_FAST"); // throughput variant: no stall/ready bookkeeping, plain MADD',
              "        if (fast && fast[0] == '1')",
              '            vu1RegisterCompiledPieces(vu1c_pieces_t, sizeof(vu1c_pieces_t) / sizeof(vu1c_pieces_t[0]));',
              '        else',
              '            vu1RegisterCompiledPieces(vu1c_pieces, sizeof(vu1c_pieces) / sizeof(vu1c_pieces[0]));',
              '    }']
    lines.append('} vu1c_registrar;')
    lines.append('}')
    lines.append('')
    lines.append('#undef VU1C_EXIT')
    lines.append('#undef VU1C_EXIT_DELAY')
    lines.append('#undef VU1C_ADVANCE')
    lines.append('#if defined(_MSC_VER)')
    lines.append('#pragma float_control(pop)')
    lines.append('#endif')
    text = '\n'.join(lines) + '\n'
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    if os.path.exists(OUT) and open(OUT, encoding='utf-8').read() == text:
        print(f'vu1_recomp: {len(sets)} programs, {total_pairs} pairs ({total_compiled} compiled) - {OUT} unchanged')
        return
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print(f'vu1_recomp: {len(sets)} programs, {total_pairs} pairs ({total_compiled} compiled) -> {OUT} ({len(lines)} lines)')


if __name__ == '__main__':
    main()
