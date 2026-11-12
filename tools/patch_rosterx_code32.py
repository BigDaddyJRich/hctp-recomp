r"""Nov 12 ROSTER STEP C2 (32-bit cast codes): value-based enc/dec at every guest access of the Common cast-entry code field
(Common 0x4920C0 + 0x100 + i*0x20 + 0xA, u16) plus u16 code-param masks, so slot codes > 0xFFFF (ids >= 656) survive.
  sh   -> WRITE16(addr, hctpCodeEnc(v))      code that does not fit u16 -> reserved sentinel (host table sentinel->u32)
  lhu  -> SET_GPR_ZE32(r, hctpCodeDec(raw))  sentinel -> u32 code, anything else unchanged
  lh   -> SET_GPR_S32(r, hctpCodeDecS(raw))  same, non-sentinels keep sign extension
  andi rd, rs, 0xFFFF (code params) -> hctpCodeMask(v): registered slot codes > 0xFFFF pass, everything else & 0xFFFF
Identity for every value while no slot id >= 656 exists. Sites = hctp-project analysis\stepc2 (scan_all.py all.txt + index
forms + CreateGameChar tagSelect* + SetRoyalRumble MATCHNO copies + U32 plan item 3 callees); design ROSTER_STEP_C_20261111 sec E/F.
Idempotent; argv[1] = runner dir. Anchored per pc on the generated "// 0x<pc>:" comment (all copies, delay-slot duplicates too)."""
import os, re, shutil, sys
RUN = sys.argv[1] if len(sys.argv) > 1 else r"F:\hctp-newbase-vulcan\ps2xRuntime\src\runner"
BK = os.path.join(RUN, "..", "..", "..", "overrides_backup", "runner_code32")
DECL = ("uint16_t hctpCodeEnc(uint32_t v); uint32_t hctpCodeDec(uint16_t v); int32_t hctpCodeDecS(uint16_t v); "
        "uint64_t hctpCodeMask(uint64_t v); // hctp_overrides.cpp (patch_rosterx_code32.py)\n")
