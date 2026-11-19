#pragma once

#include <string>
#include <vector>

namespace lsodiag {

// Phase 5 producer. Start/stop/status may be called from the Lua (main) thread. All D3D work -
// revalidation, shared texture creation, per-frame copies, captures - runs on the DCS render
// thread inside the EndFrame renderer callback (see render_callback.h).
std::string ShareStart(const std::vector<std::string>& names);  // "" on success, else reason
// The mapping with the compat block only (DCS not compatible: the viewer shows why, nothing else runs).
std::string ShareAnnounce();
void ShareFrame();                                               // no-op (kept for old scripts)
std::string QueueCapture(const std::string& name);              // capture on next render frame
std::string QueueMarker(int frames);                            // frame-order experiment
void ShareSetStatusText(const char* text);                       // Header::status (viewer sidebar)
void ShareSetCarriers(const char* list, int selected);             // Header::carriers ("name|type|x|z" lines)
void ShareSetSymbology(const char* keyValueLines);                  // Header::sym
void ShareSetCue(const char* lines);                                // Header::cue (LSO 2 rows, case III stack)
long ShareReadControl(std::string* name);                           // viewer's carrier request (seq, name)
long ShareReadCameraView(int i, int* request, int* trackMode = nullptr);  // Header::views[i] request (seq, index); ptz[i].trackMode
void ShareSetCameraViews(int i, int current, const char* names);    // Header::views[i]: "NAME|NAME|..." and the one shown
void ShareSetCameras(const char* lines);                            // Header::cameras ("name|ldr|hdr|available|enabled|aspect")
long ShareReadCameraControl(unsigned* mask);                        // viewer's camera request (seq, mask)
// The viewer's joystick for extra camera i: rates -1..1 (0 when stale) and the reset count; false = no header.
bool ShareReadPtz(int i, float* pan, float* tilt, float* zoom, long* resetSeq, int* pictureType);
void ShareWritePtzState(int i, float panDeg, float tiltDeg, float zoom, bool manual);
// Header::camPose[i] (v9): what camera i shows (view or camera name) and its rest pose, from the hook's 'set'.
void ShareSetCameraPose(int i, const char* name, const double* pos, const double* look, double fovDeg);
void ShareSetMishap(const char* text);                              // Header::mishap (seq + 1)
std::string ShareRetarget(int slot, const char* name);              // share another render target through a slot
long long ShareFrameCount();                                        // DCS frames seen by the render callback
int ShareTargetExists(const char* name);                            // 1 / 0 (render thread's last check), -1 = not known yet
std::string ShareStatus();                                       // one-line counters
void ShareStop();                                                // releases everything

}  // namespace lsodiag
