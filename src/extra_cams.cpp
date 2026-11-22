#include "extra_cams.h"

#include "common.h"
#include "compat.h"
#include "plat_cam.h"
#include "ptz.h"
#include "share.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lsodiag {
namespace {

constexpr double kDeg = 3.14159265358979323846 / 180.0;

// Visualizer.dll 2.9.31.29434
constexpr uintptr_t kCallRva = 0x1912C1;      // call +0xBCFC0 (the PLAT's picture source)
constexpr uintptr_t kBuilderRva = 0xBCFC0;    // desc* builder(desc* out, unsigned index, const int* viewportId)
constexpr uintptr_t kAssignRva = 0x184F10;    // desc& assign(desc* dst, const desc* src)
constexpr uintptr_t kDtorRva = 0xBBD90;       // void destroy(desc*)
constexpr uintptr_t kSlotsRva = 0x2E27E8;     // desc* (vector begin)
constexpr uintptr_t kSlotCountRva = 0x2E27F0; // size_t
constexpr size_t kDescSize = 0x330;
constexpr size_t kSlotCount = 0x22;
// descriptor fields
constexpr size_t kOffValid = 0x00, kOffType = 0xC8, kOffOverlay = 0xE0;

using BuilderFn = void* (*)(void* out, unsigned index, const int* viewportId);
using AssignFn = void* (*)(void* dst, const void* src);
using DtorFn = void (*)(void* desc);

struct Cam {
    bool set = false;
    double pos[3] = {}, look[3] = {};
    double fov = 60 * kDeg, aspect = 4.0 / 3.0;
    int slot = 30, type = 0;
    // pan head (ptz.h): fixed rate (rad/s), ramp up / down time (s), how far off the aircraft may be before a move (rad)
    double panRate = 60 * kDeg, panRamp = 0.3, panDeadband = 8 * kDeg;
    int panOnly = 0;  // while tracking: +1 pans only toward larger pan angles, -1 smaller, 0 both ways
};

SRWLOCK g_lock = SRWLOCK_INIT;   // guards g_cams and g_track (Lua thread writes, the view setup thread reads)
Cam g_cams[kExtraCams];

// Tracking (ExtraCamsTrack): the aircraft the camera follows, in the carrier's axes, ~10 times a second from
// the hook. The view setup thread extrapolates it to the frame and moves the camera's aim smoothly.
struct Track {
    bool on = false;
    double p[3] = {}, v[3] = {};
    double modelT = -1;  // the sample's model time (velocity from successive samples)
    double recv = 0;     // video::Now-like clock (QPC s) when it arrived
};
Track g_track[kExtraCams];
// Aim state per camera (view setup thread only). The camera is a PTZ camera that only pans while it tracks: its
// tilt stays that of the rest view (the configured look-at point), its pan moves as ptz.h describes.
// Manual control (the viewer's joystick, shared memory): pan and tilt at rates up to kManPan / kManTilt (slower
// when zoomed in), zoom by up to kManZoom e-folds per second. Moving the stick takes the camera off automatic
// tracking until the viewer's reset; after the reset pan returns as when tracking ends, tilt and zoom ease back.
constexpr double kManPan = 45 * 3.14159265358979323846 / 180, kManTilt = 30 * 3.14159265358979323846 / 180, kManZoom = 0.9;
// Tracking a target that will outrun the pan head (ptz.h PanAhead): how far ahead its course is followed (s),
// and how far off, as a share of half the picture's width, it may get before a move must be under way. Tuned
// offline on traps past the island camera (48 deg, 60 deg/s, 0.3 s ramp): the aircraft stays within ~13 deg
// right / ~19 deg left of the aim (24 deg = the edge); before, it reached ~37 deg as it passed the island.
constexpr double kAheadS = 1.0, kOutrun = 0.6;
struct Aim {
    bool init = false;
    PanState pan;
    double rest[7] = {};  // the pos / look / fov the state belongs to (a new rest view is taken at once, off the joystick)
    double last = 0;
    double tilt = 0, fov = 0;  // rad; the rest view's unless under manual control or easing back
    bool manual = false, ptzInit = false;
    long resetSeen = 0;
} g_aim[kExtraCams];
// Where each camera was this frame (world), for the render origin fix (plat_origin.cpp): written by the view
// setup thread, read by the thread that builds the render graph.
volatile double g_worldPos[kExtraCams][3] = {};
volatile ULONGLONG g_worldTick[kExtraCams] = {};
volatile LONG g_slotOf[kExtraCams] = {-1, -1, -1, -1, -1, -1};
volatile LONG g_hangar[kExtraCams] = {};   // the camera shows a hangar deck position

double Seconds() {
    static const double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * inv;
}
volatile LONG g_mask = 0;
int g_typeShown[kExtraCams] = {-1, -1, -1, -1, -1, -1};  // picture type last handed to DCS (view setup thread)

unsigned char* g_vis = nullptr;
unsigned char* g_site = nullptr;   // the call instruction
unsigned char* g_stub = nullptr;
int32_t g_origRel = 0;
bool g_installed = false;
BuilderFn g_builder = nullptr;
AssignFn g_assign = nullptr;
DtorFn g_dtor = nullptr;

// counters (view setup thread writes, Lua thread reads and clears: approximate is fine)
volatile long long g_frames = 0, g_injected[kExtraCams] = {}, g_noPlat = 0, g_noShip = 0, g_busy = 0, g_invalid = 0, g_locked = 0;
volatile LONG g_faulted = 0;
bool g_loggedFirst[kExtraCams] = {};

// The cockpit picture sources (0-31) DCS itself filled, taken each frame in BuilderWrap after DCS's own
// builders ran and before the cameras add theirs (view setup thread): the last time each one was the
// cockpit's, and the last snapshot. Read by the viewport resize (mfd.cpp, render thread) so that a source the
// cockpit uses is never resized by ILARTS. 10 s of memory: the cockpit's sources are all invalid on the F10
// map (no cockpit views), and a TV page may come and go.
constexpr ULONGLONG kCockpitMemoryMs = 10000;
volatile ULONGLONG g_cockpitSeen[32] = {};
volatile ULONGLONG g_cockpitTick = 0;

unsigned CockpitSources() {
    const unsigned char* slots = *reinterpret_cast<unsigned char* const*>(g_vis + kSlotsRva);
    const size_t count = *reinterpret_cast<const size_t*>(g_vis + kSlotCountRva);
    if (!slots || count < kSlotCount) return 0;
    unsigned m = 0;
    for (unsigned i = 0; i < 32; ++i)
        if (slots[i * kDescSize + kOffValid]) m |= 1u << i;
    return m;
}

bool SehCockpitSources(unsigned* out) {
    __try {
        *out = CockpitSources();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The picture source's overlay callback: draws nothing. (DCS calls the PLAT's with no arguments it uses and
// the cockpit ones with (index, pointer); both are fine with this.)
void NoOverlay(unsigned, void*) {}

unsigned char* AllocNear(const void* anchor, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t base = reinterpret_cast<uintptr_t>(anchor) & ~(gran - 1);
    for (uintptr_t d = gran; d < 0x70000000; d += gran)
        for (uintptr_t a : {base - d, base + d}) {
            void* q = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (q) return static_cast<unsigned char*>(q);
        }
    return nullptr;
}

void Normalize(double* v) {
    const double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12) v[0] /= l, v[1] /= l, v[2] /= l;
}

// Camera pose (row-major: forward, up, right, position) from the carrier matrix M (rows x, y, z, position)
// and the camera's position / look-at point in the carrier's axes. The camera is fixed to the ship: it rolls
// and pitches with the deck.
void Pose(const double* M, const Cam& c, const double* look, double* o) {
    const double* X = M;
    const double* Y = M + 4;
    const double* Z = M + 8;
    double d[3] = {look[0] - c.pos[0], look[1] - c.pos[1], look[2] - c.pos[2]};
    Normalize(d);
    double f[3], u[3], r[3];
    for (int k = 0; k < 3; ++k) f[k] = d[0] * X[k] + d[1] * Y[k] + d[2] * Z[k];
    // right = forward x ship-up, up = right x forward (DCS axes: x forward, y up, z right; x cross y = z)
    r[0] = f[1] * Y[2] - f[2] * Y[1];
    r[1] = f[2] * Y[0] - f[0] * Y[2];
    r[2] = f[0] * Y[1] - f[1] * Y[0];
    Normalize(r);
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];
    for (int k = 0; k < 3; ++k) {
        o[k] = f[k];
        o[4 + k] = u[k];
        o[8 + k] = r[k];
        o[12 + k] = M[12 + k] + c.pos[0] * X[k] + c.pos[1] * Y[k] + c.pos[2] * Z[k];
    }
    o[3] = o[7] = o[11] = 0;
    o[15] = 1;
}

// After DCS has built the PLAT's picture source: the enabled cameras' sources. Plain C (runs under SEH).
void Inject(const void* platDesc, unsigned mask) {
    if (!*static_cast<const unsigned char*>(platDesc)) {  // no PLAT camera this frame: no carrier to sit on
        ++g_noPlat;
        return;
    }
    double M[16];
    if (!PlatCamShipMatrix(M)) {
        ++g_noShip;
        return;
    }
    unsigned char* slots = *reinterpret_cast<unsigned char**>(g_vis + kSlotsRva);
    const size_t count = *reinterpret_cast<size_t*>(g_vis + kSlotCountRva);
    if (!slots || count < kSlotCount) return;
    for (int i = 0; i < kExtraCams; ++i) {
        if (!(mask & (1u << i))) continue;
        Cam c;
        Track tr;
        if (!TryAcquireSRWLockShared(&g_lock)) {  // being reconfigured: this frame without it
            ++g_locked;
            continue;
        }
        c = g_cams[i];
        tr = g_track[i];
        ReleaseSRWLockShared(&g_lock);
        if (!c.set || c.slot < 0 || c.slot >= 32) continue;
        unsigned char* dst = slots + static_cast<size_t>(c.slot) * kDescSize;
        if (dst[kOffValid]) {  // the cockpit uses this picture source: leave it alone
            ++g_busy;
            continue;
        }
        // aim: pan toward the tracked aircraft (its position extrapolated to now), else toward the rest view;
        // the tilt is always the rest view's
        const double tnow = Seconds();
        const double age = tnow - tr.recv;
        const bool tracking = tr.on && age < 1.0;
        const double rx = c.look[0] - c.pos[0], ry = c.look[1] - c.pos[1], rz = c.look[2] - c.pos[2];
        const double restPan = atan2(rz, rx), restTilt = atan2(ry, sqrt(rx * rx + rz * rz));
        Aim& a = g_aim[i];
        double restNow[7];
        memcpy(restNow, c.pos, sizeof(c.pos));
        memcpy(restNow + 3, c.look, sizeof(c.look));
        restNow[6] = c.fov;
        if (!a.init || memcmp(restNow, a.rest, sizeof(restNow)) != 0) {
            memcpy(a.rest, restNow, sizeof(restNow));
            a.pan = PanState{};
            a.pan.pan = restPan;
            a.tilt = restTilt;
            a.fov = c.fov;
            a.manual = false;
            a.init = true;
            a.last = tnow;
        }
        const double dt = std::min(0.1, std::max(0.0, tnow - a.last));
        a.last = tnow;
        // the viewer's joystick
        float jp = 0, jt = 0, jz = 0;
        long resetSeq = 0;
        int pictureType = 0;  // the viewer's FLIR switch (0 = as configured)
        const bool joy = ShareReadPtz(i, &jp, &jt, &jz, &resetSeq, &pictureType);
        if (joy && !a.ptzInit) a.ptzInit = true, a.resetSeen = resetSeq;
        if (joy && resetSeq != a.resetSeen) {
            a.resetSeen = resetSeq;
            a.manual = false;
        }
        if (fabs(jp) > 0.01 || fabs(jt) > 0.01 || fabs(jz) > 0.01) {
            if (!a.manual) a.pan.rate = 0, a.pan.phase = 0;
            a.manual = true;
        }
        if (a.manual) {
            const double slow = std::min(1.0, a.fov / c.fov);  // zoomed in: finer
            a.pan.pan = PanWrap(a.pan.pan + jp * fabs(jp) * kManPan * slow * dt);
            a.pan.rate = 0, a.pan.phase = 0;
            a.tilt = std::min(45 * kDeg, std::max(-70 * kDeg, a.tilt + jt * fabs(jt) * kManTilt * slow * dt));
            a.fov = std::min(std::min(c.fov * 1.6, 100 * kDeg), std::max(2 * kDeg, a.fov * exp(-jz * kManZoom * dt)));
        } else {
            double targetPan = restPan;
            PanAhead ahead;
            if (tracking) {
                const double lead = std::min(age, 0.5);
                const double tx = tr.p[0] + tr.v[0] * lead - c.pos[0], tz = tr.p[2] + tr.v[2] * lead - c.pos[2];
                if (tx * tx + tz * tz > 1.0) {
                    targetPan = atan2(tz, tx);
                    // where it is heading over the next second (constant velocity), and how far off it may get
                    // before a move must already be under way: kOutrun of the half picture
                    ahead.n = static_cast<int>(kAheadS / ahead.step + 0.5);
                    for (int k = 0; k < ahead.n; ++k) {
                        const double t = (k + 1) * ahead.step;
                        ahead.bearing[k] = atan2(tz + tr.v[2] * t, tx + tr.v[0] * t);
                    }
                    ahead.outrun = kOutrun * c.fov / 2;
                }
            }
            PanStep(a.pan, targetPan, tracking ? c.panDeadband : 0.0, c.panRate, c.panRamp, dt, tracking ? c.panOnly : 0,
                    ahead.n > 0 ? &ahead : nullptr);
            const double k = std::min(1.0, dt / 0.4);  // tilt and zoom ease back to the rest view
            a.tilt += (restTilt - a.tilt) * k;
            a.fov += (c.fov - a.fov) * k;
        }
        ShareWritePtzState(i, static_cast<float>(PanWrap(a.pan.pan - restPan) / kDeg), static_cast<float>((a.tilt - restTilt) / kDeg),
                           static_cast<float>(c.fov / a.fov), a.manual);
        const double look[3] = {c.pos[0] + 100 * cos(a.tilt) * cos(a.pan.pan), c.pos[1] + 100 * sin(a.tilt),
                                c.pos[2] + 100 * cos(a.tilt) * sin(a.pan.pan)};
        double pose[16];
        Pose(M, c, look, pose);
        for (int k = 0; k < 3; ++k) g_worldPos[i][k] = pose[12 + k];
        g_worldTick[i] = GetTickCount64();
        g_slotOf[i] = c.slot;
        alignas(16) unsigned char buf[kDescSize + 0x20];
        const int viewportId = 100 + c.slot;
        PlatCamOverride(pose, a.fov, c.aspect);
        unsigned char* d = static_cast<unsigned char*>(g_builder(buf, static_cast<unsigned>(c.slot), &viewportId));
        PlatCamOverride(nullptr, 0, 0);
        if (d[kOffValid]) {
            *reinterpret_cast<void**>(d + kOffOverlay) = reinterpret_cast<void*>(&NoOverlay);
            const int type = pictureType > 0 ? pictureType : c.type;
            if (type > 0) *reinterpret_cast<int*>(d + kOffType) = type;
            if (type != g_typeShown[i]) {
                g_typeShown[i] = type;
                Log("EXTRACAM camera %d: picture type %d%s", i, type, type == 4 ? " (infrared, from the viewer)" : "");
            }
            g_assign(dst, d);
            ++g_injected[i];
        } else {
            ++g_invalid;
        }
        g_dtor(d);
    }
}

bool SehInject(const void* platDesc, unsigned mask) {
    __try {
        Inject(platDesc, mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PlatCamOverride(nullptr, 0, 0);
        return false;
    }
}

// Replaces DCS's call of the PLAT picture source builder.
void* BuilderWrap(void* out, unsigned index, const int* viewportId) {
    void* r = g_builder(out, index, viewportId);
    unsigned cockpit = 0;
    if (SehCockpitSources(&cockpit)) {
        const ULONGLONG now = GetTickCount64();
        for (int i = 0; i < 32; ++i)
            if (cockpit & (1u << i)) g_cockpitSeen[i] = now;
        g_cockpitTick = now;
    }
    const unsigned mask = static_cast<unsigned>(g_mask);
    if (!mask || !r) return r;
    ++g_frames;
    if (!SehInject(r, mask)) {
        InterlockedExchange(&g_mask, 0);
        InterlockedExchange(&g_faulted, 1);
        Log("EXTRACAM exception while building a camera's picture source: cameras switched off");
        return r;
    }
    for (int i = 0; i < kExtraCams; ++i)
        if (g_injected[i] && !g_loggedFirst[i]) {
            g_loggedFirst[i] = true;
            Log("EXTRACAM camera %d: first picture source handed to DCS (source %d, thread %lu)", i, g_cams[i].slot,
                GetCurrentThreadId());
        }
    return r;
}

bool WriteRel(int32_t rel) {
    DWORD old = 0;
    if (!VirtualProtect(g_site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(g_site + 1, &rel, 4);   // 4 bytes inside one cache line (+0x1912C2..C5): seen whole by the other thread
    DWORD tmp = 0;
    VirtualProtect(g_site, 5, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), g_site, 5);
    return true;
}

// Checks the code this module depends on; fills the pointers. "" or why not.
std::string Resolve() {
    HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
    if (!vis) return "Visualizer.dll not loaded";
    if (!CompatOk()) return CompatWhy();
    unsigned char* base = reinterpret_cast<unsigned char*>(vis);
    unsigned char call[5];
    if (!SafeRead(base + kCallRva, call, sizeof(call)) || call[0] != 0xE8) return "Visualizer+0x1912C1 is not the expected call";
    int32_t rel = 0;
    memcpy(&rel, call + 1, 4);
    const unsigned char* target = base + kCallRva + 5 + rel;
    if (target != base + kBuilderRva && !(g_installed && target == g_stub)) return "Visualizer+0x1912C1 does not call the PLAT view setup";
    // destructor: push rbx ; sub rsp,20h ; cmp qword [rcx+300h],0     assignment: starts with a mov / push
    unsigned char dt[6];
    const unsigned char dtExpect[6] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
    if (!SafeRead(base + kDtorRva, dt, sizeof(dt)) || memcmp(dt, dtExpect, sizeof(dt)) != 0) return "Visualizer+0xBBD90 is not the expected code";
    g_vis = base;
    g_site = base + kCallRva;
    g_builder = reinterpret_cast<BuilderFn>(base + kBuilderRva);
    g_assign = reinterpret_cast<AssignFn>(base + kAssignRva);
    g_dtor = reinterpret_cast<DtorFn>(base + kDtorRva);
    return "";
}

std::string Install() {
    if (g_installed) return "";
    std::string why = Resolve();
    if (!why.empty()) return why;
    if (!PlatCamEnabled()) return "the synthetic PLAT camera is not running";
    if (!g_stub) {
        unsigned char* mem = AllocNear(g_site, 64);
        if (!mem) return "no memory near Visualizer.dll";
        unsigned char code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};  // mov rax, BuilderWrap ; jmp rax
        const uint64_t fn = reinterpret_cast<uint64_t>(&BuilderWrap);
        memcpy(code + 2, &fn, 8);
        memcpy(mem, code, sizeof(code));
        DWORD old = 0;
        if (!VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &old)) return "VirtualProtect (stub) failed";
        FlushInstructionCache(GetCurrentProcess(), mem, 64);
        g_stub = mem;
    }
    memcpy(&g_origRel, g_site + 1, 4);
    const long long rel = reinterpret_cast<intptr_t>(g_stub) - reinterpret_cast<intptr_t>(g_site + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return "stub out of rel32 range";
    if (!WriteRel(static_cast<int32_t>(rel))) return "could not patch Visualizer+0x1912C1";
    g_installed = true;
    Log("EXTRACAM installed: Visualizer+0x1912C1 calls the stub at %p (DCS's own builder first, then the cameras)", g_stub);
    return "";
}

}  // namespace

std::string ExtraCamsAvailable() {
    if (g_faulted) return "switched off after an exception";
    if (g_installed) return "";
    std::string why = Resolve();
    if (!why.empty()) return why;
    if (!PlatCamEnabled()) return "the synthetic PLAT camera is not running";
    return "";
}

void ExtraCamsConfigure(int index, const double* pos, const double* look, double fovDeg, double aspect, int slot, int type) {
    if (index < 0 || index >= kExtraCams) return;
    Cam c;
    c.set = true;
    memcpy(c.pos, pos, sizeof(c.pos));
    memcpy(c.look, look, sizeof(c.look));
    c.fov = (fovDeg < 2 ? 2 : fovDeg > 150 ? 150 : fovDeg) * kDeg;
    c.aspect = aspect > 0.2 && aspect < 5 ? aspect : 4.0 / 3.0;
    c.slot = slot;
    c.type = type;
    AcquireSRWLockExclusive(&g_lock);
    c.panRate = g_cams[index].panRate;   // set separately (ExtraCamsPtz)
    c.panRamp = g_cams[index].panRamp;
    c.panDeadband = g_cams[index].panDeadband;
    c.panOnly = g_cams[index].panOnly;
    const bool changed = memcmp(&g_cams[index], &c, sizeof(c)) != 0;
    g_cams[index] = c;
    ReleaseSRWLockExclusive(&g_lock);
    if (changed)
        Log("EXTRACAM camera %d: at (%.1f, %.1f, %.1f) looking at (%.1f, %.1f, %.1f), %.0f deg, aspect %.3f, source %d, type %d",
            index, pos[0], pos[1], pos[2], look[0], look[1], look[2], fovDeg, c.aspect, slot, type);
}

std::string ExtraCamsEnable(unsigned mask) {
    mask &= (1u << kExtraCams) - 1;
    if (mask) {
        if (g_faulted) return "switched off after an exception";
        std::string why = Install();
        if (!why.empty()) return why;
    }
    if (static_cast<unsigned>(g_mask) != mask) Log("EXTRACAM cameras rendering: mask %u -> %u", static_cast<unsigned>(g_mask), mask);
    InterlockedExchange(&g_mask, static_cast<LONG>(mask));
    return "";
}

unsigned ExtraCamsMask() { return static_cast<unsigned>(g_mask); }

void ExtraCamsPtz(int index, double rateDeg, double rampSeconds, double deadbandDeg, int only) {
    if (index < 0 || index >= kExtraCams) return;
    const double rate = (rateDeg < 1 ? 1 : rateDeg > 360 ? 360 : rateDeg) * kDeg;
    const double ramp = rampSeconds < 0.02 ? 0.02 : rampSeconds > 5 ? 5 : rampSeconds;
    const double dead = (deadbandDeg < 0 ? 0 : deadbandDeg > 60 ? 60 : deadbandDeg) * kDeg;
    AcquireSRWLockExclusive(&g_lock);
    Cam& c = g_cams[index];
    only = only > 0 ? 1 : only < 0 ? -1 : 0;
    const bool changed = c.panRate != rate || c.panRamp != ramp || c.panDeadband != dead || c.panOnly != only;
    c.panRate = rate, c.panRamp = ramp, c.panDeadband = dead, c.panOnly = only;
    ReleaseSRWLockExclusive(&g_lock);
    if (changed)
        Log("EXTRACAM camera %d pan: %.0f deg/s, ramp %.2f s, moves when the aircraft is %.0f deg off, %s", index, rateDeg, ramp, deadbandDeg,
            only > 0 ? "only toward larger angles" : only < 0 ? "only toward smaller angles" : "both ways");
}

bool ExtraCamsNear(const double* world, double metres) {
    const unsigned mask = static_cast<unsigned>(g_mask);
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < kExtraCams; ++i) {
        if (!(mask & (1u << i)) || now - g_worldTick[i] > 2000) continue;
        const double dx = world[0] - g_worldPos[i][0], dy = world[1] - g_worldPos[i][1], dz = world[2] - g_worldPos[i][2];
        if (dx * dx + dy * dy + dz * dz < metres * metres) return true;
    }
    return false;
}