PAT = {
    # WRITE16(addr, (uint16_t)GPR_U32(ctx, N)) or, $at-absolute: static_cast<uint16_t>((uint16_t)GPR_U32(ctx, N)) + FAST_WRITE16
    "st": (re.compile(r"\(uint16_t\)GPR_U32\(ctx, (\d+)\)"), lambda m: f"hctpCodeEnc(GPR_U32(ctx, {m.group(1)}))"),
    # (uint16_t)READ16(addr) or (uint16_t)FAST_READ16(abs)
    "ld": (re.compile(r"SET_GPR_ZE32\(ctx, (\d+), \(uint16_t\)(FAST_)?READ16\((.+)\)\);"), lambda m: f"SET_GPR_ZE32(ctx, {m.group(1)}, hctpCodeDec({m.group(2) or ''}READ16({m.group(3)})));"),
    "lds": (re.compile(r"SET_GPR_S32\(ctx, (\d+), \(int16_t\)(FAST_)?READ16\((.+)\)\);"), lambda m: f"SET_GPR_S32(ctx, {m.group(1)}, hctpCodeDecS({m.group(2) or ''}READ16({m.group(3)})));"),
    "mask": (re.compile(r"SET_GPR_U64\(ctx, (\d+), GPR_U64\(ctx, (\d+)\) & \(uint64_t\)\(uint16_t\)65535\);"), lambda m: f"SET_GPR_U64(ctx, {m.group(1)}, hctpCodeMask(GPR_U64(ctx, {m.group(2)})));"),
}
DONE = {"st": "hctpCodeEnc(", "ld": "hctpCodeDec(", "lds": "hctpCodeDecS(", "mask": "hctpCodeMask("}
SITES = {
    0x12cce0: {0x12cd5c: 'mask', 0x12cd64: 'mask', 0x12cd8c: 'mask', 0x12cd90: 'mask', 0x12cdc0: 'mask'},
    0x12cf30: {0x12cf30: 'mask', 0x12cf58: 'mask'},
    0x12cf80: {0x12cf80: 'mask', 0x12cfa8: 'mask'},
    0x130d10: {0x130e14: 'st', 0x130f2c: 'st', 0x130f48: 'st', 0x130f94: 'st', 0x130fa4: 'st', 0x131118: 'ld', 0x131198: 'st', 0x131294: 'ld', 0x131300: 'st'},
    0x1a51d0: {0x1a5350: 'ld'},
    0x1f5410: {0x1f5438: 'ld'},
    0x1f5c50: {0x1f5d34: 'ld', 0x1f5d90: 'ld', 0x1f6180: 'st', 0x1f6300: 'st', 0x1f6308: 'ld'},
    0x1fa0b0: {0x1fa5b8: 'st'},
    0x1fa640: {0x1fa658: 'lds', 0x1fa688: 'st', 0x1fa6d0: 'st', 0x1fa6e0: 'st', 0x1fa6e8: 'st'},
    0x1fa700: {0x1fa7a4: 'ld', 0x1fa880: 'ld'},
    0x1fb000: {0x1fb020: 'ld', 0x1fb038: 'st', 0x1fb0b0: 'st'},
    0x1fb150: {0x1fb1a4: 'ld', 0x1fb2c0: 'ld', 0x1fb2d4: 'ld', 0x1fb3b0: 'ld', 0x1fb66c: 'ld', 0x1fb928: 'ld', 0x1fbac4: 'ld', 0x1fbb00: 'ld'},
    0x1fbc70: {0x1fbeec: 'ld'},
    0x1fcc60: {0x1fcce8: 'ld'},
    0x1fd940: {0x1fd994: 'ld'},
    0x1fdb10: {0x1fdc04: 'ld'},
    0x1fde20: {0x1fde4c: 'ld'},
    0x1fdf80: {0x1fdfb4: 'ld'},
    0x1fe0b0: {0x1fe238: 'st', 0x1fe348: 'st', 0x1fe350: 'st'},
    0x1feed0: {0x1fef54: 'ld'},
    0x202ba0: {0x202bd8: 'ld'},
    0x224620: {0x224664: 'st', 0x224710: 'ld', 0x224748: 'ld', 0x224754: 'st', 0x2247b0: 'st', 0x2247c0: 'ld'},
    0x2304b0: {0x2304fc: 'ld'},
    0x233e60: {0x233e98: 'ld'},
    0x2340b0: {0x2340ec: 'ld'},
    0x234400: {0x2344bc: 'ld'},
    0x23ade0: {0x23ae20: 'ld'},
    0x23ae90: {0x23aebc: 'ld'},
    0x251e30: {0x251e68: 'ld'},
    0x2542b0: {0x254370: 'ld', 0x2543a0: 'ld', 0x2543ec: 'ld'},
    0x2579e0: {0x257a5c: 'st'},
    0x257ea0: {0x2580c8: 'st', 0x2580f4: 'st', 0x258104: 'st', 0x25812c: 'st', 0x258178: 'st', 0x2581b8: 'st', 0x2581d8: 'st', 0x258208: 'st', 0x2583fc: 'ld', 0x258414: 'st', 0x258468: 'st', 0x258520: 'ld', 0x2585b0: 'ld', 0x258610: 'st'},
    0x258cc0: {0x258d10: 'st'},
    0x259840: {0x2598c8: 'st', 0x259934: 'st', 0x259940: 'ld'},
    0x2a3a10: {0x2a3a44: 'ld'},
    0x2a7e80: {0x2a7f44: 'ld'},
    0x2a8d30: {0x2a8f74: 'ld'},
    0x2a9c90: {0x2aa9b0: 'st'},
    0x2ab710: {0x2ab710: 'mask', 0x2ab748: 'mask'},
    0x2abb50: {0x2abb50: 'mask', 0x2abb8c: 'mask'},
    0x2abd70: {0x2abd70: 'mask', 0x2abdac: 'mask'},
    0x2ac030: {0x2ac030: 'mask', 0x2ac068: 'mask'},
    0x2ac230: {0x2ac230: 'mask', 0x2ac26c: 'mask'},
    0x2ac980: {0x2aca10: 'ld', 0x2aca40: 'ld', 0x2acaec: 'ld', 0x2acb4c: 'ld', 0x2acb64: 'ld', 0x2acc24: 'ld', 0x2acd28: 'ld', 0x2acd7c: 'ld', 0x2acdac: 'ld'},
    0x2ace70: {0x2acf48: 'ld', 0x2acf98: 'ld'},
    0x2ad1b0: {0x2ad1e8: 'ld'},
    0x2ae4e0: {0x2ae5fc: 'lds', 0x2ae790: 'ld', 0x2ae89c: 'ld'},
    0x2af300: {0x2af300: 'mask', 0x2af334: 'mask'},
    0x2af480: {0x2af484: 'mask', 0x2af4c8: 'mask'},
    0x2e1690: {0x2e16b8: 'ld'},
    0x309e50: {0x309e54: 'mask'},
    0x309ed0: {0x309ef0: 'mask', 0x309f28: 'mask'},
    0x30cad0: {0x30cb7c: 'ld', 0x30cbd8: 'ld'},
    0x30ce00: {0x30cf0c: 'ld', 0x30d040: 'ld', 0x30d110: 'ld'},
    0x310560: {0x310aac: 'st', 0x310ad4: 'st', 0x310cbc: 'st'},
    0x33a990: {0x33abd4: 'st', 0x33ac08: 'st', 0x33ac40: 'st', 0x33acd4: 'ld'},
    0x33b0f0: {0x33b18c: 'ld'},
    0x33bfd0: {0x33c030: 'st', 0x33c048: 'st'},
    0x341f80: {0x3421a0: 'st'},
    0x3425a0: {0x3426c4: 'st'},
    0x344860: {0x344900: 'st'},
    0x345700: {0x345924: 'mask'},
    0x3868c0: {0x38697c: 'st', 0x386984: 'ld'},
    0x3873b0: {0x38743c: 'st', 0x38744c: 'st'},
    0x39a7a0: {0x39a848: 'ld', 0x39a91c: 'ld', 0x39aa20: 'ld'},
    0x39c0c0: {0x39c300: 'ld', 0x39c32c: 'st', 0x39c338: 'st', 0x39c340: 'st', 0x39c34c: 'st', 0x39c35c: 'st', 0x39c368: 'st', 0x39c370: 'st', 0x39c378: 'st', 0x39c384: 'st', 0x39c394: 'st', 0x39c39c: 'st', 0x39c3a4: 'st', 0x39c450: 'st'},
    0x39c620: {0x39c6e8: 'ld', 0x39c784: 'ld'},
    0x39dc00: {0x39dcb8: 'ld', 0x39dd60: 'ld', 0x39ddd0: 'ld', 0x39de70: 'ld', 0x39df9c: 'ld', 0x39e000: 'ld', 0x39e078: 'ld'},
    0x39e200: {0x39e25c: 'ld'},
    0x3a94e0: {0x3a95d4: 'ld', 0x3a9638: 'ld'},
}
files = {}
for f in os.listdir(RUN):
    m = re.search(r"_0x([0-9a-f]+)\.cpp$", f)
    if m:
        files[int(m.group(1), 16)] = f
