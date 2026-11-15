#include "plat_cam.h"

#include "common.h"
#include "compat.h"
#include "engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace lsodiag {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

// DCS's PLAT camera in the carrier's axes (x forward, y up, z starboard), measured 2026-11-11 from
// HDGTRACE (CVN-72, 1511 samples at DCS's own camera updates): offset sd < 0.1 mm, pitch exactly 3.6,
// roll exactly 0, heading offset 170.694 deg (sd 0.007 = DCS's own stepping).
constexpr double kOffset[3] = {-14.0, 20.5, -14.8713};
constexpr double kYawOffsetDeg = 170.694;
constexpr double kPitchDeg = 3.6;
constexpr double kFov = 0.296706;          // DCS camera +0x10 (rad, vertical)
constexpr double kAspect = 4.0 / 3.0;      // DCS camera +0x18

constexpr uintptr_t kMatrixFnRva = 0x125B50;   // object matrix (out, object, parent or null, time)
constexpr uintptr_t kFrameTimeOffset = 0x1D0;  // double in the Visualizer object: this frame's time
constexpr uintptr_t kFrameTimeRva = 0x2E1778;  // Visualizer global, same value (fallback)
constexpr double kBindMetres = 300.0;          // scene object must be this close to the chosen carrier
constexpr ULONGLONG kSeenMs = 2000;            // scene object handed off within this time = alive

using MatrixFn = double* (*)(double* out, void* obj, const double* parent, double time);

struct Ship {
    void* obj = nullptr;
    const void* vptr = nullptr;
    double pos[3] = {};
    ULONGLONG seen = 0, posTick = 0;
};

struct State {
    bool enabled = false;
    HMODULE vis = nullptr;
    MatrixFn matrix = nullptr;
    void* visObj = nullptr;              // Visualizer object (ctx[2] of the hand-off)
    Ship ships[32];
    // selection
    bool haveTarget = false;
    double target[3] = {};
    char type[32] = {};
    Ship* bound = nullptr;
    // mission pose of the selected carrier
    double lua[12] = {};
    ULONGLONG luaTick = 0;
    // last pose handed out
    double pose[16] = {};
    ULONGLONG poseTick = 0;
    // the carrier matrix that pose was built from (rows x, y, z axes and position)
    double shipM[16] = {};
    // counters
    long long handed = 0, fromObject = 0, fromLua = 0, none = 0, axisMismatch = 0, matrixFault = 0, rebinds = 0;
    double maxPosVsLua = 0, maxAxisVsLua = 0;
    // check against DCS's own camera while it exists (at a station)
    long long dcsChecks = 0, dcsOtherShip = 0;  // other ship: DCS's camera is on a different carrier (station vs selection)
    double dcsMaxPos = 0, dcsMaxHdg = 0;
} g;

struct CamObject {
    const void* vtbl;
} g_obj;
const void* g_vtbl[8] = {};

void* CamDtor(void* self, unsigned) { return self; }
void* CamMatrix(void*, void* ret) {
    memcpy(ret, g.pose, sizeof(g.pose));
    return ret;
}
double CamFov(void*) { return kFov; }
double CamAspect(void*) { return kAspect; }
uintptr_t CamZero(void*, uintptr_t, uintptr_t, uintptr_t) { return 0; }

// A second camera object for the extra cameras (extra_cams.h): while an override is set, the PLAT view setup
// gets this one, with the pose / field of view / aspect given.
CamObject g_objX;
const void* g_vtblX[8] = {};
bool g_xActive = false;
double g_xPose[16] = {}, g_xFov = kFov, g_xAspect = kAspect;
void* CamMatrixX(void*, void* ret) {
    memcpy(ret, g_xPose, sizeof(g_xPose));
    return ret;
}
double CamFovX(void*) { return g_xFov; }
double CamAspectX(void*) { return g_xAspect; }

