"""Nov 11: roster-expansion slot body scale fixes in generated runner files (idempotent; re-run after a regen together
with patch_rosterx_cawcheck.py and patch_rosterx_cawoff.py).
1) MatchWrestler::MakeChar 0x345700: scale table 0x46DAA4 read (lwc1 $f20 at 0x345954) -> hctpScaleAddr(v0, a0).
2) CreateGameChar 0x1fb150: record copy source (memcpy at 0x1fb24c, a1) -> hctpRecordSrc (slot record; scale = +0x112/4096)."""
import os, sys
RUN = sys.argv[1] if len(sys.argv) > 1 else r"F:\hctp-newbase-vulcan\ps2xRuntime\src\runner"
def patch(fname, decl, old, new, what):
    f = os.path.join(RUN, fname)
    src = open(f, encoding="utf-8", errors="surrogateescape").read()
    if new in src:
        print(what, "already patched"); return
    assert src.count(old) == 1, what + ": site not found"
    src = src.replace(old, new)
    if decl not in src:
        k = src.index('#include "ps2_stubs.h"') + len('#include "ps2_stubs.h"\n')
        src = src[:k] + decl + src[k:]
    open(f, "w", encoding="utf-8", errors="surrogateescape", newline="").write(src)
    print(what, "patched")
M = "label_345954:\n"
patch("MakeChar__13MatchWrestlerFv_0x345700.cpp",
      "uint32_t hctpScaleAddr(uint32_t addr, uint32_t code); // hctp_overrides.cpp (patch_rosterx_scale.py)\n",
      M, M + "    SET_GPR_U32(ctx, 2, hctpScaleAddr(GPR_U32(ctx, 2), GPR_U32(ctx, 4))); // patch_rosterx_scale.py\n", "MakeChar")
O = "    SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 2), GPR_U32(ctx, 1)));\n    ctx->in_delay_slot = false;\n    ctx->pc = 0x124D20u;\n"
patch("CreateGameChar__FP9tagSelecti13eWrestlerTypeP5CDataP13EditGenerator_0x1fb150.cpp",
      "uint32_t hctpRecordSrc(uint8_t *rdram, PS2Runtime *runtime, uint32_t a1, uint32_t sel); // patch_rosterx_scale.py\n",
      O, O.replace("    ctx->in_delay_slot = false;\n", "    SET_GPR_U32(ctx, 5, hctpRecordSrc(rdram, runtime, GPR_U32(ctx, 5), GPR_U32(ctx, 21))); // patch_rosterx_scale.py\n    ctx->in_delay_slot = false;\n"),
      "CreateGameChar")
