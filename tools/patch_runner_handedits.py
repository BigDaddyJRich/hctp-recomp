r"""Nov 12 (REGEN_SAFETY step 3): re-applies the 4 TRUE hand edits of the generated runner after a recompiler regen, so they stop
being lost on regen. Run LAST, after sync_generated (VU0) + patch_rosterx_cawcheck/_cawoff/_scale + patch_prop_texwindow.
Text is exactly what the live runner had on Nov 11 (regen_safety_20261111
unner_live); hunks were cut from that diff with 3+ lines
of context and must match exactly once, else the script FAILS (no partial guessing). Idempotent: an already-applied hunk is skipped.
  1. CSkinModel::DrawChar 0x155dc0 (Nov 7): SvR06 per-group flush port (CRender::SendPacket 0x14e760 when the 8 KB scratchpad packet
     reaches 0x1EC1; HCTP_DRAWCHAR_FLUSH=0 off) + resume label 0x15624c.
  2. register_functions.cpp: table entry 88209 = (0x15624c - 0x100008) / 4 -> DrawChar (the resume point after SendPacket returns).
  3. CCharIK::ActionLink 0x1b8860 (Nov 7): SvR04 14-entry twist list in the 0x1b933c loop (hctpIkSvrTwistBone in hctp_overrides.cpp).
  4. CGameSync::Draw 0x1f5c50 (Nov 2): HCTP_RUMBLE_60S entry timer (slti 0x1f6574/0x1f6588 61/421 -> 3000) + hctpRumbleSpawn call
     (Finkel names the new Royal Rumble entrant). Applies on top of patch_rosterx_cawcheck.py's site in the same file.
NOT re-applied on purpose: the Nov 2 diagnostics in AMemCopy 0x1415a0 / CData::Copy 0x15dbb0 (no behaviour; dropped at next regen).
Usage: python patch_runner_handedits.py [runner_dir]   (default = the vulcan runner)"""
import os, sys
RUN = sys.argv[1] if len(sys.argv) > 1 else r"F:\hctp-newbase-vulcan\ps2xRuntime\src\runner"
HUNKS = [
    ('DrawChar__10CSkinModelFiii_0x155dc0.cpp', [
        (r'''#ifdef PS2_FUNCTION_LOG_TRACKER
#include "ps2_log.h"
#endif

// Function: DrawChar__10CSkinModelFiii
// Address: 0x155dc0 - 0x1564b8''',
         r'''#ifdef PS2_FUNCTION_LOG_TRACKER
#include "ps2_log.h"
#endif

#include <cstdlib>
namespace {
struct DrawCharFlushSave {
    __m128i v1, a0, a2, t1, t2, t4, t5;
    __m128 vf[4];
    bool pending = false;
};
DrawCharFlushSave g_drawCharFlushSave;
}

// Function: DrawChar__10CSkinModelFiii
// Address: 0x155dc0 - 0x1564b8'''),
        (r'''        case 0x156040u: goto label_156040;
        case 0x156134u: goto label_156134;
        case 0x156184u: goto label_156184;
        case 0x1562d4u: goto label_1562d4;
        case 0x156350u: goto label_156350;
        case 0x156474u: goto label_156474;''',
         r'''        case 0x156040u: goto label_156040;
        case 0x156134u: goto label_156134;
        case 0x156184u: goto label_156184;
        case 0x15624cu: goto label_drawchar_flush_resume;
        case 0x1562d4u: goto label_1562d4;
        case 0x156350u: goto label_156350;
        case 0x156474u: goto label_156474;'''),
        (r'''    // 0x156248: 0x24420010  addiu       $v0, $v0, 0x10
    ctx->pc = 0x156248u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 2), 16));
label_15624c:
    // 0x15624c: 0x1d80ffb9  bgtz        $t4, . + 4 + (-0x47 << 2)
    ctx->pc = 0x15624Cu;''',
         r'''    // 0x156248: 0x24420010  addiu       $v0, $v0, 0x10
    ctx->pc = 0x156248u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 2), 16));
    // HCTP PC (Nov 7): SvR06 DrawChar 0x1b83a0..0x1b8504 per-group flush port. HCTP builds the whole mesh packet in one
    // 8 KB scratchpad half; meshes with 37-52 bone groups (SvR06 models) overflow it. SvR06: after each group's MSCAL,
    // if (cursor - bufstart) >= 0x1EC1 -> CRender::SendPacket(render, buf, size), then cursor = bufstart = buffer picked
    // from render+0x10594 &1 again. Stock models never reach 0x1EC1. HCTP_DRAWCHAR_FLUSH=0 disables.
    {
        static const bool s_flushOn = [] { const char* e = std::getenv("HCTP_DRAWCHAR_FLUSH"); return !(e && e[0] == '0'); }();
        const uint32_t pkt = SUB32(GPR_U32(ctx, 2), GPR_U32(ctx, 5));
        if (s_flushOn && GPR_S64(ctx, 12) > 0 && pkt >= 0x1EC1u) {
            g_drawCharFlushSave.v1 = GPR_VEC(ctx, 3);  g_drawCharFlushSave.a0 = GPR_VEC(ctx, 4);
            g_drawCharFlushSave.a2 = GPR_VEC(ctx, 6);  g_drawCharFlushSave.t1 = GPR_VEC(ctx, 9);
            g_drawCharFlushSave.t2 = GPR_VEC(ctx, 10); g_drawCharFlushSave.t4 = GPR_VEC(ctx, 12);
            g_drawCharFlushSave.t5 = GPR_VEC(ctx, 13);
            for (int i = 0; i < 4; ++i) g_drawCharFlushSave.vf[i] = ctx->vu0_vf[4 + i];
            g_drawCharFlushSave.pending = true;
            SET_GPR_U64(ctx, 4, GPR_U64(ctx, 17));   // a0 = render (s1); a1 = bufstart already
            SET_GPR_U32(ctx, 6, pkt);                 // a2 = size
            SET_GPR_U32(ctx, 31, 0x15624Cu);
            ctx->pc = 0x14E760u;
            if (!runtime->dispatchGuestBranch(rdram, ctx, 0x14E760u, 0x156248u, 0x15624Cu, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
                return;
            }
            goto label_drawchar_flush_resume;
        }
    }
    goto label_15624c;
label_drawchar_flush_resume:
    if (g_drawCharFlushSave.pending) {
        g_drawCharFlushSave.pending = false;
        SET_GPR_VEC(ctx, 3, g_drawCharFlushSave.v1);  SET_GPR_VEC(ctx, 4, g_drawCharFlushSave.a0);
        SET_GPR_VEC(ctx, 6, g_drawCharFlushSave.a2);  SET_GPR_VEC(ctx, 9, g_drawCharFlushSave.t1);
        SET_GPR_VEC(ctx, 10, g_drawCharFlushSave.t2); SET_GPR_VEC(ctx, 12, g_drawCharFlushSave.t4);
        SET_GPR_VEC(ctx, 13, g_drawCharFlushSave.t5);
        for (int i = 0; i < 4; ++i) ctx->vu0_vf[4 + i] = g_drawCharFlushSave.vf[i];
        const uint32_t flag = READ32(ADD32(GPR_U32(ctx, 17), 0x10594u));
        SET_GPR_S32(ctx, 5, (int32_t)((flag & 1u) ? 0x70002000u : 0x70000000u));
        SET_GPR_U64(ctx, 2, GPR_U64(ctx, 5));
    }
    ctx->pc = 0x15624Cu;
label_15624c:
    // 0x15624c: 0x1d80ffb9  bgtz        $t4, . + 4 + (-0x47 << 2)
    ctx->pc = 0x15624Cu;'''),
    ]),
    ('ActionLink__7CCharIKFv_0x1b8860.cpp', [
        (r'''#include "ps2_log.h"
#endif

// Function: ActionLink__7CCharIKFv
// Address: 0x1b8860 - 0x1b9644
void ActionLink__7CCharIKFv_0x1b8860(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {''',
         r'''#include "ps2_log.h"
#endif

// HAND EDIT Nov 7 (SvR twist list, see hctp_overrides.cpp hctpIkSvrTwistBone): s_svrTwist switches the 0x1b933c loop
// to SvR04's 14-entry list. Backup overrides_backup/ActionLink_CCharIK_before_svr_twist_1107.cpp. Re-apply after regen.
int hctpIkSvrTwistBone(const uint8_t *rdram, uint32_t model, int which);
static int s_svrTwist = 0;
static const int kSvrTwist[14] = {6, 25, 7, 27, 10, 26, 11, 28, 13, 29, 14, 17, 30, 18};
// Function: ActionLink__7CCharIKFv
// Address: 0x1b8860 - 0x1b9644
void ActionLink__7CCharIKFv_0x1b8860(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {'''),
        (r'''    // 0x1b9344: 0x8c900020  lw          $s0, 0x20($a0)
    ctx->pc = 0x1b9344u;
    SET_GPR_S32(ctx, 16, (int32_t)READ32(ADD32(GPR_U32(ctx, 4), 32)));
    // 0x1b9348: 0xc06d3f8  jal         func_1B4FE0
    ctx->pc = 0x1B9348u;
    SET_GPR_U32(ctx, 31, 0x1B9350u);''',
         r'''    // 0x1b9344: 0x8c900020  lw          $s0, 0x20($a0)
    ctx->pc = 0x1b9344u;
    SET_GPR_S32(ctx, 16, (int32_t)READ32(ADD32(GPR_U32(ctx, 4), 32)));
    s_svrTwist = hctpIkSvrTwistBone(rdram, GPR_U32(ctx, 16), 0); // HAND EDIT Nov 7
    // 0x1b9348: 0xc06d3f8  jal         func_1B4FE0
    ctx->pc = 0x1B9348u;
    SET_GPR_U32(ctx, 31, 0x1B9350u);'''),
        (r'''    // 0x1b934c: 0x8c450000  lw          $a1, 0x0($v0) (Delay Slot)
    SET_GPR_S32(ctx, 5, (int32_t)READ32(ADD32(GPR_U32(ctx, 2), 0)));
    ctx->in_delay_slot = false;
    ctx->pc = 0x1B4FE0u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x1B4FE0u, 0x1B9348u, 0x1B9350u, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;''',
         r'''    // 0x1b934c: 0x8c450000  lw          $a1, 0x0($v0) (Delay Slot)
    SET_GPR_S32(ctx, 5, (int32_t)READ32(ADD32(GPR_U32(ctx, 2), 0)));
    ctx->in_delay_slot = false;
    if (s_svrTwist) // HAND EDIT Nov 7: SvR04 list; momo_x (29/30) by name, HCTP has no enum for them
    {
        const int id = kSvrTwist[GPR_S32(ctx, 18) & 15];
        if (id == 29 || id == 30)
        {
            SET_GPR_S32(ctx, 2, hctpIkSvrTwistBone(rdram, GPR_U32(ctx, 16), id == 29 ? 1 : 2));
            ctx->pc = 0x1B9350u;
            goto label_1b9350;
        }
        SET_GPR_S32(ctx, 5, id);
    }
    ctx->pc = 0x1B4FE0u;
    if (!runtime->dispatchGuestBranch(rdram, ctx, 0x1B4FE0u, 0x1B9348u, 0x1B9350u, PS2Runtime::GuestBranchKind::DirectCall, "JAL")) {
        return;'''),
        (r'''        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x1B936Cu;
        // 0x1b9370: 0x2a43000a  slti        $v1, $s2, 0xA (Delay Slot)
        SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 18) < (int64_t)(int32_t)10) ? 1 : 0);
        ctx->in_delay_slot = false;
        if (branch_taken_0x1b936c) {
            ctx->pc = 0x1B9378u;''',
         r'''        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x1B936Cu;
        // 0x1b9370: 0x2a43000a  slti        $v1, $s2, 0xA (Delay Slot)
        SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 18) < (int64_t)(int32_t)(s_svrTwist ? 11 : 10)) ? 1 : 0); // HAND EDIT Nov 7
        ctx->in_delay_slot = false;
        if (branch_taken_0x1b936c) {
            ctx->pc = 0x1B9378u;'''),
        (r'''        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x1B94E8u;
        // 0x1b94ec: 0x24030009  addiu       $v1, $zero, 0x9 (Delay Slot)
        SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 9));
        ctx->in_delay_slot = false;
        if (branch_taken_0x1b94e8) {
            ctx->pc = 0x1B9504u;''',
         r'''        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x1B94E8u;
        // 0x1b94ec: 0x24030009  addiu       $v1, $zero, 0x9 (Delay Slot)
        SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), s_svrTwist ? 10 : 9)); // HAND EDIT Nov 7
        ctx->in_delay_slot = false;
        if (branch_taken_0x1b94e8) {
            ctx->pc = 0x1B9504u;'''),
        (r'''    ctx->pc = 0x1B94F8u;
    // 0x1b94f8: 0x2403000b  addiu       $v1, $zero, 0xB
    ctx->pc = 0x1b94f8u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 11));
    // 0x1b94fc: 0x16430030  bne         $s2, $v1, . + 4 + (0x30 << 2)
    ctx->pc = 0x1B94FCu;
    {''',
         r'''    ctx->pc = 0x1B94F8u;
    // 0x1b94f8: 0x2403000b  addiu       $v1, $zero, 0xB
    ctx->pc = 0x1b94f8u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), s_svrTwist ? 13 : 11)); // HAND EDIT Nov 7
    // 0x1b94fc: 0x16430030  bne         $s2, $v1, . + 4 + (0x30 << 2)
    ctx->pc = 0x1B94FCu;
    {'''),
        (r'''label_1b95c4:
    // 0x1b95c4: 0x2a43000c  slti        $v1, $s2, 0xC
    ctx->pc = 0x1b95c4u;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 18) < (int64_t)(int32_t)12) ? 1 : 0);
    // 0x1b95c8: 0x1460ff59  bnez        $v1, . + 4 + (-0xA7 << 2)
    ctx->pc = 0x1B95C8u;
    {''',
         r'''label_1b95c4:
    // 0x1b95c4: 0x2a43000c  slti        $v1, $s2, 0xC
    ctx->pc = 0x1b95c4u;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 18) < (int64_t)(int32_t)(s_svrTwist ? 14 : 12)) ? 1 : 0); // HAND EDIT Nov 7
    // 0x1b95c8: 0x1460ff59  bnez        $v1, . + 4 + (-0xA7 << 2)
    ctx->pc = 0x1B95C8u;
    {'''),
    ]),
    ('Draw__9CGameSyncFv_0x1f5c50.cpp', [
        (r'''#include <stdexcept>
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"''',
         r'''#include <cstdlib>
#include <stdexcept>
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"'''),
        (r'''
// Function: Draw__9CGameSyncFv
// Address: 0x1f5c50 - 0x1f66d8
void Draw__9CGameSyncFv_0x1f5c50(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef PS2_FUNCTION_LOG_TRACKER
    PS_LOG_ENTRY("Draw__9CGameSyncFv_0x1f5c50");''',
         r'''
// Function: Draw__9CGameSyncFv
// Address: 0x1f5c50 - 0x1f66d8
// HCTP_RUMBLE_60S=1 (community "1 minute royal rumble countdown" PNACH, slti 0x1f6574 / 0x1f6588 -> 0xBB8):
// a new Royal Rumble entrant every 60 s (50 s wait + the 10 s countdown) instead of filling the ring right away.
// Hand edit of the generated file; backup overrides_backup\Draw__9CGameSyncFv_0x1f5c50_pre_rumble60.cpp.
// Royal Rumble: a new entrant leaves the entry queue 0x527A90 for slot 0x527AB8 (hctp_overrides.cpp, host code only)
void hctpRumbleSpawn(uint8_t *rdram, uint32_t slot);
static int32_t hctpRumbleEntryFrames(int32_t stock)
{
    static const bool on = std::getenv("HCTP_RUMBLE_60S") && std::getenv("HCTP_RUMBLE_60S")[0] == '1';
    return on ? 3000 : stock;
}

void Draw__9CGameSyncFv_0x1f5c50(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef PS2_FUNCTION_LOG_TRACKER
    PS_LOG_ENTRY("Draw__9CGameSyncFv_0x1f5c50");'''),
        (r'''    SET_GPR_S32(ctx, 3, (int32_t)FAST_READ32(0x527AC8u));
    // 0x1f6574: 0x2861003d  slti        $at, $v1, 0x3D
    ctx->pc = 0x1f6574u;
    SET_GPR_U64(ctx, 1, ((int64_t)GPR_S64(ctx, 3) < (int64_t)(int32_t)61) ? 1 : 0);
    // 0x1f6578: 0x10200006  beqz        $at, . + 4 + (0x6 << 2)
    ctx->pc = 0x1F6578u;
    {''',
         r'''    SET_GPR_S32(ctx, 3, (int32_t)FAST_READ32(0x527AC8u));
    // 0x1f6574: 0x2861003d  slti        $at, $v1, 0x3D
    ctx->pc = 0x1f6574u;
    SET_GPR_U64(ctx, 1, ((int64_t)GPR_S64(ctx, 3) < (int64_t)(int32_t)hctpRumbleEntryFrames(61)) ? 1 : 0);
    // 0x1f6578: 0x10200006  beqz        $at, . + 4 + (0x6 << 2)
    ctx->pc = 0x1F6578u;
    {'''),
        (r'''    SET_GPR_S32(ctx, 3, (int32_t)FAST_READ32(0x527AC8u));
    // 0x1f6588: 0x286101a5  slti        $at, $v1, 0x1A5
    ctx->pc = 0x1f6588u;
    SET_GPR_U64(ctx, 1, ((int64_t)GPR_S64(ctx, 3) < (int64_t)(int32_t)421) ? 1 : 0);
    // 0x1f658c: 0x14200020  bnez        $at, . + 4 + (0x20 << 2)
    ctx->pc = 0x1F658Cu;
    {''',
         r'''    SET_GPR_S32(ctx, 3, (int32_t)FAST_READ32(0x527AC8u));
    // 0x1f6588: 0x286101a5  slti        $at, $v1, 0x1A5
    ctx->pc = 0x1f6588u;
    SET_GPR_U64(ctx, 1, ((int64_t)GPR_S64(ctx, 3) < (int64_t)(int32_t)hctpRumbleEntryFrames(421)) ? 1 : 0);
    // 0x1f658c: 0x14200020  bnez        $at, . + 4 + (0x20 << 2)
    ctx->pc = 0x1F658Cu;
    {'''),
        (r'''        ctx->branch_pc = 0x1F65BCu;
        // 0x1f65c0: 0xac227ab8  sw          $v0, 0x7AB8($at) (Delay Slot)
        WRITE32(ADD32(GPR_U32(ctx, 1), 31416), GPR_U32(ctx, 2));
        ctx->in_delay_slot = false;
        if (branch_taken_0x1f65bc) {
            ctx->pc = 0x1F65D8u;''',
         r'''        ctx->branch_pc = 0x1F65BCu;
        // 0x1f65c0: 0xac227ab8  sw          $v0, 0x7AB8($at) (Delay Slot)
        WRITE32(ADD32(GPR_U32(ctx, 1), 31416), GPR_U32(ctx, 2));
        hctpRumbleSpawn(rdram, GPR_U32(ctx, 2)); // hand edit: Finkel names the new Royal Rumble entrant (host only)
        ctx->in_delay_slot = false;
        if (branch_taken_0x1f65bc) {
            ctx->pc = 0x1F65D8u;'''),
    ]),
    ('register_functions.cpp', [
        (r'''        g_ps2RecompiledFunctionTable[88078] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x156040
        g_ps2RecompiledFunctionTable[88139] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x156134
        g_ps2RecompiledFunctionTable[88159] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x156184
        g_ps2RecompiledFunctionTable[88243] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x1562d4
        g_ps2RecompiledFunctionTable[88274] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x156350
        g_ps2RecompiledFunctionTable[88347] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x156474''',
         r'''        g_ps2RecompiledFunctionTable[88078] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x156040
        g_ps2RecompiledFunctionTable[88139] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x156134
        g_ps2RecompiledFunctionTable[88159] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x156184
        g_ps2RecompiledFunctionTable[88209] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x15624c (HCTP PC DrawChar flush resume)
        g_ps2RecompiledFunctionTable[88243] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x1562d4
        g_ps2RecompiledFunctionTable[88274] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x156350
        g_ps2RecompiledFunctionTable[88347] = DrawChar__10CSkinModelFiii_0x155dc0; // 0x156474'''),
    ]),
]
bad = 0
for f, hunks in HUNKS:
    p = os.path.join(RUN, f)
    src = open(p, encoding="utf-8", errors="surrogateescape").read()
    done = 0
    for i, (old, new) in enumerate(hunks):
        if new in src:
            continue
        if src.count(old) != 1:
            print(f"FAIL {f} hunk {i}: anchor found {src.count(old)} times"); bad += 1; continue
        src = src.replace(old, new); done += 1
    if done:
        open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(src)
    print(f"{f}: {done} applied, {len(hunks) - done} already present or failed")
if bad:
    sys.exit(f"{bad} hunk(s) FAILED - runner layout changed, re-cut the hand edits")
