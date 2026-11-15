// Cloud jitter investigation (PLAT clouds jitter only while the player is away from the LSO station).
//
// SceneRenderer.dll (unprotected) is the only importer of enlight.dll's volumetric cloud renderer:
//   CloudsModule::createRenderer(w, h, msaa, preset, name)      5 call sites
//   CloudsRenderer::addRenderingPass(graph, binder, sceneCtx, 5 textures, deps, bool, bool,
//                                    const Vec2f&, const Vec4f&)  call sites +0x2243E, +0x3C86E
// At +0x3C86E the renderer is [view+0x270], the first bool is [view+0x1F8]; the second bool and the
// Vec2f are computed per call. This probe redirects SceneRenderer's import entries for those two
// functions to pass-through recorders (no DCS code is modified) and reports, per cloud renderer,
// the arguments it was given, so the PLAT renderer can be compared at the station and away.
#pragma once

#include <string>

namespace lsodiag {

std::string CloudsProbeInstall();  // "" on success, otherwise why not
void CloudsProbeRemove();
// Per-renderer summary since the previous call. dump=true also asks each renderer's next call to
// copy its SceneRenderContext bytes into the log (one dump per renderer per request).
std::string CloudsProbeStats(bool dump);
// Experiment on the PLAT's cloud pass ("LSOFRAME" renderer), see clouds_probe.cpp. Off by default.
// mode 0 off, 1 full rebase to an origin next to the PLAT camera (fixes the blockiness but moves the
// clouds), 2 rebuild only the ray-direction matrix in double (the fix).
void CloudsRebase(int mode);
int CloudsRebaseMode();
// Deck jitter trace: record the PLAT camera position per PLAT frame while on; Dump logs CAMTRACE lines
// (QPC seconds, camera world xyz, view forward) and returns the sample count. CloudsQpc = QPC seconds.
void CloudsTrace(bool on);
int CloudsTraceDump(const char* label);
double CloudsQpc();
std::string CloudsRebaseStats();

}  // namespace lsodiag
