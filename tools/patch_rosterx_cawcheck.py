r"""Nov 10 STEP A permanent roster codes: patch every guest "slti rd, rs, 0x2710" (cast code < 10000 = stock, else CAW)
in the generated runner .cpp files so roster-expansion slot codes (16200..65499) take the stock path.
Idempotent; re-run after a recompiler regen. Backups: <runner>\..\..\..\overrides_backup\runner_cawcheck\.
GetDefStsBit 0x1bcb74 is skipped (range compare of a non-code value)."""
import os, re, shutil, sys
RUN = sys.argv[1] if len(sys.argv) > 1 else r"F:\hctp-newbase-vulcan\ps2xRuntime\src\runner"
BK = os.path.join(RUN, "..", "..", "..", "overrides_backup", "runner_cawcheck")
SKIP = {"GetDefStsBit"}
CMT = re.compile(r"//\s*0x([0-9a-f]+):\s*0x[0-9a-f]+\s+slti\s+\$\w+,\s*\$\w+,\s*0x2710")
LINE = re.compile(r"SET_GPR_U64\(ctx, (\d+), \(\(int64_t\)GPR_S64\(ctx, (\d+)\) < \(int64_t\)\(int32_t\)10000\) \? 1 : 0\);")
DECL = "bool hctpCodeLt10k(int64_t v); // hctp_overrides.cpp (patch_rosterx_cawcheck.py)\n"
total = 0
for f in sorted(os.listdir(RUN)):
    if not re.search(r"_0x[0-9a-f]+\.cpp$", f) or f.split("__")[0] in SKIP:
        continue
    path = os.path.join(RUN, f)
    src = open(path, encoding="utf-8", errors="surrogateescape").read()
    if "0x2710" not in src:
        continue
    lines = src.split("\n"); n = 0
    for i, l in enumerate(lines):
        if CMT.search(l):
            for j in range(i + 1, min(i + 4, len(lines))):
                m = LINE.search(lines[j])
                if m:
                    lines[j] = lines[j].replace(m.group(0), f"SET_GPR_U64(ctx, {m.group(1)}, hctpCodeLt10k(GPR_S64(ctx, {m.group(2)})) ? 1 : 0);")
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
