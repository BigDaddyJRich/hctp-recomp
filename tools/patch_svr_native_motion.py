r"""Nov 12: SvR04/SvR06 native motion port, runner side (plan: F:\hctp-project\notes\SVR_NATIVE_ANIMATION_PLAN_20261112.md sec 6).
Overrides side (helpers for E/F/G below + mode helpers) = hctp_overrides.cpp "SVR NATIVE MOTION".
  A. CMotionModel alloc 0x60 -> 0x68 at all 11 `li a0,0x60; jal __nw` sites before ctor 0x1b4f50 (SvR04 0x64, SvR06 0x68 at every site:
     33 shorts +0x24..+0x65). Verified in the ELF: every jal 0x1b4f50 caller has exactly one of these li's before its __nw.
     Nov 12 fix: CreateGameChar has a SECOND li: 0x1fb5ac (delay slot of `beqz s2, 0x1fb5dc` straight to the __nw, skipping 0x1fb5d8)
     -> that path still allocated 0x60 and +0x60.. (shorts 30..32) were the next heap block (CAM crash, r_momo_x = 21114).
  B. P3 CMotionHokan::ActionLink 0x1b5b60 at label_1b5da4: bone 13 -> a1 0x1d (l_momo_x), 17 -> 0x1e (r_momo_x) (SvR04 0x1a8a2c,
     SvR06 RotationHokan 0x216660). Without it, bones 13/17 flagged by P2 would write r_ninoude_x (0x1a).
  C. P4 same function, RotX at 0x1b5f68 (type-3 kote path): halve the angle when the model has r_kote_x2 (SvR06 flag 2, 0x216660).
  D. CCharIK::ActionLink 0x1b8860 twist loop (on top of patch_runner_handedits.py Nov 7 hunks): 3 lists by model
     (hctpSvrIkMode: 0 HCTP 12 = 0x4129f0, 1 SvR04 14 = 0x346020 / SvR06 mode1 0x337b10, 2 SvR06 16 = 0x337b40); group thresholds
     and group-end (tip store) indices per list; ids 29..32 go straight to GetBoneNo (valid after P1b), no by-name lookup.
Idempotent; FAILS (no partial guessing) when a hunk is not found exactly. Run after patch_runner_handedits.py.
Usage: python patch_svr_native_motion.py [runner_dir]"""
import glob, os, re, shutil, sys

RUN = sys.argv[1] if len(sys.argv) > 1 else r"F:\hctp-newbase-vulcan\ps2xRuntime\src\runner"
BK = os.path.join(RUN, "..", "..", "..", "overrides_backup", "runner_svrnative_1112")
SITES = [0x188d10, 0x1e276c, 0x1fb5ac, 0x1fb5d8, 0x213278, 0x243ba0, 0x26eb98, 0x29afb0, 0x29b3b4, 0x2eacf4, 0x2eba00, 0x3459c8]
MARK = "// SVRNATIVE"


def rd(f):
    return open(f, encoding="utf-8", errors="surrogateescape").read()


def wr(f, s):
    os.makedirs(BK, exist_ok=True)
    b = os.path.join(BK, os.path.basename(f))
    if not os.path.exists(b):
        shutil.copy2(f, b)
    open(f, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)


def once(s, old, new, what):
    n = s.count(old)
    if n != 1:
        sys.exit(f"FAIL {what}: found {n}x")
    return s.replace(old, new)


