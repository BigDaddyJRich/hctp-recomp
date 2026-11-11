"""Nov 11: event-prop VRAM window by real texture span (see hctpPropBigWindow in hctp_overrides.cpp).
Patches the generated runner MakeWeapon__FiiPib_0x211530.cpp at 0x2117a4 ("sltiu at, v0, 5": id-0x82A < 5 = stock big
list): at = hctpPropBigWindow(rdram, id=$s5, texpack=$fp, stock) so a prop whose texpack spans > 0x100 blocks takes the
stock 0x300-block window branch (0x2117c8). Idempotent; re-run after a recompiler regen. Backup in overrides_backup\\runner_propwin."""
import os, shutil, sys
RUN = sys.argv[1] if len(sys.argv) > 1 else r"F:\hctp-newbase-vulcan\ps2xRuntime\src\runner"
F = os.path.join(RUN, "MakeWeapon__FiiPib_0x211530.cpp")
BK = os.path.join(RUN, "..", "..", "..", "overrides_backup", "runner_propwin")
OLD = "SET_GPR_U64(ctx, 1, ((uint64_t)GPR_U64(ctx, 2) < (uint64_t)(int64_t)(int32_t)5) ? 1 : 0);"
NEW = ("SET_GPR_U64(ctx, 1, hctpPropBigWindow(rdram, GPR_U32(ctx, 21), GPR_U32(ctx, 30), "
       "(uint64_t)GPR_U64(ctx, 2) < (uint64_t)(int64_t)(int32_t)5) ? 1 : 0);")
DECL = "bool hctpPropBigWindow(const uint8_t *rdram, uint32_t id, uint32_t texpack, bool stockBig); // patch_prop_texwindow.py\n"
src = open(F, encoding="utf-8", errors="surrogateescape").read()
if NEW in src:
    print("already patched"); sys.exit()
i = src.index("// 0x2117a4:")
j = src.index(OLD, i)
assert j - i < 200, "unexpected layout"
src = src[:j] + NEW + src[j + len(OLD):]
k = src.index('#include "ps2_stubs.h"') + len('#include "ps2_stubs.h"\n')
src = src[:k] + DECL + src[k:]
os.makedirs(BK, exist_ok=True)
if not os.path.exists(os.path.join(BK, os.path.basename(F))):
    shutil.copy2(F, os.path.join(BK, os.path.basename(F)))
open(F, "w", encoding="utf-8", errors="surrogateescape", newline="").write(src)
print("patched", F)
