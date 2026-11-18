// Public build (player kit): the SvR04 / SvR06 / SYM entrance contexts and the Season scene viewer need code recompiled
// from other discs (svr04_module.cpp, svr06_module.cpp, sym_module.cpp) that a player's build does not have. These are
// the same no-ops the real files run when their switch is off (HCTP_SVR04_CTX / HCTP_SVR06_CTX / HCTP_SYM_CTX /
// HCTP_SCENE_VIEWER unset): Install does nothing, Arm returns false, the tick and SE queues are empty.
// In a tree that has the real file (the dev runner) its stub compiles to nothing (__has_include).
#include "ps2_runtime.h"
#include <cstdint>
#include <utility>
#include <vector>

#if !__has_include("hctp_svr04_ctx.cpp")
bool hctpSvr04CtxArm(uint32_t) { return false; }
void hctpSvr04CtxEventTick(uint32_t, uint32_t) {}
void hctpSvr04CtxInstall(PS2Runtime &) {}
#endif

#if !__has_include("hctp_svr06_ctx.cpp")
bool hctpSvr06CtxArm(uint32_t) { return false; }
void hctpSvr06CtxEventTick(uint32_t, uint32_t) {}
void hctpSvr06CtxTakeSe(std::vector<int32_t> &out) { out.clear(); }
void hctpSvr06CtxInstall(PS2Runtime &) {}
#endif

#if !__has_include("hctp_sym_ctx.cpp")
bool hctpSymCtxArm(uint32_t) { return false; }
void hctpSymCtxEventTick(uint32_t, uint32_t) {}
void hctpSymCtxTakeSe(std::vector<std::pair<int, int>> &out) { out.clear(); }
void hctpSymCtxInstall(PS2Runtime &) {}
#endif

#if !__has_include("hctp_scene_viewer.cpp")
void hctpSceneViewerInstall(PS2Runtime &) {}
#endif

// SvR06/SvR04 entrance pyro (HCTP_SVR06_PYRO, hctp_svr06_pyro.cpp): it draws through a host-texture GS path that is not
// in the public runtime, and it only runs when MODS/SVR04 or MODS/SVR06 particle packs exist (other games' data).
// The player kit leaves that file out; these are its "no pack" answers (Has false, nothing armed or drawn).
#if !__has_include("hctp_svr06_pyro.cpp")
#include "hctp_svr04_cue.h"
void hctpPyroSetSource(int) {}
void hctpPyroArm(PS2Runtime *, const char *, uint32_t) {}
bool hctpPyroHas(const char *) { return false; }
void hctpPyroDisarm() {}
void hctpPyroNoteMotionModel(uint32_t) {}
bool hctpPyroEventTick(uint32_t, uint32_t, uint32_t) { return false; }
bool hctpPyroSvr04Cues(const char *, std::vector<HctpSvr04Cue> &out) { out.clear(); return false; }
bool hctpPyroBoneMatrix(const uint8_t *, int, float *) { return false; }
#endif
