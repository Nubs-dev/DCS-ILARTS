// Shared-memory protocol between the DCS-side producer (ILARTS.dll) and the viewer (ILARTS.exe).
// Plain C layout, no pointers, versioned. Shared by src/share.cpp and viewer/lso_viewer.cpp.
#pragma once

#include <windows.h>

#include <cstdint>

namespace lsoshare {

constexpr wchar_t kMappingName[] = L"Local\\ILARTS_Share";
constexpr uint32_t kMagic = 0x534F534C;  // "LSOS"
constexpr uint32_t kVersion = 9;  // 2: compat and pass blocks, 3: several passes, 4: cameras, grades, mishaps, 5: camera PTZ, 6: camera views,
                                  // 7: 6 cameras / 16 targets (the deck camera's render slots),
                                  // 8: whose passes (auto recording filter), mission name
                                  // 9: each camera's pose (the viewer's camera tuning panel)
constexpr int kMaxTargets = 16;  // 4 of the PLAT and the station, then 2 per camera (LDR, HDR)
constexpr int kMaxCameras = 6;   // ISLAND and the deck camera's render slots
constexpr char kViewHangar = 'H';  // CameraViews::names[k][23]: a hangar deck position (it never tracks)
constexpr int kMaxViews = 16;  // v6: views (positions) one camera can be put at

// One shared render-target copy.
//   handleName: name of the NT handle (IDXGIResource1::CreateSharedHandle); open it with
//               ID3D11Device1::OpenSharedResourceByName. Changes whenever generation changes.
//   format:     DXGI_FORMAT of the shared texture (same as the DCS source).
//   Synchronisation: IDXGIKeyedMutex, key 0 for both sides. Producer uses timeout 0 (never blocks
//   DCS); consumers should hold the mutex only long enough to copy into their own texture.
struct TargetSlot {
    char name[32];
    wchar_t handleName[96];
    uint32_t width, height, format;
    uint32_t active;              // 1 while DCS has the source render target
    volatile LONG generation;     // incremented after handleName/size/format change (written last)
    uint32_t reserved0;
    volatile LONG64 frameCounter;  // incremented after each completed copy
    LONG64 lastCopyQpc;            // QueryPerformanceCounter at last copy
};

struct Header {
    uint32_t magic, version, targetCount, reserved0;
    LUID adapterLuid;  // consumers must create their device on this adapter
    uint32_t producerPid, reserved1;
    LONG64 qpcFrequency;
    TargetSlot targets[kMaxTargets];
    // Appended 2026-11-10 (layout above unchanged): short test/status text from the producer, shown by
    // the viewer in its sidebar. NUL-terminated ASCII; empty = nothing to show.
    char status[96];

    // Appended 2026-11-12 (layout above unchanged) ------------------------------------------------
    // Carriers the PLAT can sit on (written by the producer about once a second). selected = index
    // into carriers[] of the one the PLAT shows now, -1 = none.
    uint32_t carrierCount;
    int32_t selected;
    struct Carrier {
        char name[48];   // mission unit name
        char type[16];   // DCS type, e.g. "CVN_72"
        float x, z;      // position (m, mission coordinates)
    } carriers[8];

    // PLAT text overlay values, already formatted the way Supercarrier's PLATCameraUI.lua prints them
    // (the viewer draws them when DCS's own overlay is not in the picture). seq is incremented after
    // each update.
    struct Symbology {
        volatile LONG seq;
        uint32_t valid;          // 1 = current
        uint32_t dcsOverlay;     // 1 = DCS draws its own overlay into LSOFRAME (do not draw a second one)
        uint32_t night;          // 1 = white text (night), 0 = black (day)
        uint32_t statusVisible;  // carrier status letter shown (it flashes while foul)
        uint32_t waveoff;        // "W" shown
        char date[12], time[12], status[4], carrierId[4], deckWind[4], speed[4], distance[8], sink[4];
        float gateFovDeg, gateDistance, gateDeltaH;  // adjustGate(fov, distance, optimal_glissade_delta_h)
    } sym;

    // Written by the viewer: a carrier chosen by name. The producer acts on each new selectSeq.
    struct Control {
        volatile LONG selectSeq;
        char selectName[48];
        // v4: extra cameras the viewer wants rendered (bit i = cameras[i]); acted on at each new cameraSeq
        volatile LONG cameraSeq;
        uint32_t cameraMask;
    } control;