bool SehMatrix(void* obj, double time, double* out) {
    __try {
        g.matrix(out, obj, nullptr, time);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

double FrameTime() {
    double t = 0;
    if (g.visObj && SafeRead(static_cast<const char*>(g.visObj) + kFrameTimeOffset, &t, sizeof(t))) return t;
    if (g.vis) SafeRead(reinterpret_cast<const char*>(g.vis) + kFrameTimeRva, &t, sizeof(t));
    return t;
}

bool Alive(const Ship* s, ULONGLONG now) {
    if (!s || !s->obj || now - s->seen > kSeenMs) return false;
    const void* v = nullptr;
    return SafeReadPtr(s->obj, &v) && v == s->vptr;
}

double Dist2(const double* a, const double* b) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

void Rebind(ULONGLONG now) {
    if (!g.haveTarget) return;
    Ship* best = nullptr;
    double bd = kBindMetres * kBindMetres;
    for (Ship& s : g.ships) {
        if (!Alive(&s, now) || !s.posTick) continue;
        const double d = Dist2(s.pos, g.target);
        if (d < bd) bd = d, best = &s;
    }
    if (best != g.bound) {
        ++g.rebinds;
        if (best)
            Log("PLATCAM bound to scene object %p (%.0f m from the chosen carrier %s)", best->obj, sqrt(bd), g.type);
        else if (g.bound)
            Log("PLATCAM scene object of %s lost; using the mission pose", g.type);
        g.bound = best;
    }
}

double Angle(const double* a, const double* b) {
    const double la = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (la < 1e-9 || lb < 1e-9) return 180;
    double c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
    if (c > 1) c = 1;
    if (c < -1) c = -1;
    return acos(c) / kDeg;
}

// Builds the camera pose for this frame into g.pose. False = no carrier pose available.
bool BuildPose() {
    const ULONGLONG now = GetTickCount64();
    double M[16];
    bool have = false;
    const bool luaFresh = g.luaTick && now - g.luaTick < 300;
    if (g.bound && Alive(g.bound, now)) {
        if (SehMatrix(g.bound->obj, FrameTime(), M)) {
            have = true;
            if (luaFresh) {
                // sanity check against the mission pose (wrong object, other axis convention)
                const double ang = Angle(M, g.lua);
                const double dp = sqrt(Dist2(M + 12, g.lua + 9));
                if (ang > g.maxAxisVsLua) g.maxAxisVsLua = ang;
                if (dp > g.maxPosVsLua) g.maxPosVsLua = dp;
                if (ang > 2.0 || dp > 100.0) {
                    ++g.axisMismatch;
                    have = false;  // do not trust this object's matrix
                    g.bound = nullptr;
                }
            }
            if (have) ++g.fromObject;
        } else {
            ++g.matrixFault;
            g.bound = nullptr;
        }
    }
    if (!have && luaFresh) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) M[r * 4 + c] = g.lua[r * 3 + c];
        M[12] = g.lua[9];
        M[13] = g.lua[10];
        M[14] = g.lua[11];
        have = true;
        ++g.fromLua;
    }
    if (!have) {
        ++g.none;
        return false;
    }
    memcpy(g.shipM, M, sizeof(g.shipM));
    const double* X = M;
    const double* Y = M + 4;
    const double* Z = M + 8;
    const double h = atan2(X[2], X[0]) + kYawOffsetDeg * kDeg;
    const double p = kPitchDeg * kDeg;
    const double ch = cos(h), sh = sin(h), cp = cos(p), sp = sin(p);
    double* o = g.pose;
    o[0] = cp * ch, o[1] = sp, o[2] = cp * sh, o[3] = 0;     // forward
    o[4] = -sp * ch, o[5] = cp, o[6] = -sp * sh, o[7] = 0;   // up
    o[8] = -sh, o[9] = 0, o[10] = ch, o[11] = 0;             // right
    for (int k = 0; k < 3; ++k) o[12 + k] = M[12 + k] + kOffset[0] * X[k] + kOffset[1] * Y[k] + kOffset[2] * Z[k];
    o[15] = 1;
    g.poseTick = now;
    return true;
}