# ---- A. alloc sites
files = glob.glob(os.path.join(RUN, "*.cpp"))
done = 0
for site in SITES:
    tag = "// 0x%x: 0x24040060" % site
    hits = [f for f in files if tag in rd(f)]
    if not hits:
        sys.exit(f"FAIL alloc site 0x{site:x}: comment not found")
    for f in hits:
        s = rd(f)
        out, pos, n = [], 0, 0
        for m in re.finditer(re.escape(tag), s):
            k = s.index("\n", m.end()) + 1
            if s[k:].lstrip().startswith("ctx->pc = 0x%xu;" % site):  # optional pc line between comment and op
                k = s.index("\n", k) + 1
            line_end = s.index("\n", k)
            line = s[k:line_end]
            if "ADD32(GPR_U32(ctx, 0), 104)" in line:
                n += 1
                continue
            if "SET_GPR_S32(ctx, 4, (int32_t)ADD32(GPR_U32(ctx, 0), 96));" not in line:
                sys.exit(f"FAIL alloc site 0x{site:x} in {os.path.basename(f)}: unexpected line {line.strip()}")
            out.append(s[pos:k])
            out.append(line.replace("ADD32(GPR_U32(ctx, 0), 96));", "ADD32(GPR_U32(ctx, 0), 104)); " + MARK + " CMotionModel 0x60->0x68 (SvR06 0x68)"))
            pos = line_end
            n += 1
        out.append(s[pos:])
        ns = "".join(out)
        if ns != s:
            wr(f, ns)
        done += n
print(f"A: {done} alloc lines at 0x68 ({len(SITES)} sites)")

# ---- B + C. CMotionHokan::ActionLink
F = os.path.join(RUN, "ActionLink__12CMotionHokanFv_0x1b5b60.cpp")
s = rd(F)
if MARK not in s:
    s = once(s, '''label_1b5da4:
    // 0x1b5da4: 0x8e740038  lw          $s4, 0x38($s3)''', '''label_1b5da4:
    if (GPR_S32(ctx, 17) == 13) SET_GPR_S32(ctx, 5, 0x1d); // SVRNATIVE P3 (SvR 0x1a8a2c): l_momo -> l_momo_x
    else if (GPR_S32(ctx, 17) == 17) SET_GPR_S32(ctx, 5, 0x1e); // SVRNATIVE P3: r_momo -> r_momo_x
    // 0x1b5da4: 0x8e740038  lw          $s4, 0x38($s3)''', "P3 label_1b5da4")
    s = once(s, '''    // 0x1b5f6c: 0x46000306  mov.s       $f12, $f0 (Delay Slot)
    ctx->f[12] = FPU_MOV_S(ctx->f[0]);''', '''    // 0x1b5f6c: 0x46000306  mov.s       $f12, $f0 (Delay Slot)
    ctx->f[12] = FPU_MOV_S(ctx->f[0]);
    if (hctpSvrKoteHalf(rdram, GPR_U32(ctx, 19))) ctx->f[12] *= 0.5f; // SVRNATIVE P4 (SvR06 0x216660 flag 2): kote_x half twist''', "P4 0x1b5f6c")
    s = once(s, '#include "ps2_stubs.h"\n', '#include "ps2_stubs.h"\nbool hctpSvrKoteHalf(const uint8_t *rdram, uint32_t hokan); ' + MARK + '\n', "P4 decl")
    wr(F, s)
    print("B/C: patched", os.path.basename(F))
else:
    print("B/C: already patched")

