"""Nov 11: guest "addiu rd, rs, -0x2710" (cast code - 10000 = CAW offset; negative = stock) in the generated runner .cpp
files -> hctpCawOff(rs): roster-expansion slot codes (16200+) give -1 (stock path), every other value is unchanged.
Missed by patch_rosterx_cawcheck.py (slti only): InitEventSystem 0x2ad340 set CSd5EventData+0x96C (CAW event) for slot
casts -> invisible slot wrestlers in entrances; MakeChar 0x345700 picked a CAW id. Idempotent; re-run after a regen."""
import os, re, shutil, sys
RUN = sys.argv[1] if len(sys.argv) > 1 else r"F:\hctp-newbase-vulcan\ps2xRuntime\src\runner"
BK = os.path.join(RUN, "..", "..", "..", "overrides_backup", "runner_cawoff")
CMT = re.compile(r"//\s*0x([0-9a-f]+):\s*0x[0-9a-f]+\s+addiu\s+\$\w+,\s*\$\w+,\s*-0x2710\b")
LINE = re.compile(r"\(int32_t\)ADD32\(GPR_U32\(ctx, (\d+)\), 4294957296\)")
DECL = "int32_t hctpCawOff(uint32_t v); // hctp_overrides.cpp (patch_rosterx_cawoff.py)\n"
total = 0
for f in sorted(os.listdir(RUN)):
    if not re.search(r"_0x[0-9a-f]+\.cpp$", f):
        continue
    path = os.path.join(RUN, f)
    src = open(path, encoding="utf-8", errors="surrogateescape").read()
    if "-0x2710" not in src:
        continue
    lines = src.split("\n"); n = 0
    for i, l in enumerate(lines):
        if CMT.search(l):
            for j in range(i + 1, min(i + 4, len(lines))):
                m = LINE.search(lines[j])
                if m:
                    lines[j] = lines[j].replace(m.group(0), f"(int32_t)hctpCawOff(GPR_U32(ctx, {m.group(1)}))")
                    n += 1; break
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
print("patched sites:", total)
