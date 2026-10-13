# Copies the recompiler's output (generated\) into PS2Recomp's runtime, touching only files whose content
# changed, so ninja recompiles just those. The two generated headers go to ps2xRuntime\include\ (as the
# runtime README says); .cpp files no longer generated are removed from the runner folder.
import filecmp, os, shutil, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
gen = os.path.join(root, 'generated')
runner = os.path.join(root, 'PS2Recomp', 'ps2xRuntime', 'src', 'runner')
include = os.path.join(root, 'PS2Recomp', 'ps2xRuntime', 'include')
HEADERS = {'ps2_recompiled_functions.h', 'ps2_recompiled_stubs.h'}

# HCTP: vf0 is hardwired to (0,0,0,1) on the VU - writes are discarded. The recompiler wrote it (VU0 macro ops with
# dest $vf0, used by HCTP's collision code only for their MAC flags; qmtc2 to $vf0) -> identity matrices built from
# vf0 (objMatrix::SetScale etc.) broke: every 2-D sprite collapsed to the screen origin (ATTENTION plate). Ops with
# dest vf0 now only set MAC/status flags like PCSX2 (Z/S per field, status Z/S + sticky ZS/SS); qmtc2 to vf0 is dropped.
import re
VF0_HELPER = b"""
#ifndef HCTP_VU0_FLAGS_ONLY_DEFINED
#define HCTP_VU0_FLAGS_ONLY_DEFINED
#include <cstring>
static inline void hctpVu0FlagsOnly(R5900Context *ctx, __m128 res, __m128 mask)
{
    alignas(16) float v[4];
    alignas(16) int32_t m[4];
    _mm_store_ps(v, res);
    _mm_store_si128(reinterpret_cast<__m128i *>(m), _mm_castps_si128(mask));
    uint32_t mac = 0;
    for (int f = 0; f < 4; ++f) // lane 0 = x -> MAC bit 3 (Z) / 7 (S)
    {
        if (!m[f])
            continue;
        uint32_t b;
        std::memcpy(&b, &v[f], 4);
        if ((b & 0x7FFFFFFFu) == 0u)
            mac |= (8u >> f);
        if (b & 0x80000000u)
            mac |= (0x80u >> f);
    }
    ctx->vu0_mac_flags = mac;
    uint32_t st = ctx->vu0_status & ~0x3u;
    if (mac & 0x0Fu) st |= 0x41u;
    if (mac & 0xF0u) st |= 0x82u;
    ctx->vu0_status = static_cast<uint16_t>(st);
}
#endif
"""
_BLEND = re.compile(rb'ctx->vu0_vf\[0\] = (?:_mm_blendv_ps|PS2_VBLEND)\(ctx->vu0_vf\[0\], res, (_mm_castsi128_ps\(mask\))\);')
_QMTC2 = re.compile(rb'ctx->vu0_vf\[0\] = _mm_castsi128_ps\(GPR_VEC\(ctx, \d+\)\);')
def fix_vf0(data):
    data = _BLEND.sub(rb'hctpVu0FlagsOnly(ctx, res, \1);', data)
    data = _QMTC2.sub(b'/* qmtc2 to vf0 dropped (read-only) */', data)
    if b'hctpVu0FlagsOnly(ctx' in data:
        i = data.index(b'#include "ps2_runtime.h"')
        i = data.index(b'\n', i) + 1
        data = data[:i] + VF0_HELPER + data[i:]
    return data

# HCTP: VU0 macro-mode memory ops (vsqi/vlqi/vsqd/vlqd/...) address VU0 DATA memory (0x11004000 + vi*16, 4 KB),
# but the recompiler emitted READ128/WRITE128 at vi*16 = EE main RAM 0x0-0x3FF0. MathEngine::Initialize 0x145c50 stores
# the sin/cos/atan coefficient tables with vsqi -> they landed in RDRAM 0x40-0x180, VU0 microprograms 0x510/0x600 read
# zeros -> every sin/cos = 0 -> every rotation matrix had a zero 3x3 (ATTENTION banner, menus, 3D all collapsed).
_VU0MEM = re.compile(rb'uint32_t addr = \(\(uint32_t\)\(ctx->vi\[(\d+)\] & 0x3FF\)\) << 4;')
def fix_vu0mem(data):
    return _VU0MEM.sub(rb'uint32_t addr = 0x11004000u + ((((uint32_t)ctx->vi[\1]) << 4) & 0xFF0u); /* HCTP: VU0 data mem */', data)

changed = added = 0
names = set(os.listdir(gen))
# reading ~25,000 files takes minutes: say how far it is (the setup turns these lines into progress)
total = len(names)
for i, n in enumerate(sorted(names), 1):
    if i % 1000 == 0 or i == total:
        print(f'Checking which files changed: {i:,} of {total:,}', flush=True)
    dst = os.path.join(include if n in HEADERS else runner, n)
    src = os.path.join(gen, n)
    if n.endswith('.cpp'):
        with open(src, 'rb') as f:
            data = f.read()
        hasVf0 = b'ctx->vu0_vf[0] = ' in data
        hasVu0Mem = _VU0MEM.search(data) is not None
        if hasVf0 or hasVu0Mem:
            if hasVf0:
                data = fix_vf0(data)
            if hasVu0Mem:
                data = fix_vu0mem(data)
            if os.path.exists(dst):
                with open(dst, 'rb') as f:
                    if f.read() == data:
                        continue
                changed += 1
            else:
                added += 1
            with open(dst, 'wb') as f:
                f.write(data)
            continue
    if os.path.exists(dst):
        if filecmp.cmp(src, dst, shallow=False):
            continue
        changed += 1
    else:
        added += 1
    shutil.copyfile(src, dst)
# our own runtime code (overrides\), kept with the generated files
for n in os.listdir(os.path.join(root, 'overrides_hctp')):
    src, dst = os.path.join(root, 'overrides_hctp', n), os.path.join(runner, n)
    if not os.path.exists(dst) or not filecmp.cmp(src, dst, shallow=False):
        shutil.copyfile(src, dst); changed += 1
    names.add(n)
removed = 0
for n in os.listdir(runner):
    if n.endswith('.cpp') and n not in names:
        os.remove(os.path.join(runner, n)); removed += 1
print(f'sync: {changed} changed, {added} added, {removed} removed, {len(names) - changed - added} unchanged')