# ---- D. CCharIK::ActionLink twist lists
F = os.path.join(RUN, "ActionLink__7CCharIKFv_0x1b8860.cpp")
s = rd(F)
if MARK not in s:
    s = once(s, '''int hctpIkSvrTwistBone(const uint8_t *rdram, uint32_t model, int which);
static int s_svrTwist = 0;
static const int kSvrTwist[14] = {6, 25, 7, 27, 10, 26, 11, 28, 13, 29, 14, 17, 30, 18};''',
             '''// SVRNATIVE Nov 12: list per model (hctpSvrIkMode on the CMotionModel shorts): 0 = HCTP 0x4129f0 (12), 1 = SvR04 0x346020 /
// SvR06 mode1 0x337b10 (14), 2 = SvR06 mode2 0x337b40 (16, adds kote_x2 31/32). Thresholds = limb group starts, ends = tip store.
int hctpSvrIkMode(const uint8_t *rdram, uint32_t motionModel);
static int s_svrTwist = 0;
static const int kSvrTwist[3][16] = {{6, 25, 7, 27, 10, 26, 11, 28, 13, 14, 17, 18},
                                     {6, 25, 7, 27, 10, 26, 11, 28, 13, 29, 14, 17, 30, 18},
                                     {6, 25, 7, 27, 31, 10, 26, 11, 28, 32, 13, 29, 14, 17, 30, 18}};
static const int kTwThr[3][3] = {{4, 8, 10}, {4, 8, 11}, {5, 10, 13}};
static const int kTwEnd[3][4] = {{3, 7, 9, 11}, {3, 7, 10, 13}, {4, 9, 12, 15}};
static const int kTwLen[3] = {12, 14, 16};''', "D tables")
    s = once(s, "    s_svrTwist = hctpIkSvrTwistBone(rdram, GPR_U32(ctx, 16), 0); // HAND EDIT Nov 7",
             "    s_svrTwist = hctpSvrIkMode(rdram, GPR_U32(ctx, 4)); // SVRNATIVE (a0 = CMotionModel)", "D mode")
    s = once(s, '''    if (s_svrTwist) // HAND EDIT Nov 7: SvR04 list; momo_x (29/30) by name, HCTP has no enum for them
    {
        const int id = kSvrTwist[GPR_S32(ctx, 18) & 15];
        if (id == 29 || id == 30)
        {
            SET_GPR_S32(ctx, 2, hctpIkSvrTwistBone(rdram, GPR_U32(ctx, 16), id == 29 ? 1 : 2));
            ctx->pc = 0x1B9350u;
            goto label_1b9350;
        }
        SET_GPR_S32(ctx, 5, id);
    }''', '''    if (s_svrTwist) // SVRNATIVE: SvR list; ids 29..32 are real CMotionModel shorts now (P1b), GetBoneNo = SvR 1:1
        SET_GPR_S32(ctx, 5, kSvrTwist[s_svrTwist][GPR_S32(ctx, 18) & 15]);''', "D id")
    s = once(s, '''        SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 18) < (int64_t)(int32_t)4) ? 1 : 0);''',
             '''        SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 18) < (int64_t)(int32_t)kTwThr[s_svrTwist][0]) ? 1 : 0); // SVRNATIVE''', "D thr0")
    s = once(s, '''    ctx->pc = 0x1b9368u;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 18) < (int64_t)(int32_t)8) ? 1 : 0);''', '''    ctx->pc = 0x1b9368u;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 18) < (int64_t)(int32_t)kTwThr[s_svrTwist][1]) ? 1 : 0); // SVRNATIVE''', "D thr1")
    s = once(s, "(int64_t)(int32_t)(s_svrTwist ? 11 : 10)) ? 1 : 0); // HAND EDIT Nov 7",
             "(int64_t)(int32_t)kTwThr[s_svrTwist][2]) ? 1 : 0); // SVRNATIVE", "D thr2")
    s = once(s, '''        // 0x1b9394: 0x24030003  addiu       $v1, $zero, 0x3 (Delay Slot)
        SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 3));''', '''        // 0x1b9394: 0x24030003  addiu       $v1, $zero, 0x3 (Delay Slot)
        SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), kTwEnd[s_svrTwist][0])); // SVRNATIVE''', "D end0a")
    s = once(s, '''    ctx->pc = 0x1b94d8u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 3));''', '''    ctx->pc = 0x1b94d8u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), kTwEnd[s_svrTwist][0])); // SVRNATIVE''', "D end0b")
    s = once(s, '''    ctx->pc = 0x1b94e4u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 7));''', '''    ctx->pc = 0x1b94e4u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), kTwEnd[s_svrTwist][1])); // SVRNATIVE''', "D end1")
    s = once(s, "ADD32(GPR_U32(ctx, 0), s_svrTwist ? 10 : 9)); // HAND EDIT Nov 7",
             "ADD32(GPR_U32(ctx, 0), kTwEnd[s_svrTwist][2])); // SVRNATIVE", "D end2")
    s = once(s, "ADD32(GPR_U32(ctx, 0), s_svrTwist ? 13 : 11)); // HAND EDIT Nov 7",
             "ADD32(GPR_U32(ctx, 0), kTwEnd[s_svrTwist][3])); // SVRNATIVE", "D end3")
    s = once(s, "(int64_t)(int32_t)(s_svrTwist ? 14 : 12)) ? 1 : 0); // HAND EDIT Nov 7",
             "(int64_t)(int32_t)kTwLen[s_svrTwist]) ? 1 : 0); // SVRNATIVE", "D len")
    wr(F, s)
    print("D: patched", os.path.basename(F))
