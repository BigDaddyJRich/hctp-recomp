// Nov 14: shared by hctp_svr06_pyro.cpp (producer) and hctp_svr04_ctx.cpp (native SvR04 context). See hctpPyroSvr04Cues.
#pragma once
#include <cstdint>
#include <vector>
// copySrc >= 0: GMEV type-5 row = SvR04 gmParticleCopy (beta 0x459f20..0x45a0ac): an emitter whose MyAction also calls
// ParticleManager::SetCopyTrans(mgr, no, copySrc, copyOff) every frame (retail 0x2f6ac0); copyOff = own tag pos - source tag pos.
struct HctpSvr04Cue { int kind, no, scene, frame, endScene, endFrame, mode, part; float pos[3], rot[3]; int copySrc = -1; float copyOff[3] = {0, 0, 0}; int obj = 0; };
bool hctpPyroSvr04Cues(const char *id, std::vector<HctpSvr04Cue> &out);
bool hctpPyroBoneMatrix(const uint8_t *rd, int part, float M[16]);
// Nov 16 S8: world bone matrix [part] of the prop that SvR04 event `svrNo` has as obj `obj` (SEVP row), from its HCTP
// CSD3MotionWeapon (+0x34 = world bone copy, ActionLink 0x1c3ccc) = SvR04 SetParticleMatrixFromObject 0x2e51e4. hctp_overrides.cpp.
bool hctpSvrPropBoneMatrix(const uint8_t *rd, uint32_t svrNo, int obj, int part, float M[16]);
// Nov 17 HHH 4002 water: SvR06 attr op 0x6C = CAttribute::AttrFunc_CMD_Particle2 (beta 0x238fa0, table row 0x336fb0) ->
// StartParticleFromAtr(attr, p[1] list, p[2] no, p[3] part; 0xFF Death, 0xFE Vanish) 0x23c000. Recorded per SvR06 event by
// svr06Register (hctp_overrides.cpp) from the RAW EVNT bank; driven by the native SvR06 context (hctp_svr06_ctx.cpp).
struct HctpSvr06AttrCue { int obj, scene, frame, list, no, part; };
bool hctpSvr06AttrCues(uint32_t svrNo, std::vector<HctpSvr06AttrCue> &out);
// World bone matrix [part] of the prop that SvR06 event `svrNo` has as EVTS obj `obj` (its HCTP CSD3MotionWeapon +0x34).
bool hctpSvr06PropBoneMatrix(const uint8_t *rd, uint32_t svrNo, int obj, int part, float M[16]);
