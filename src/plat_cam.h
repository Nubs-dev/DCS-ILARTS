// Synthetic PLAT camera: lets the PLAT render without ever opening the LSO view, on any carrier.
//
// Visualizer's PLAT view setup (+0xBCFC0) asks globalView slot 31 for the LSO camera and uses only
// three of its virtuals: +0x08 pose (osg::Matrixd by hidden pointer), +0x10 vertical FOV (rad) and
// +0x18 aspect. DCS's own camera (measured 2026-11-11, 1511 station samples) sits at a fixed point in
// the carrier's axes (-14.0, 20.5, -14.8713 m), is level (roll 0), pitched 3.6 deg up and points at
// the carrier's heading + 170.694 deg. This module answers that one call site with its own object
// built the same way from the matrix Visualizer draws the carrier with this frame (+0x125B50, the
// refresh path's own function), so the camera and the deck move together exactly.
//
// The carrier's scene object is found by position and axis (PlatCamNoteObject, from the hull refresh
// hand-off; run 2026-11-12: carriers do NOT have flag 0x40 there). Until the carrier's scene object
// is known, the pose comes from the mission's unit pose pushed by the hook (PlatCamShip).
#pragma once

#include <string>

namespace lsodiag {

std::string PlatCamEnable(bool on);  // "" or why it cannot run
bool PlatCamEnabled();

// Carrier to sit on: its current position (mission coordinates) and DCS type name.
void PlatCamSelect(double x, double y, double z, const char* type);

// The selected carrier's pose from the mission state: x, y, z axes (9) and position (3).
void PlatCamShip(const double* axesPos12);

// From the hull refresh hand-off whenever it computes an object's matrix m (every 0.25-1 s per object).
void PlatCamNoteObject(void* obj, const double* m, void* visualizer);

// globalView slot 31 hook, Visualizer's PLAT view setup call only. dcsCamera = what DCS returned
// (null away from the station). Returns the synthetic camera, or null to keep DCS's own answer.
void* PlatCamForViewSetup(void* dcsCamera);

// Pose last handed to Visualizer (row-major osg::Matrixd); false if none yet / older than 1 s.
bool PlatCamPose(double* m16);

// The carrier matrix the last PLAT pose was built from (row-major: x, y, z axes, position); false if none
// yet / older than 1 s. Same thread as the view setup.
bool PlatCamShipMatrix(double* m16);

// Extra cameras (extra_cams.h): while a pose is set, the PLAT view setup call gets a camera with this pose
// (row-major osg::Matrixd: forward, up, right, position), horizontal field of view (rad) and aspect instead
// of the PLAT's. nullptr ends it. Set and cleared around one call, on the view setup's thread.
void PlatCamOverride(const double* pose16, double fovRad, double aspect);

// Counters since the last call, and the binding.
std::string PlatCamStats();

}  // namespace lsodiag