void ExtraCamsSetHangar(int index, bool hangar) {
    if (index < 0 || index >= kExtraCams) return;
    if (InterlockedExchange(&g_hangar[index], hangar ? 1 : 0) != (hangar ? 1 : 0))
        Log("EXTRACAM camera %d: %s", index, hangar ? "a hangar deck position (no rain or snow)" : "not a hangar position");
}

bool ExtraCamsHangarNear(const double* world, double metres) {
    const unsigned mask = static_cast<unsigned>(g_mask);
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < kExtraCams; ++i) {
        if (!g_hangar[i] || !(mask & (1u << i)) || now - g_worldTick[i] > 2000) continue;
        const double dx = world[0] - g_worldPos[i][0], dy = world[1] - g_worldPos[i][1], dz = world[2] - g_worldPos[i][2];
        if (dx * dx + dy * dy + dz * dz < metres * metres) return true;
    }
    return false;
}

bool ExtraCamsTargetName(const char* name) {
    if (!name || strncmp(name, "mfd", 3) != 0 || !name[3]) return false;
    char* end = nullptr;
    const long slot = strtol(name + 3, &end, 10);
    if (!end || *end) return false;
    const unsigned mask = static_cast<unsigned>(g_mask);
    for (int i = 0; i < kExtraCams; ++i)
        if ((mask & (1u << i)) && g_slotOf[i] == slot) return true;
    return false;
}

