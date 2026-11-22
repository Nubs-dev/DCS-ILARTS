// Extra cameras: fixed cameras on the carrier (island, catapult station), rendered by DCS the same way as
// the PLAT.
//
// DCS keeps 34 "picture sources" (Visualizer+0x2E27E8, 0x330 bytes each): 0-31 are the cockpit's TV displays
// (target "mfd<n>", buffers "hdrBufferMFD<n>"), 32 is the PLAT, 33 the tactical map. Every frame the function
// at Visualizer+0x18FB50 rebuilds them: the cockpit ones first (an unused one is marked invalid), then the PLAT
// through `call +0xBCFC0` at +0x1912C1 (builder(out, index 32, &viewportId 0x84)). Everything after that - the
// object searches (+0x171B20 ...), SceneRenderer's per-source pipelines - walks all valid sources alike.
//
// This module redirects that one call. After DCS has built the PLAT's source it runs DCS's own builder again
// for each enabled camera, into an unused cockpit source (30, 31): the builder asks globalView slot 31 for the
// camera, and plat_cam.h answers that call with the extra camera's pose (PlatCamOverride). The result is
// stored with DCS's own assignment (+0x184F10) and released with its destructor (+0xBBD90). The source's
// overlay callback is replaced with one that draws nothing (the PLAT's would draw the PLAT symbology).
//
// Nothing is patched until a camera is first enabled. Needs the synthetic PLAT camera (the carrier's matrix
// and the slot-31 answer come from it). NOT YET RUN IN DCS when this was written (2026-11-13).
#pragma once

#include <string>

namespace lsodiag {

constexpr int kExtraCams = 6;  // = lsoshare::kMaxCameras: ISLAND and the deck camera's render slots

// "" when the cameras can be used on this DCS (code as expected, synthetic PLAT camera running), else why not.
std::string ExtraCamsAvailable();

// Camera `index` (0 .. kExtraCams-1): position and look-at point in the carrier's axes (m: x forward, y up,
// z starboard, from the model origin), horizontal field of view (deg), picture aspect, the cockpit picture
// source it uses (0-31) and the picture type (0 = as the PLAT: black-and-white TV).
void ExtraCamsConfigure(int index, const double* pos, const double* look, double fovDeg, double aspect, int slot, int type);

// Bit i = camera i renders. Installs the redirect on first use. "" or why it could not.
std::string ExtraCamsEnable(unsigned mask);
unsigned ExtraCamsMask();

// The camera's pan head while it tracks (ptz.h): fixed rate (deg/s), ramp up / down time (s), and how far off
// (deg) the aircraft may be before the camera moves. The camera only pans; its tilt stays the rest view's.
// only: while tracking, +1 = pans only toward larger pan angles (atan2(z, x) in the carrier's axes: for a camera
// looking aft that is to the right), -1 = only smaller, 0 = both ways. The return to the rest view is not limited.
void ExtraCamsPtz(int index, double rateDeg, double rampSeconds, double deadbandDeg, int only);

// Camera `index` aims at an aircraft at p (carrier's axes, m) at mission time modelTime (s); called ~10 times
// a second while it lasts. p = nullptr: back to the camera's own look-at point (eased).
void ExtraCamsTrack(int index, const double* p, double modelTime);

// For the render origin fix: is the world position within `metres` of a camera that is rendering (as of this
// frame)? And: is `name` the render target ("mfd30") of a camera that is rendering?
bool ExtraCamsNear(const double* world, double metres);
// Camera `index` shows a hangar deck position (the hook's view flag `hangar`): no rain or snow in its picture (rain.cpp).
void ExtraCamsSetHangar(int index, bool hangar);
// Is the world position within `metres` of a rendering camera that shows a hangar deck position?
bool ExtraCamsHangarNear(const double* world, double metres);
bool ExtraCamsTargetName(const char* name);

// Does the cockpit itself use picture source `source` (0-31)? True if DCS filled it within the last 10 s (seen
// each frame before the cameras add theirs, while the redirect runs), else from the source's own valid byte.
// Any thread. The viewport resize (mfd.cpp) leaves such a source alone, as the cameras do.
bool ExtraCamsCockpitSource(int source);

void ExtraCamsRemove();

// Counters since the last call.
std::string ExtraCamsStats();

}  // namespace lsodiag