    // LSO station screen ("LSO 1") values, formatted as PLATCameraUI.lua prints them; the viewer draws
    // the screen itself when DCS's LSOFINALTARGET does not exist (no station visit). Updated together
    // with sym (sym.seq). Empty strings keep the screen's defaults.
    struct Station {
        uint32_t valid;
        char heading[16], list[24], trim[24], cross[8];
        int32_t crossSide;  // 1 port, -1 starboard, 0 none
        int32_t foul;       // setFoulDeck value: 0 clear, > 0 foul
        char acType[24], acHE[24], acBA[24];
        float htrFt, htdFt;  // hook-to-ramp / touchdown markers (ft); 0 = default
    } station;

    // LSO 2 cue panel (setCuePanelInfo rows) and the case III stack of LSO 1; seq incremented after each
    // update (about once a second).
    struct Cue {
        volatile LONG seq;
        uint32_t count;
        struct Row {
            int32_t status;      // -1 / 0 / 1 / 2 as Supercarrier: > 0 = case III (drawn on the approach picture)
            char type[24];       // DCS type
            char board[8];       // board (onboard) number
            char pilot[32];      // player or unit name
            float fuelLb;        // < 0 unknown
            float x, z, yaw;     // m from the ramp along / across the angled deck axis, rad
        } rows[16];
        char case3[3][8];
    } cue;

    // Version 2 ----------------------------------------------------------------------------------------
    // DCS compatibility (src/compat.h). state: 0 not checked yet, 1 the verified build, 2 another build
    // whose code the DLL depends on is unchanged (runs normally), 3 not compatible (nothing installed: the
    // viewer shows a warning instead of the feeds).
    struct Compat {
        uint32_t state;
        char dcsVersion[32];       // running DCS
        char verifiedVersion[32];  // the build this release was made for
        char detail[160];          // what differs (state 3)
    } compat;

    // Carrier approaches seen by the PLAT (written by the hook with sym, ~10 times a second): the passes under
    // way and the most recently ended ones, up to 4. A pass starts when an aircraft heading for the deck comes
    // into the PLAT camera's view and ends at the trap, bolter or wave-off; closely spaced aircraft each have
    // their own. id increments per pass within a mission; passEpoch changes with every mission.
    uint32_t passEpoch;
    uint32_t passCount;
    struct Pass {
        uint32_t id;
        uint32_t active;  // 1 while the aircraft is in the groove
        int32_t result;   // 0 none yet, 1 trap, 2 bolter, 3 wave-off, 4 other (lost / timed out), 5 not an approach (drop it)
        float range;      // m from the PLAT camera
        char type[24];    // DCS type
        char board[8];    // board (onboard) number, "" if none
        char pilot[32];   // player or unit name
        char carrier[16]; // carrier type, e.g. "CVN_72"
        // v4
        int32_t caseNo;     // recovery case 1 / 2 / 3, 0 = unknown
        char callsign[24];  // player name, else the unit's callsign, else its name
        char grade[16];     // LSO grade from Supercarrier ("OK", "(OK)", "---", "C", "WO", "B" ...), "" = none (yet)
        char wire[4];       // wire caught, "" = none / unknown
    } passes[4];

    // Version 4 ----------------------------------------------------------------------------------------
    // Extra cameras (DLL extra_cams.h): fixed cameras on the carrier that DCS renders like cockpit TV
    // displays into the render targets ldr (after DCS's TV look) and hdr (scene-linear), both shared as
    // targets[] when the camera is enabled. available = the camera can be rendered on this DCS build.
    uint32_t cameraCount;
    struct Camera {
        char name[16];  // "ISLAND", "CATAPULT"
        char ldr[32];
        char hdr[32];
        uint32_t available;
        uint32_t enabled;  // rendering now
        float aspect;      // width / height the picture is meant to be shown at (the target may be square)
    } cameras[kMaxCameras];

    // A mishap near the carrier (crash, ejection, an aircraft lost during its pass): seq increments with each
    // one; the viewer's black box keeps its footage.
    struct Mishap {
        volatile LONG seq;
        char text[64];
    } mishap;