bool ExtraCamsCockpitSource(int source) {
    if (source < 0 || source >= 32) return false;
    const ULONGLONG now = GetTickCount64();
    if (now - g_cockpitSeen[source] < kCockpitMemoryMs) return true;
    if (now - g_cockpitTick < 2000) return false;
    // The redirect is not running (no camera since the mission started): the source's valid byte is DCS's own.
    HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
    if (!vis || !CompatOk()) return false;
    const unsigned char* base = reinterpret_cast<const unsigned char*>(vis);
    const unsigned char* slots = nullptr;
    size_t count = 0;
    unsigned char valid = 0;
    return SafeRead(base + kSlotsRva, &slots, sizeof(slots)) && SafeRead(base + kSlotCountRva, &count, sizeof(count)) && slots &&
           count >= kSlotCount && SafeRead(slots + static_cast<size_t>(source) * kDescSize + kOffValid, &valid, 1) && valid;
}

void ExtraCamsTrack(int index, const double* p, double modelTime) {
    if (index < 0 || index >= kExtraCams) return;
    AcquireSRWLockExclusive(&g_lock);
    Track& t = g_track[index];
    const bool was = t.on;
    if (!p) {
        t.on = false;
        t.modelT = -1;
    } else {
        const double dt = modelTime - t.modelT;
        if (t.on && t.modelT >= 0 && dt > 0.02 && dt < 1.0) {
            for (int k = 0; k < 3; ++k) {
                const double v = (p[k] - t.p[k]) / dt;
                t.v[k] = 0.5 * t.v[k] + 0.5 * v;  // light smoothing of the 10 Hz samples
            }
        } else if (!t.on || dt >= 1.0) {
            t.v[0] = t.v[1] = t.v[2] = 0;
        }
        if (dt > 0.02 || !t.on) {
            memcpy(t.p, p, sizeof(t.p));
            t.modelT = modelTime;
            t.recv = Seconds();
        }
        t.on = true;
    }
    const bool now = t.on;
    ReleaseSRWLockExclusive(&g_lock);
    if (was != now) Log("EXTRACAM camera %d: %s", index, now ? "tracking an aircraft" : "back to its rest view");
}

void ExtraCamsRemove() {
    InterlockedExchange(&g_mask, 0);
    if (!g_installed) return;
    if (WriteRel(g_origRel)) {
        g_installed = false;
        Log("EXTRACAM removed");
    }
    for (bool& b : g_loggedFirst) b = false;
}

std::string ExtraCamsStats() {
    char b[320];
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "installed=%d mask=%u frames=%lld handed cam0=%lld cam1=%lld | no_plat=%lld no_ship=%lld source_busy=%lld "
                "invalid=%lld reconfiguring=%lld faulted=%ld",
                g_installed ? 1 : 0, static_cast<unsigned>(g_mask), g_frames, g_injected[0], g_injected[1], g_noPlat, g_noShip,
                g_busy, g_invalid, g_locked, g_faulted);
    g_frames = g_noPlat = g_noShip = g_busy = g_invalid = g_locked = 0;
    g_injected[0] = g_injected[1] = 0;
    return b;
}

}  // namespace lsodiag