else:
    print("D: already patched")

# ---- E/F/G (Nov 12 fix, plan sec 6.7). The overrides used replaceFunction wrappers that ran their code only when the original
# returned with pc == ra; the CMotionModel ctor never did (GetBoneNo_Name 0x1b4f20 tail-jumps `jr $t9` into the FindBone
# override) -> shorts 29..32 kept garbage. Now the helpers run INLINE at the exact points (no wrapper, no early-exit risk):
#   E ctor 0x1b4f50 loop exit 0x1b4fb8 (s1 = this, set 0x1b4f68, loop i = 29 done)  -> hctpSvrMmShorts
#   F CMotionHokan::BoneCreate 0x1b5710 epilogue 0x1b5b3c (s1 = hokan, set 0x1b5730; only jr $ra 0x1b5b58) -> hctpSvrBoneCreate
#   G CCharIK ctor 0x1b83d0 epilogue 0x1b8740 (s1 = this, set 0x1b83e4; after 0x1b8718 GetBoneNo(0x13) -> +0x154) -> hctpSvrIkLengths
INL = [
    ("E", "ps2___ct__12CMotionModelFP10CSkinModel_0x1b4f50.cpp", "void hctpSvrMmShorts(uint8_t *rdram, uint32_t self);",
     '''    ctx->pc = 0x1B4FB8u;
    // 0x1b4fb8: 0x220102d  daddu       $v0, $s1, $zero''',
     '''    ctx->pc = 0x1B4FB8u;
    hctpSvrMmShorts(rdram, GPR_U32(ctx, 17)); // SVRNATIVE E: shorts 29..32 (SvR04 0x345e60 / SvR06 0x337770), s1 = this
    // 0x1b4fb8: 0x220102d  daddu       $v0, $s1, $zero'''),
    ("F", "BoneCreate__12CMotionHokanFiP5CLink_0x1b5710.cpp", "void hctpSvrBoneCreate(uint8_t *rdram, uint32_t hokan);",
     '''    ctx->pc = 0x1B5B3Cu;
    // 0x1b5b3c: 0xdfbf0060  ld          $ra, 0x60($sp)''',
     '''    ctx->pc = 0x1B5B3Cu;
    hctpSvrBoneCreate(rdram, GPR_U32(ctx, 17)); // SVRNATIVE F: momo_x twist flags 13/17 (SvR04 0x1a8550), s1 = hokan
    // 0x1b5b3c: 0xdfbf0060  ld          $ra, 0x60($sp)'''),
    ("G", "ps2___ct__7CCharIKFP5CLink_0x1b83d0.cpp", "void hctpSvrIkLengths(uint8_t *rdram, uint32_t ik);",
     '''    { float f = ctx->f[0]; uint32_t bits; std::memcpy(&bits, &f, sizeof(bits)); WRITE32(ADD32(GPR_U32(ctx, 17), 340), bits); }
    // 0x1b8740: 0xdfbf0020  ld          $ra, 0x20($sp)''',
     '''    { float f = ctx->f[0]; uint32_t bits; std::memcpy(&bits, &f, sizeof(bits)); WRITE32(ADD32(GPR_U32(ctx, 17), 340), bits); }
    hctpSvrIkLengths(rdram, GPR_U32(ctx, 17)); // SVRNATIVE G: thigh += momo_x, forearm += kote_x2 (SvR04 0x1ab4d8, SvR06 0x21ad60), s1 = IK
    // 0x1b8740: 0xdfbf0020  ld          $ra, 0x20($sp)'''),
]
for tag, fn, decl, old, new in INL:
    F = os.path.join(RUN, fn)
    s = rd(F)
    if MARK in s:
        print(f"{tag}: already patched")
        continue
    s = once(s, old, new, f"{tag} call")
    s = once(s, '#include "ps2_stubs.h"\n', '#include "ps2_stubs.h"\n' + decl + " " + MARK + "\n", f"{tag} decl")
    wr(F, s)
    print(f"{tag}: patched", fn)
