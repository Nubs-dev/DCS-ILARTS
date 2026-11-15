// Cloud / image jitter in the PLAT away from the LSO station (CONFIRMED cause, 2026-11-09 tests):
// SceneRenderer draws every view relative to a render origin (snapped to 64 m) that follows the MAIN
// camera. At the station that is next to the PLAT camera; with the player 20+ km away the PLAT view is
// drawn 20+ km from its origin and jitters (user test: parked near the carrier = no jitter, parked far
// away = heavy jitter of clouds and image).
//
// SceneRenderer.dll (unprotected, same code in 2.9.25 and 2.9.31): view setup +0x14940 (view, camera, const Vec3d* origin, a4,
// 4 stack args) builds the view matrix from camera(+0x60) - origin and the culling planes, and passes
// float(origin) to the per-view context update +0x14FA0 (ctx+0x32C). Each caller computes the origin
// (e.g. via +0x103F0) - always from the main camera.
// First attempt (2026-11-09) replaced only +0x14FA0's origin: the PLAT then showed sky only (carrier and
// clouds missing) because the view matrix and culling still used the stock origin.
// Second attempt hooked the entry of +0x14940 and replaced the origin argument for the PLAT view only:
// clouds fine, carrier and sea lost - objects are placed against the frame-wide origin.
// Third attempt hooked Visualizer smCamera_Implement::SetOrigin: never called.
// Current (experiment): the frame origin itself, floor(DCSFrameDesc+0x80 / 64) * 64, is replaced for the
// whole frame at its two sources (see plat_origin.cpp). +0x14940 is still hooked, to observe only.
#pragma once

#include <string>

namespace lsodiag {

std::string PlatOriginInstall();  // "" on success, otherwise why not (verifies build and code bytes)
void PlatOriginRemove();
// 0 = off (wrappers pass through), 1 = whole frame gets the PLAT origin (proves the cause; the main view
// then shakes), 2 = the views at the carrier (the PLAT's and the extra cameras'): their view setups and
// their copies of the frame origin, all with one shared origin.
void PlatOriginEnable(int mode);
std::string PlatOriginStats();    // per call site: calls, PLAT-camera views, replaced, last distance
// Latest PLAT camera position (world, from the main thread); views are matched against it.
void PlatOriginNoteCamera(double x, double y, double z);
// For the thread building the render graph: true (and the frame's stock origin) when this frame's PLAT
// view was set up with its own origin.
bool PlatOriginViewFrameOrigin(double origin[3]);
// True when the render context at ctx (SceneRenderContext: its own origin at +0x32C) belongs to a view that
// was set up with the substituted origin in this frame (the PLAT's, an extra camera's).
bool PlatOriginContextSwapped(const void* ctx);
// The PLAT view's camera as last seen by the view setup hook; false if never seen.
bool PlatOriginPlatCamera(double cam[3]);

}  // namespace lsodiag