total = done = 0; missing = []
for fa, sites in sorted(SITES.items()):
    f = files.get(fa)
    if not f:
        missing.append(f"fn 0x{fa:x}"); continue
    path = os.path.join(RUN, f)
    src = open(path, encoding="utf-8", errors="surrogateescape").read()
    lines = src.split("\n"); n = 0
    for pc, kind in sites.items():
        anchor = re.compile(r"//\s*0x%x:\s" % pc)
        rx, rep = PAT[kind]; hit = False
        for i, l in enumerate(lines):
            if not anchor.search(l):
                continue
            for j in range(i + 1, min(i + 4, len(lines))):
                if lines[j].lstrip().startswith("//"):
                    break
                if DONE[kind] in lines[j]:
                    hit = True; done += 1; break
                m = rx.search(lines[j])
                if m:
                    lines[j] = lines[j].replace(m.group(0), rep(m)); n += 1; hit = True; break
        if not hit:
            missing.append(f"{f[:40]} 0x{pc:x} {kind}")
    if n == 0:
        continue
    out = "\n".join(lines)
    if DECL not in out:
        k = out.index('#include "ps2_stubs.h"') + len('#include "ps2_stubs.h"\n')
        out = out[:k] + DECL + out[k:]
    os.makedirs(BK, exist_ok=True)
    if not os.path.exists(os.path.join(BK, f)):
        shutil.copy2(path, os.path.join(BK, f))
    open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(out)
    total += n; print(f"{n} {f}")
print("patched sites:", total, "already:", done, "missing:", len(missing))
for x in missing:
    print("  MISSING", x)
sys.exit(1 if missing else 0)