void CheckAgainstDcs(void* cam) {
    const void* const* v = nullptr;
    if (!SafeRead(cam, &v, sizeof(v)) || !v || !g.poseTick) return;
    double m[16] = {};
    void* ret = nullptr;
    __try {
        ret = reinterpret_cast<void* (*)(void*, void*)>(const_cast<void*>(v[1]))(cam, m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    double d[16];
    if (!SafeRead(ret ? ret : m, d, sizeof(d))) return;
    const double dp = sqrt(Dist2(d + 12, g.pose + 12));
    const double dh = Angle(d, g.pose);
    if (dp > 1000.0) {  // not the same carrier (e.g. the frames between a station visit and the re-selection)
        ++g.dcsOtherShip;
        return;
    }
    ++g.dcsChecks;
    if (dp > g.dcsMaxPos) g.dcsMaxPos = dp;
    if (dh > g.dcsMaxHdg) g.dcsMaxHdg = dh;
}

}  // namespace

std::string PlatCamEnable(bool on) {
    if (!on) {
        g.enabled = false;
        return "";
    }
    HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
    if (!vis) return "Visualizer.dll not loaded";
    if (!CompatOk()) return CompatWhy();
    unsigned char head[3];
    if (!SafeRead(reinterpret_cast<const char*>(vis) + kMatrixFnRva, head, sizeof(head))) return "Visualizer+0x125B50 not readable";
    const bool plain = head[0] == 0x48 && head[1] == 0x8B && head[2] == 0xC4;
    const bool tapped = head[0] == 0xFF || head[0] == 0xE9;
    if (!plain && !tapped) return "Visualizer+0x125B50 is not the expected code";
    g.vis = vis;
    g.matrix = reinterpret_cast<MatrixFn>(reinterpret_cast<char*>(vis) + kMatrixFnRva);
    g_vtbl[0] = reinterpret_cast<const void*>(&CamDtor);
    g_vtbl[1] = reinterpret_cast<const void*>(&CamMatrix);
    g_vtbl[2] = reinterpret_cast<const void*>(&CamFov);
    g_vtbl[3] = reinterpret_cast<const void*>(&CamAspect);
    for (int i = 4; i < 8; ++i) g_vtbl[i] = reinterpret_cast<const void*>(&CamZero);
    g_obj.vtbl = g_vtbl;
    g_vtblX[0] = reinterpret_cast<const void*>(&CamDtor);
    g_vtblX[1] = reinterpret_cast<const void*>(&CamMatrixX);
    g_vtblX[2] = reinterpret_cast<const void*>(&CamFovX);
    g_vtblX[3] = reinterpret_cast<const void*>(&CamAspectX);
    for (int i = 4; i < 8; ++i) g_vtblX[i] = reinterpret_cast<const void*>(&CamZero);
    g_objX.vtbl = g_vtblX;
    g.enabled = true;
    Log("PLATCAM synthetic PLAT camera enabled (object %p)", static_cast<void*>(&g_obj));
    return "";
}

bool PlatCamEnabled() { return g.enabled; }

void PlatCamSelect(double x, double y, double z, const char* type) {
    const bool changed = !g.haveTarget || strcmp(g.type, type ? type : "") != 0 ||
                         (x - g.target[0]) * (x - g.target[0]) + (z - g.target[2]) * (z - g.target[2]) > 1000.0 * 1000.0;
    g.target[0] = x, g.target[1] = y, g.target[2] = z;
    strncpy_s(g.type, type ? type : "", _TRUNCATE);
    g.haveTarget = true;
    if (changed) {
        g.bound = nullptr;  // force a fresh pick
        g.luaTick = 0;      // the mission pose pushed so far belongs to the previous carrier
        Log("PLATCAM carrier chosen: %s at (%.0f, %.0f, %.0f)", g.type, x, y, z);
    }
    Rebind(GetTickCount64());
}

void PlatCamShip(const double* v) {
    // a pose of another ship (the hook's ship query still on the previous carrier right after a switch)
    if (g.haveTarget && (v[9] - g.target[0]) * (v[9] - g.target[0]) + (v[11] - g.target[2]) * (v[11] - g.target[2]) > 1000.0 * 1000.0)
        return;
    memcpy(g.lua, v, sizeof(g.lua));
    g.luaTick = GetTickCount64();
    // the mission pose is the freshest position of the chosen carrier: keep the target on it
    if (g.haveTarget) {
        g.target[0] = v[9], g.target[1] = v[10], g.target[2] = v[11];
        if (!g.bound) Rebind(g.luaTick);
    }
}

void PlatCamNoteObject(void* obj, const double* m, void* visualizer) {
    if (!obj || !m || !g.enabled) return;
    if (visualizer) g.visObj = visualizer;
    // only the scene object that IS the chosen carrier: its origin within 50 m of the carrier's mission
    // position and its forward axis within 3 deg of the mission pose (rules out aircraft parked on deck)
    if (!g.haveTarget || Dist2(m + 12, g.target) > 50.0 * 50.0) return;
    const ULONGLONG now = GetTickCount64();
    if (g.luaTick && now - g.luaTick < 500 && Angle(m, g.lua) > 3.0) return;
    Ship* slot = nullptr;
    Ship* oldest = &g.ships[0];
    for (Ship& s : g.ships) {
        if (s.obj == obj) {
            slot = &s;
            break;
        }
        if (s.seen < oldest->seen) oldest = &s;
    }
    if (!slot) {
        if (oldest == g.bound) g.bound = nullptr;
        slot = oldest;
        *slot = Ship{};
        slot->obj = obj;
        SafeReadPtr(obj, &slot->vptr);
    }
    slot->seen = now;
    slot->pos[0] = m[12], slot->pos[1] = m[13], slot->pos[2] = m[14];
    slot->posTick = now;
    Rebind(now);   // nearest qualifying object wins (the carrier's own origin, not a parked aircraft)
}

void* PlatCamForViewSetup(void* dcsCamera) {
    if (g.enabled && g_xActive) return &g_objX;
    if (!g.enabled || !g.haveTarget) return nullptr;
    if (!BuildPose()) return nullptr;
    if (dcsCamera) CheckAgainstDcs(dcsCamera);
    ++g.handed;
    return &g_obj;
}

bool PlatCamPose(double* m16) {
    if (!g.poseTick || GetTickCount64() - g.poseTick > 1000) return false;
    memcpy(m16, g.pose, sizeof(g.pose));
    return true;
}

bool PlatCamShipMatrix(double* m16) {
    if (!g.poseTick || GetTickCount64() - g.poseTick > 1000) return false;
    memcpy(m16, g.shipM, sizeof(g.shipM));
    return true;
}

void PlatCamOverride(const double* pose16, double fovRad, double aspect) {
    if (!pose16) {
        g_xActive = false;
        return;
    }
    memcpy(g_xPose, pose16, sizeof(g_xPose));
    g_xFov = fovRad;
    g_xAspect = aspect;
    g_xActive = true;
}

std::string PlatCamStats() {
    const ULONGLONG now = GetTickCount64();
    int ships = 0;
    for (const Ship& s : g.ships)
        if (Alive(&s, now)) ++ships;
    char b[512];
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "enabled=%d carrier=%s bound=%p scene_carriers=%d handed=%lld from_scene=%lld from_mission=%lld none=%lld "
                "rejected=%lld faults=%lld rebinds=%lld scene_vs_mission max %.2f m %.3f deg | vs DCS camera: checks=%lld max %.2f m %.3f deg"
                " other_ship=%lld",
                g.enabled ? 1 : 0, g.haveTarget ? g.type : "-", g.bound ? g.bound->obj : nullptr, ships, g.handed, g.fromObject,
                g.fromLua, g.none, g.axisMismatch, g.matrixFault, g.rebinds, g.maxPosVsLua, g.maxAxisVsLua, g.dcsChecks,
                g.dcsMaxPos, g.dcsMaxHdg, g.dcsOtherShip);
    g.handed = g.fromObject = g.fromLua = g.none = g.axisMismatch = g.matrixFault = g.rebinds = g.dcsChecks = g.dcsOtherShip = 0;
    g.maxPosVsLua = g.maxAxisVsLua = g.dcsMaxPos = g.dcsMaxHdg = 0;
    return b;
}

}  // namespace lsodiag