    // Version 5 ----------------------------------------------------------------------------------------
    // Manual pan / tilt / zoom of the extra cameras (the viewer's joystick). The viewer writes rates -1 .. 1
    // (pan + = right, tilt + = up, zoom + = in) and stamps tick (GetTickCount64) while the stick is held; rates
    // older than 0.3 s count as 0 (a viewer that went away cannot leave a camera turning). resetSeq + 1 = back to
    // the rest view and to automatic tracking.
    // pictureType: 0 = the camera's configured picture (black-and-white TV), else DCS's picture type to use
    // (4 = infrared / FLIR, experimental).
    struct PtzControl {
        float pan, tilt, zoom;
        volatile LONG resetSeq;
        volatile LONG64 tick;
        int32_t pictureType;
        // What the camera follows automatically (the viewer; the hook reads it): 0 = the camera's own setting,
        // 1 = nothing (it stays at rest), 2 = the recovery (landing area), 3 = the catapult nearest it,
        // 4 = the active catapult, 5..8 = catapult 1..4.
        int32_t trackMode;
    } ptz[kMaxCameras];
    // What each camera does (the DLL, every frame it renders): degrees off its rest view, zoom factor (rest field
    // of view / now), manual = 1 while it follows the joystick (automatic tracking suspended until a reset).
    struct PtzState {
        float panDeg, tiltDeg, zoom;
        uint32_t manual;
    } ptzState[kMaxCameras];

    // Version 6 ----------------------------------------------------------------------------------------
    // Views of the extra cameras: the positions one camera can be put at (the deck camera: bow bubble, waist
    // bubble, elevators ...). The hook lists them (count, names) and says which one is shown (current; -1 = the
    // camera has no views). The viewer asks for one by writing request (an index) and then incrementing
    // requestSeq. Moving a camera costs nothing: it is still one picture for DCS to render.
    struct CameraViews {
        volatile LONG requestSeq;  // viewer
        int32_t request;           // viewer
        int32_t current;           // hook
        uint32_t count;            // hook
        char names[kMaxViews][24]; // hook; a name has up to 22 characters, and its last byte (names[k][23]) holds
                                   // kViewHangar for a hangar deck position (0 otherwise; older readers stop at the NUL)
    } views[kMaxCameras];

    // Version 8 ----------------------------------------------------------------------------------------
    // Whose passes they are (the viewer's automatic recording filter: your aircraft, section, flight or all), from the
    // hook with the passes. ownAircraft: 0 = not reported (a hook older than the DLL: treat every pass as matching),
    // 1 = you are in an aircraft, or were earlier in this mission (you landed and went to the LSO station, or released
    // the slot: the flags then follow that last aircraft), 2 = you have had none this mission (spectator, LSO / observer).
    // passFlags[i] belongs to passes[i]: bit 0 = flown by your aircraft, bit 1 = your section (your 2-ship element:
    // units 1-2, 3-4 ... of your group), bit 2 = your flight (your group). Your own aircraft sets all three.
    uint32_t ownAircraft;
    uint32_t passFlags[4];
    // The mission's name (hook: DCS.getMissionName(), else the file name without .miz), UTF-8, NUL-terminated,
    // "" until the hook has published it. Written with sym (same seq); the viewer names each mission's
    // recordings folder after it.
    char missionName[64];

    // Version 9 ----------------------------------------------------------------------------------------
    // Where each camera is: the hook's settings for what it shows (its rest view, without the joystick's pan / tilt /
    // zoom), as last sent to the DLL ('set'), in the carrier's axes (m from the model origin: x forward, y up, z
    // starboard). name = the view shown (a deck position), else the camera's own name: the name its entry in the
    // tuning file (Saved Games\DCS\Config\ILARTS-cameras.lua) has. seq increments after each change; valid = 0
    // until the hook has sent the camera.
    struct CameraPose {
        volatile LONG seq;
        uint32_t valid;
        char name[24];
        float pos[3], look[3];
        float fovDeg;  // horizontal
    } camPose[kMaxCameras];
};

constexpr uint32_t kPassOwn = 1, kPassSection = 2, kPassFlight = 4;  // Header::passFlags bits

}  // namespace lsoshare
