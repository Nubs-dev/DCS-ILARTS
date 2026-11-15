// Q4 (PLAT keeps updating away from the LSO station) - runtime observation of the LSO camera.
//
// Visualizer (unprotected, 2.9.31) builds the PLAT view each frame from
//   cam = Inter.dll!globalView->vfunc(+0xF8)        (null => PLAT view skipped)
// and uses on cam: +0x08 pose (returns osg::Matrixd via hidden pointer), +0x10 / +0x18 (doubles),
// +0x20 (per-frame void call during the PLAT render, presumably the symbology draw).
// The IView implementation is in protected DCS.exe and is not analysed; this probe only calls the
// same interface functions Visualizer calls (except +0x20) and reads memory.
#pragma once

#include <string>

namespace lsodiag {

// Logs the camera state; detailed lines only when something changed. Returns a one-line summary.
std::string LsoCameraProbe();
// Per-frame statistics of the PLAT camera pose since the last call (heading and position steps).
std::string LsoPoseTraceStats();
// The latest sampled pose as "qpc_seconds yaw_deg x y z", or "none".
std::string LsoPoseLast();
// Camera orientation follows the ship every frame (see lso_camera.cpp). The hook passes the ship's axes
// each simulation frame, then samples the camera pose (LsoCameraProbe) in the same frame.
bool LsoSmoothShip(const char* text);
// Position (x, y, z) of the PLAT camera at the latest per-frame sample, and the sample's age.
bool LsoLastCameraPosition(double* out, double* ageSeconds);
std::string LsoSmoothEnable(bool on);  // "" on success
std::string LsoSmoothStats();

// Keep the PLAT view rendering away from the LSO station: makes the view system keep returning the
// last LSO camera when DCS's own lookup returns null (private-vtable swap on the globalView object).
// Start returns "" on success, otherwise why it refused. Stop restores DCS's vtable pointer.
std::string LsoKeepPlatStart();
std::string LsoKeepPlatStop();
// Per-slot globalView call counts since the previous call (all callers, and edSupercarrier.dll callers).
std::string LsoViewCallStats();
// While the PLAT camera is substituted, give Supercarrier (only) the globalView slot 12/13 answers it got
// at the LSO station, so it keeps driving the PLAT symbology.
void LsoSupercarrierOverride(bool on);

}  // namespace lsodiag
