#include "lso_camera.h"
#include "plat_origin.h"
#include "plat_cam.h"

#include "common.h"
#include "compat.h"
#include "engine.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <intrin.h>
#include <vector>

namespace lsodiag {
namespace {

constexpr int kSlotGetLsoCamera = 0xF8 / 8;  // IView slot 31 (Visualizer 0xBCFC0 / 0xBC250)
constexpr size_t kScanBytes = 0x2000;         // how much of the globalView object to scan for the pointer
// Visualizer.dll: the return address of the slot-31 call in the PLAT view setup (+0xBCFC0), the frame
// time (double) and its step (double).
constexpr uintptr_t kPlatSetupRetRva = 0xBD015, kFrameTimeRva = 0x2E1778, kFrameStepRva = 0x2E1788;

using GetCamFn = void* (*)(void* self);
using GetDoubleFn = double (*)(void* self);
using GetMatrixFn = void* (*)(void* self, void* ret);  // returns osg::Matrixd by value (hidden pointer)

struct State {
    const void* lastCam = nullptr;      // last non-null camera
    const void* lastCamVtbl = nullptr;
    std::vector<size_t> fieldOffsets;   // offsets in the globalView object that held the camera pointer
    bool lastWasNull = true;
    int calls = 0;
} g_cam;

// ---- Keep-PLAT override ----------------------------------------------------------------------
// The globalView object's vptr is pointed at a private copy of its vtable in which slot 31 is
// replaced. The replacement calls DCS's own function; when that returns null (player not at the
// LSO station) it returns the LSO camera last returned this mission, provided that object still
// carries the same vptr - to callers inside Visualizer.dll only. DCS code and DCS's vtable are not
// modified; the original vptr is put back by LsoKeepPlatStop(). Of the unprotected modules only
// Visualizer calls slot 31, at the two PLAT render sites (0xBC250, 0xBCFC0); protected ones
// (Supercarrier +0x370C7, possibly DCS.exe or aircraft modules) always get DCS's own answer.
// Every other slot is DCS's own function unless verbose logging was on at the start (call counting,
// slots 12 / 13) or the Supercarrier override was asked for (slots 12 / 13).
constexpr size_t kVtblCopySlots = 512;  // generous: entries past the real end are never called

struct Keep {
    void* view = nullptr;                     // globalView object we swapped
    const void* const* origVtbl = nullptr;    // its original vptr
    GetCamFn origSlot31 = nullptr;
    const void* table[kVtblCopySlots + 1] = {};  // [0] = RTTI locator (vtbl[-1]), then the slots
    void* volatile cached = nullptr;          // last non-null camera returned by DCS
    const void* cachedVtbl = nullptr;
    volatile LONG enabled = 0;
    volatile LONG64 dcsNull = 0, substituted = 0, passed = 0;
} g_keep;

uintptr_t g_viewSetupRet = 0;          // Visualizer+0xBD015: return address of the PLAT view setup call
uintptr_t g_visLo = 0, g_visHi = 0;    // Visualizer.dll: the only callers that get the kept camera
volatile ULONGLONG g_stationTick = 0;  // last time DCS itself returned an LSO camera
bool AtStation() { return GetTickCount64() - g_stationTick < 500; }

// ---- Camera orientation follows the ship every frame ----
// Run 2026-11-11 00:26 (HDGTRACE): the carrier's heading (Unit:getPosition) advances every frame, the LSO
// camera's position too, but the camera's ORIENTATION only changes 3-5 times a second, in steps of up to
// 0.174 deg. The hull is drawn smoothly, so the deck twitches against the camera and the sky moves in
// steps. Right after one of its own steps the camera is consistent with the ship (offset repeatable to
// 0.0015 deg), so: rel = camera axes expressed in ship axes, taken at those steps; every frame the
// orientation handed to Visualizer's PLAT view setup (+0xBCFC0, the only place that reads the matrix, via
// vtable slot 1) is rel applied to the ship's current axes. Only that caller (return address +0xBD015)
// gets a wrapper object; everybody else gets DCS's camera.
struct Smooth {
    volatile unsigned char enabled = 0;
    bool haveShip = false, haveRel = false, haveLast = false;
    double ship[9] = {};      // rows: ship x, y, z axes in world (Unit:getPosition)
    ULONGLONG shipTick = 0;
    double rel[9] = {};       // rel[i][j] = dot(camera axis i, ship axis j)
    double lastCam[9] = {};   // camera rotation at the previous sample
    void* real = nullptr;
    const void* const* realVtbl = nullptr;
    const void* wrapVtbl[8] = {};
    struct {
        const void* vtbl;
    } obj{};
    uintptr_t visRet = 0;
    // What Visualizer's PLAT view setup saw at its latest call (render side), for the trace.
    LARGE_INTEGER renderQpc{};
    double renderTime = 0, renderDt = 0, renderPos[3] = {};
    long long corrected = 0, calibrated = 0, wrapped = 0, rejected = 0, stale = 0;
    double maxCorr = 0;
} g_smooth;

void CamRot(const double* pose, double* r) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = pose[i * 4 + j];
}

// Called with every per-frame sample of DCS's own camera pose (main thread, after the ship axes).
void SmoothObserve(const double* pose) {
    double r[9];
    CamRot(pose, r);
    bool changed = false;
    if (g_smooth.haveLast)
        for (int k = 0; k < 9; ++k)
            if (fabs(r[k] - g_smooth.lastCam[k]) > 1e-9) changed = true;
    const bool fresh = g_smooth.haveShip && GetTickCount64() - g_smooth.shipTick < 200;
    if (fresh && (changed || !g_smooth.haveRel)) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double d = 0;
                for (int k = 0; k < 3; ++k) d += r[i * 3 + k] * g_smooth.ship[j * 3 + k];
                g_smooth.rel[i * 3 + j] = d;
            }
        g_smooth.haveRel = true;
        ++g_smooth.calibrated;
    }
    memcpy(g_smooth.lastCam, r, sizeof(r));
    g_smooth.haveLast = true;
}

void* WrapMatrix(void*, void* ret) {
    void* p = reinterpret_cast<GetMatrixFn>(const_cast<void*>(g_smooth.realVtbl[1]))(g_smooth.real, ret);
    if (p && g_smooth.visRet) {
        const double* rm = static_cast<const double*>(p);
        const uintptr_t vis = g_smooth.visRet - kPlatSetupRetRva;
        QueryPerformanceCounter(&g_smooth.renderQpc);
        SafeRead(reinterpret_cast<const void*>(vis + kFrameTimeRva), &g_smooth.renderTime, sizeof(double));  // frame time
        SafeRead(reinterpret_cast<const void*>(vis + kFrameStepRva), &g_smooth.renderDt, sizeof(double));    // its step
        g_smooth.renderPos[0] = rm[12];
        g_smooth.renderPos[1] = rm[13];
        g_smooth.renderPos[2] = rm[14];
    }
    if (!p || !g_smooth.enabled || !g_smooth.haveRel) return p;
    if (GetTickCount64() - g_smooth.shipTick >= 200) {
        ++g_smooth.stale;
        return p;
    }
    double* m = static_cast<double*>(p);
    double n[9];
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) {
            double d = 0;
            for (int j = 0; j < 3; ++j) d += g_smooth.rel[i * 3 + j] * g_smooth.ship[j * 3 + k];
            n[i * 3 + k] = d;
        }
    double dot = n[0] * m[0] + n[1] * m[1] + n[2] * m[2];
    if (dot > 1) dot = 1;
    const double ang = acos(dot) * 57.29577951308232;
    if (!(ang < 0.5)) {  // never more than DCS's own step; anything larger means the data do not match
        ++g_smooth.rejected;
        return p;
    }
    if (ang > g_smooth.maxCorr) g_smooth.maxCorr = ang;
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) m[i * 4 + k] = n[i * 3 + k];
    ++g_smooth.corrected;
    return p;
}
double WrapDouble2(void*) { return reinterpret_cast<GetDoubleFn>(const_cast<void*>(g_smooth.realVtbl[2]))(g_smooth.real); }
double WrapDouble3(void*) { return reinterpret_cast<GetDoubleFn>(const_cast<void*>(g_smooth.realVtbl[3]))(g_smooth.real); }
using Fwd4Fn = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t);
template <int N>
uintptr_t WrapForward(void*, uintptr_t a, uintptr_t b, uintptr_t c) {
    return reinterpret_cast<Fwd4Fn>(const_cast<void*>(g_smooth.realVtbl[N]))(g_smooth.real, a, b, c);
}

// For Visualizer's PLAT view setup only: a stand-in whose matrix getter corrects the orientation.
void* SmoothWrap(void* cam, const void* returnAddress) {
    if (!g_smooth.enabled || !cam || reinterpret_cast<uintptr_t>(returnAddress) != g_smooth.visRet) return cam;
    const void* const* v = nullptr;
    if (!SafeRead(cam, &v, sizeof(v)) || !v) return cam;
    g_smooth.real = cam;
    g_smooth.realVtbl = v;
    g_smooth.obj.vtbl = &g_smooth.wrapVtbl[0];
    ++g_smooth.wrapped;
    return &g_smooth.obj;
}

void* HookGetLsoCamera(void* self) {
    const void* ra = _ReturnAddress();
    void* cam = g_keep.origSlot31(self);
    if (cam) {
        g_stationTick = GetTickCount64();
        InterlockedIncrement64(&g_keep.passed);
        if (cam != g_keep.cached) {
            const void* v = nullptr;
            SafeReadPtr(cam, &v);
            g_keep.cachedVtbl = v;
            g_keep.cached = cam;
        }
    }
    // Visualizer's PLAT view setup: the synthetic camera (plat_cam.h) when it is running. Every other
    // caller (the overlay callback +0xBC250, Supercarrier) keeps DCS's own answer.
    if (g_viewSetupRet && reinterpret_cast<uintptr_t>(ra) == g_viewSetupRet) {
        if (void* s = PlatCamForViewSetup(cam)) return s;
    }
    if (cam) return SmoothWrap(cam, ra);
    InterlockedIncrement64(&g_keep.dcsNull);
    void* c = g_keep.cached;
    if (!g_keep.enabled || !c) return nullptr;
    // The kept camera is a DCS object DCS itself no longer hands out here; give it only to Visualizer's PLAT
    // sites (view setup, overlay callback), whose use of it is known. Protected callers - Supercarrier,
    // DCS.exe, aircraft modules - get DCS's own null.
    const uintptr_t r = reinterpret_cast<uintptr_t>(ra);
    if (r < g_visLo || r >= g_visHi) return nullptr;
    const void* v = nullptr;
    if (!SafeReadPtr(c, &v) || v != g_keep.cachedVtbl) {  // object gone or reused: stop substituting
        g_keep.cached = nullptr;
        return nullptr;
    }
    InterlockedIncrement64(&g_keep.substituted);
    return SmoothWrap(c, ra);
}

// ---- Supercarrier view-state answers (slots 12 / 13) ----
// Supercarrier (edSupercarrier.dll+0x215A4 / +0x215B9) asks globalView slot 12 every frame and, only
// when that says yes, slot 13 (Visualizer compares slot 13's result with 0x13). While the player is at
// the LSO station we record what DCS answers Supercarrier; while the PLAT camera is being substituted
// (player elsewhere) and the override is on, Supercarrier - and only Supercarrier, by return address -
// gets the recorded station answers. Everyone else always gets DCS's real answers.
using Slot4Fn = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t);
constexpr int kAnswerSlots[2] = {12, 13};
struct Answer {
    Slot4Fn orig = nullptr;
    volatile uintptr_t stationValue = 0;
    volatile LONG haveStation = 0;
    struct Seen { uintptr_t v; LONG64 n; } seen[2][8] = {};  // [0] station, [1] away (Supercarrier callers)
    volatile LONG64 overridden = 0;
} g_ans[2];
volatile uintptr_t g_lastMode12 = ~uintptr_t(0), g_lastMode13 = ~uintptr_t(0);  // DCS's latest real answers
volatile LONG g_scOverride = 0;
uintptr_t g_scLo = 0, g_scHi = 0;

void NoteValue(Answer& a, int mode, uintptr_t v) {
    for (auto& s : a.seen[mode]) {
        if (s.n && s.v == v) { ++s.n; return; }
        if (!s.n) { s.v = v; s.n = 1; return; }
    }
}

template <int I>
uintptr_t AnswerHook(void* self, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    Answer& a = g_ans[I];
    auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    bool sc = ret >= g_scLo && ret < g_scHi;
    if (sc && !AtStation() && g_scOverride && a.haveStation && g_keep.enabled && g_keep.cached) {
        InterlockedIncrement64(&a.overridden);
        return a.stationValue;
    }
    uintptr_t v = a.orig(self, a1, a2, a3);
    (I == 0 ? g_lastMode12 : g_lastMode13) = v;
    if (sc) {
        bool st = AtStation();
        NoteValue(a, st ? 0 : 1, v);
        // learn only "yes" answers: right after leaving, DCS already answers 0 within the AtStation()
        // grace window (observed 2026-11-08: slot 12 = 0x7 / slot 13 = 0x6 at the station, slot 12 = 0 away)
        if (st && v != 0) {
            a.stationValue = v;
            a.haveStation = 1;
        }
    }
    return v;
}

// Puts the slot 12 / 13 hooks into the private table (the table may already be the object's vtable: one
// aligned pointer store per slot, readers see DCS's function or the hook). Only for entries that are DCS.exe
// code; "" or why not.
bool g_ansInstalled = false;
std::string InstallAnswerHooks() {
    if (g_ansInstalled) return "";
    const void* hooks[2] = {reinterpret_cast<const void*>(&AnswerHook<0>), reinterpret_cast<const void*>(&AnswerHook<1>)};
    const void* orig[2] = {};
    for (int i = 0; i < 2; ++i) {
        orig[i] = g_keep.table[1 + kAnswerSlots[i]];
        if (DescribeAddress(orig[i]).rfind("DCS.exe+", 0) != 0) return "slot " + std::to_string(kAnswerSlots[i]) + " not in DCS.exe";
    }
    for (int i = 0; i < 2; ++i) {
        g_ans[i] = Answer{};
        g_ans[i].orig = reinterpret_cast<Slot4Fn>(const_cast<void*>(orig[i]));
    }
    for (int i = 0; i < 2; ++i)
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(const_cast<void**>(&g_keep.table[1 + kAnswerSlots[i]])),
                                   const_cast<void*>(hooks[i]));
    g_ansInstalled = true;
    return "";
}

// ---- call accounting on the private vtable (observation only) ----
// Each of the first kThunkSlots entries that points into DCS.exe (or at our slot-31 hook) is routed
// through a small generated thunk that counts the call, notes the return address, separately counts
// calls whose return address lies in edSupercarrier.dll, then jumps to the original target with every
// argument register and the stack untouched (only the volatile scratch registers rax/r10/r11 are used).
constexpr int kThunkSlots = 128;
struct SlotRec {
    volatile LONG64 count, lastCaller, scCount, scLastCaller;
};
SlotRec g_slotRec[kThunkSlots];
LONG64 g_slotPrev[kThunkSlots], g_slotScPrev[kThunkSlots];
unsigned char* g_thunks = nullptr;
constexpr int kThunkBytes = 160;
// Per slot, a direct-mapped table of return addresses (index = (address >> 2) & 255) with call counts
// since the last report. A collision restarts the entry's count; with a handful of call sites per slot
// that is rare. Written by the thunks without locking (diagnostic counts only).
constexpr int kCallerEntries = 256;
struct CallerEntry {
    volatile LONG64 addr, count;
};
CallerEntry g_callers[kThunkSlots][kCallerEntries];

bool ModuleRange(const wchar_t* name, uintptr_t* lo, uintptr_t* hi) {
    HMODULE m = GetModuleHandleW(name);
    if (!m) return false;
    auto base = reinterpret_cast<const unsigned char*>(m);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    *lo = reinterpret_cast<uintptr_t>(base);
    *hi = *lo + nt->OptionalHeader.SizeOfImage;
    return true;
}

void Put(unsigned char*& p, std::initializer_list<unsigned char> b) {
    for (unsigned char c : b) *p++ = c;
}
void Put64(unsigned char*& p, uint64_t v) {
    memcpy(p, &v, 8);
    p += 8;
}

void InstrumentTable() {
    if (!g_thunks) {
        if (!ModuleRange(L"edSupercarrier.dll", &g_scLo, &g_scHi)) g_scLo = g_scHi = 0;
        g_thunks = static_cast<unsigned char*>(VirtualAlloc(nullptr, kThunkSlots * kThunkBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!g_thunks) return;
    }
    DWORD old = 0;
    VirtualProtect(g_thunks, kThunkSlots * kThunkBytes, PAGE_READWRITE, &old);
    int n = 0;
    for (int s = 0; s < kThunkSlots; ++s) {
        const void* target = g_keep.table[1 + s];
        if (target != reinterpret_cast<const void*>(&HookGetLsoCamera) && target != reinterpret_cast<const void*>(&AnswerHook<0>) &&
            target != reinterpret_cast<const void*>(&AnswerHook<1>) && DescribeAddress(target).rfind("DCS.exe+", 0) != 0)
            continue;
        unsigned char* p = g_thunks + s * kThunkBytes;
        unsigned char* start = p;
        Put(p, {0x48, 0x8B, 0x04, 0x24});                   // mov rax,[rsp]
        Put(p, {0x49, 0xBB}); Put64(p, reinterpret_cast<uint64_t>(&g_slotRec[s]));  // mov r11,&rec
        Put(p, {0xF0, 0x49, 0xFF, 0x03});                   // lock inc qword [r11]
        Put(p, {0x49, 0x89, 0x43, 0x08});                   // mov [r11+8],rax
        // caller table: entry = &g_callers[s][(rax >> 2) & 255]
        Put(p, {0x49, 0xBA}); Put64(p, reinterpret_cast<uint64_t>(&g_callers[s][0]));  // mov r10,table
        Put(p, {0x49, 0x89, 0xC3});                         // mov r11,rax
        Put(p, {0x49, 0xC1, 0xEB, 0x02});                   // shr r11,2
        Put(p, {0x41, 0x81, 0xE3, 0xFF, 0x00, 0x00, 0x00}); // and r11d,255
        Put(p, {0x49, 0xC1, 0xE3, 0x04});                   // shl r11,4
        Put(p, {0x4D, 0x01, 0xDA});                         // add r10,r11
        Put(p, {0x49, 0x39, 0x02});                         // cmp [r10],rax
        Put(p, {0x74, 0x0B});                               // je count
        Put(p, {0x49, 0x89, 0x02});                         // mov [r10],rax
        Put(p, {0x49, 0xC7, 0x42, 0x08, 0x00, 0x00, 0x00, 0x00});  // mov qword [r10+8],0
        Put(p, {0x49, 0xFF, 0x42, 0x08});                   // count: inc qword [r10+8]
        Put(p, {0x49, 0xBB}); Put64(p, reinterpret_cast<uint64_t>(&g_slotRec[s]));  // mov r11,&rec (again)
        Put(p, {0x49, 0xBA}); Put64(p, g_scLo);             // mov r10,scLo
        Put(p, {0x4C, 0x39, 0xD0});                         // cmp rax,r10
        Put(p, {0x72, 0x18});                               // jb done
        Put(p, {0x49, 0xBA}); Put64(p, g_scHi);             // mov r10,scHi
        Put(p, {0x4C, 0x39, 0xD0});                         // cmp rax,r10
        Put(p, {0x73, 0x09});                               // jae done
        Put(p, {0xF0, 0x49, 0xFF, 0x43, 0x10});             // lock inc qword [r11+0x10]
        Put(p, {0x49, 0x89, 0x43, 0x18});                   // mov [r11+0x18],rax
        Put(p, {0x48, 0xB8}); Put64(p, reinterpret_cast<uint64_t>(target));  // done: mov rax,target
        Put(p, {0xFF, 0xE0});                               // jmp rax
        g_keep.table[1 + s] = start;
        ++n;
    }
    VirtualProtect(g_thunks, kThunkSlots * kThunkBytes, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), g_thunks, kThunkSlots * kThunkBytes);
    Log("VIEWCALLS instrumented %d globalView slots; edSupercarrier.dll range %p..%p", n,
        reinterpret_cast<void*>(g_scLo), reinterpret_cast<void*>(g_scHi));
}

bool ResolveGlobalView(void** view, const void* const** vtbl, std::string* why) {
    HMODULE exe = GetModuleHandleW(nullptr);
    (void)exe;
    if (!CompatOk()) return *why = CompatWhy(), false;
    HMODULE inter = GetModuleHandleW(L"Inter.dll");
    if (!inter) return *why = "Inter.dll not loaded", false;
    auto sym = reinterpret_cast<const void*>(GetProcAddress(inter, "?globalView@@3PEAVIView@@EA"));
    const void* v = nullptr;
    if (!sym || !SafeReadPtr(sym, &v) || !v) return *why = "globalView null", false;
    if (!SafeRead(v, vtbl, sizeof(*vtbl)) || !*vtbl) return *why = "globalView vtable unreadable", false;
    *view = const_cast<void*>(v);
    return true;
}

void* SehGetCam(GetCamFn fn, void* self, DWORD* code) {
    __try {
        return fn(self);
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SehDouble(GetDoubleFn fn, void* self, double* out, DWORD* code) {
    __try {
        *out = fn(self);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehMatrix(GetMatrixFn fn, void* self, double* buf, void** ret, DWORD* code) {
    __try {
        *ret = fn(self, buf);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

// Per-frame trace of the PLAT camera pose (one sample per LsoCameraProbe call = per simulation frame):
// does the heading advance every frame (smooth), only every few frames (update rate), or in coarse
// steps (precision)? Summarised and reset by LsoPoseTraceStats().
struct PoseTrace {
    bool have = false;
    LARGE_INTEGER last{};
    double yaw = 0, x = 0, z = 0;
    long long n = 0, yawZero = 0, posZero = 0;
    double sumDt = 0, maxDt = 0, sumYaw = 0, sumAbsYaw = 0, minYaw = 1e30, maxYaw = 0, sumPos = 0, minPos = 1e30, maxPos = 0;
    double sumRate = 0, sumRate2 = 0;  // yaw rate per frame (deg/s), for its spread
} g_trace;

double g_poseLastY = 0;

void PoseTraceNote(const double* pose) {
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    const double yaw = atan2(pose[2], pose[0]) * 57.29577951308232;
    if (g_trace.have) {
        const double dt = double(now.QuadPart - g_trace.last.QuadPart) / double(f.QuadPart);
        if (dt < 0.0015) return;  // second probe within the same frame (the hook's once-a-second summary)
        double dy = yaw - g_trace.yaw;
        if (dy > 180) dy -= 360;
        if (dy < -180) dy += 360;
        const double dp = hypot(pose[12] - g_trace.x, pose[14] - g_trace.z);
        ++g_trace.n;
        g_trace.sumDt += dt;
        if (dt > g_trace.maxDt) g_trace.maxDt = dt;
        g_trace.sumYaw += dy;
        const double ay = fabs(dy);
        g_trace.sumAbsYaw += ay;
        if (ay < 1e-9) {
            ++g_trace.yawZero;
        } else {
            if (ay < g_trace.minYaw) g_trace.minYaw = ay;
            if (ay > g_trace.maxYaw) g_trace.maxYaw = ay;
        }
        if (dp < 1e-9) {
            ++g_trace.posZero;
        } else {
            if (dp < g_trace.minPos) g_trace.minPos = dp;
            if (dp > g_trace.maxPos) g_trace.maxPos = dp;
        }
        g_trace.sumPos += dp;
        if (dt > 1e-6) {
            const double r = dy / dt;
            g_trace.sumRate += r;
            g_trace.sumRate2 += r * r;
        }
    }
    g_trace.have = true;
    g_trace.last = now;
    g_trace.yaw = yaw;
    g_trace.x = pose[12];
    g_trace.z = pose[14];
    g_poseLastY = pose[13];
    SmoothObserve(pose);
}

// Position of the PLAT camera at the latest per-frame sample (hook side) and how old that sample is.
bool LsoLastCameraPosition(double* out, double* ageSeconds) {
    if (!g_trace.have) return false;
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    out[0] = g_trace.x;
    out[1] = g_poseLastY;
    out[2] = g_trace.z;
    if (ageSeconds) *ageSeconds = double(now.QuadPart - g_trace.last.QuadPart) / double(f.QuadPart);
    return true;
}

// Ship axes for this frame, "xx xy xz yx yy yz zx zy zz" (Unit:getPosition x, y, z vectors).
bool LsoSmoothShip(const char* text) {
    double v[9];
    if (!text || sscanf_s(text, "%lf %lf %lf %lf %lf %lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                          &v[8]) != 9)
        return false;
    memcpy(g_smooth.ship, v, sizeof(v));
    g_smooth.haveShip = true;
    g_smooth.shipTick = GetTickCount64();
    return true;
}

std::string LsoSmoothEnable(bool on) {
    if (!on) {
        g_smooth.enabled = 0;
        return "";
    }
    HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
    if (!vis) return "Visualizer.dll not loaded";
    if (!CompatOk()) return CompatWhy();
    g_smooth.visRet = reinterpret_cast<uintptr_t>(vis) + kPlatSetupRetRva;
    g_smooth.wrapVtbl[0] = reinterpret_cast<const void*>(&WrapForward<0>);
    g_smooth.wrapVtbl[1] = reinterpret_cast<const void*>(&WrapMatrix);
    g_smooth.wrapVtbl[2] = reinterpret_cast<const void*>(&WrapDouble2);
    g_smooth.wrapVtbl[3] = reinterpret_cast<const void*>(&WrapDouble3);
    g_smooth.wrapVtbl[4] = reinterpret_cast<const void*>(&WrapForward<4>);
    g_smooth.wrapVtbl[5] = reinterpret_cast<const void*>(&WrapForward<5>);
    g_smooth.wrapVtbl[6] = reinterpret_cast<const void*>(&WrapForward<6>);
    g_smooth.wrapVtbl[7] = reinterpret_cast<const void*>(&WrapForward<7>);
    g_smooth.enabled = 1;
    return "";
}

std::string LsoSmoothStats() {
    char b[256];
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "enabled=%d views=%lld corrected=%lld max_correction=%.4f deg calibrations=%lld rejected=%lld stale_ship=%lld",
                g_smooth.enabled ? 1 : 0, g_smooth.wrapped, g_smooth.corrected, g_smooth.maxCorr, g_smooth.calibrated,
                g_smooth.rejected, g_smooth.stale);
    g_smooth.wrapped = g_smooth.corrected = g_smooth.calibrated = g_smooth.rejected = g_smooth.stale = 0;
    g_smooth.maxCorr = 0;
    return b;
}

// The latest sampled pose: "qpc_seconds yaw_deg x y z" (yaw = atan2(forward.z, forward.x)).
std::string LsoPoseLast() {
    if (!g_trace.have) return "none";
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    char b[560];
    const double* r = g_smooth.lastCam;  // DCS's own camera rotation (rows: forward, up, right)
    // After the mode: the latest PLAT view setup (render side): qpc, Visualizer frame time and step, camera position.
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "%.6f %.6f %.4f %.4f %.4f %.8f %.8f %.8f %.8f %.8f %.8f %.8f %.8f %.8f m%llX %.6f %.6f %.6f %.4f %.4f %.4f",
                double(g_trace.last.QuadPart) / double(f.QuadPart), g_trace.yaw, g_trace.x, g_poseLastY, g_trace.z, r[0], r[1],
                r[2], r[3], r[4], r[5], r[6], r[7], r[8], static_cast<unsigned long long>(g_lastMode12),
                double(g_smooth.renderQpc.QuadPart) / double(f.QuadPart), g_smooth.renderTime, g_smooth.renderDt,
                g_smooth.renderPos[0], g_smooth.renderPos[1], g_smooth.renderPos[2]);
    return b;
}

std::string LsoPoseTraceStats() {
    PoseTrace t = g_trace;
    const bool have = g_trace.have;
    const LARGE_INTEGER last = g_trace.last;
    const double yaw = g_trace.yaw, x = g_trace.x, z = g_trace.z;
    g_trace = PoseTrace{};
    g_trace.have = have;
    g_trace.last = last;
    g_trace.yaw = yaw;
    g_trace.x = x;
    g_trace.z = z;
    if (t.n < 2 || t.sumDt <= 0) return "no samples";
    const double meanRate = t.sumRate / double(t.n);
    const double var = t.sumRate2 / double(t.n) - meanRate * meanRate;
    char b[512];
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "frames=%lld fps=%.0f maxdt=%.1fms | heading: turned %.4f deg (%.4f deg/s), unchanged in %lld frames, "
                "step min=%.6f max=%.6f deg, per-frame rate spread=%.3f deg/s | position: moved %.3f m (%.2f m/s), "
                "unchanged in %lld frames, step min=%.4f max=%.4f m",
                t.n, double(t.n) / t.sumDt, t.maxDt * 1000.0, t.sumYaw, t.sumYaw / t.sumDt, t.yawZero,
                t.minYaw > 1e29 ? 0.0 : t.minYaw, t.maxYaw, var > 0 ? sqrt(var) : 0.0, t.sumPos, t.sumPos / t.sumDt, t.posZero,
                t.minPos > 1e29 ? 0.0 : t.minPos, t.maxPos);
    return b;
}

std::string LsoPoseLast();
std::string LsoCameraProbe() {
    if (EngineDisabled()) return "engine calls disabled";
    HMODULE exe = GetModuleHandleW(nullptr);
    (void)exe;
    if (!CompatOk()) return CompatWhy();
    HMODULE inter = GetModuleHandleW(L"Inter.dll");
    if (!inter) return "Inter.dll not loaded";
    auto sym = reinterpret_cast<const void*>(GetProcAddress(inter, "?globalView@@3PEAVIView@@EA"));
    const void* view = nullptr;
    const void* const* vtbl = nullptr;
    const void* fn = nullptr;
    if (!sym || !SafeReadPtr(sym, &view) || !view) return "globalView null";
    if (!SafeRead(view, &vtbl, sizeof(vtbl)) || !vtbl || !SafeReadPtr(&vtbl[kSlotGetLsoCamera], &fn) || !fn)
        return "globalView vtable unreadable";
    // With the override installed, observe DCS's own answer, not the substituted one.
    if (g_keep.view == view && vtbl == &g_keep.table[1]) fn = reinterpret_cast<const void*>(g_keep.origSlot31);

    ++g_cam.calls;
    if (g_cam.calls == 1) {
        Log("LSOCAM globalView=%p vtable=%s slot31=%s", view, DescribeAddress(vtbl).c_str(), DescribeAddress(fn).c_str());
    }
    DWORD code = 0;
    void* cam = SehGetCam(reinterpret_cast<GetCamFn>(const_cast<void*>(fn)), const_cast<void*>(view), &code);
    if (code) {
        DisableEngine("globalView->slot31", code);
        return "exception in slot31";
    }

    char line[512];
    if (!cam && PlatCamEnabled()) {
        double pose[16];
        if (PlatCamPose(pose)) {
            PlatOriginNoteCamera(pose[12], pose[13], pose[14]);
            PoseTraceNote(pose);
            g_cam.lastWasNull = true;
            _snprintf_s(line, _countof(line), _TRUNCATE,
                        "camera null (DCS), substituting synthetic camera pos (%.1f, %.1f, %.1f) | dcs-null=%lld passed=%lld",
                        pose[12], pose[13], pose[14], g_keep.dcsNull, g_keep.passed);
            return line;
        }
    }
    if (!cam && g_keep.enabled && g_keep.cached) {
        // DCS has no LSO camera now; report the substituted one's pose (does it still follow the ship?)
        void* c = g_keep.cached;
        const void* const* cv = nullptr;
        double m[32] = {};
        void* ret = nullptr;
        if (SafeRead(c, &cv, sizeof(cv)) && cv == g_keep.cachedVtbl &&
            SehMatrix(reinterpret_cast<GetMatrixFn>(const_cast<void*>(cv[1])), c, m, &ret, &code) && !code) {
            const double* mm = ret ? static_cast<const double*>(ret) : m;
            double pose[16] = {};
            SafeRead(mm, pose, sizeof(pose));
            PlatOriginNoteCamera(pose[12], pose[13], pose[14]);
            PoseTraceNote(pose);
            g_cam.lastWasNull = true;
            _snprintf_s(line, _countof(line), _TRUNCATE,
                        "camera null (DCS), substituting %p pos (%.1f, %.1f, %.1f) | dcs-null=%lld substituted=%lld passed=%lld",
                        c, pose[12], pose[13], pose[14], g_keep.dcsNull, g_keep.substituted, g_keep.passed);
            return line;
        }
        if (code) {
            DisableEngine("substituted camera pose", code);
            return "exception in substituted camera pose";
        }
    }
    if (!cam) {
        if (!g_cam.lastWasNull) {
            // Just went null: is the old camera object still alive, and what do the fields hold now?
            const void* oldVtbl = nullptr;
            bool readable = g_cam.lastCam && SafeReadPtr(g_cam.lastCam, &oldVtbl);
            Log("LSOCAM -> null. previous camera %p: %s (vtable now %s, was %s)", g_cam.lastCam,
                readable ? (oldVtbl == g_cam.lastCamVtbl ? "memory intact, same vtable" : "memory CHANGED (likely freed/reused)")
                         : "UNREADABLE (freed)",
                DescribeAddress(oldVtbl).c_str(), DescribeAddress(g_cam.lastCamVtbl).c_str());
            for (size_t off : g_cam.fieldOffsets) {
                const void* v = nullptr;
                SafeReadPtr(static_cast<const char*>(view) + off, &v);
                Log("LSOCAM    globalView+0x%zX now %p", off, v);
            }
        }
        g_cam.lastWasNull = true;
        _snprintf_s(line, _countof(line), _TRUNCATE, "camera null (last %p)", g_cam.lastCam);
        return line;
    }

    const void* const* cv = nullptr;
    if (!SafeRead(cam, &cv, sizeof(cv)) || !cv) return "camera vtable unreadable";
    bool changed = g_cam.lastWasNull || cam != g_cam.lastCam;
    double fov10 = 0, fov18 = 0;
    SehDouble(cv ? reinterpret_cast<GetDoubleFn>(const_cast<void*>(cv[2])) : nullptr, cam, &fov10, &code);
    SehDouble(cv ? reinterpret_cast<GetDoubleFn>(const_cast<void*>(cv[3])) : nullptr, cam, &fov18, &code);
    double m[32] = {};
    void* ret = nullptr;
    SehMatrix(cv ? reinterpret_cast<GetMatrixFn>(const_cast<void*>(cv[1])) : nullptr, cam, m, &ret, &code);
    if (code) {
        DisableEngine("LSO camera method", code);
        return "exception in camera method";
    }
    const double* mm = ret ? static_cast<const double*>(ret) : m;
    double pose[16] = {};
    SafeRead(mm, pose, sizeof(pose));
    PlatOriginNoteCamera(pose[12], pose[13], pose[14]);
    PoseTraceNote(pose);

    if (changed) {
        Log("LSOCAM camera %p vtable=%s (ret==buf: %s)", cam, DescribeAddress(cv).c_str(), ret == m ? "yes" : "no");
        for (int i = 0; i < 8 && cv; ++i) {
            const void* e = nullptr;
            SafeReadPtr(&cv[i], &e);
            Log("LSOCAM    vtable[%d] +0x%02X = %s", i, i * 8, DescribeAddress(e).c_str());
        }
        Log("LSOCAM    +0x10 -> %.6f   +0x18 -> %.6f", fov10, fov18);
        Log("LSOCAM    pose rows: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.1f %.1f %.1f %.3f]",
            pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6], pose[7], pose[8], pose[9], pose[10], pose[11],
            pose[12], pose[13], pose[14], pose[15]);
        // Which fields of the globalView object hold this pointer?
        std::vector<unsigned char> mem(kScanBytes);
        size_t readable = 0;
        for (size_t off = 0; off < kScanBytes; off += 0x100) {
            if (!SafeRead(static_cast<const char*>(view) + off, mem.data() + off, 0x100)) break;
            readable = off + 0x100;
        }
        g_cam.fieldOffsets.clear();
        for (size_t off = 0; off + 8 <= readable; off += 8) {
            const void* v = nullptr;
            memcpy(&v, mem.data() + off, 8);
            if (v == cam) g_cam.fieldOffsets.push_back(off);
        }
        std::string offs;
        for (size_t off : g_cam.fieldOffsets) {
            char b[24];
            _snprintf_s(b, _countof(b), _TRUNCATE, " +0x%zX", off);
            offs += b;
        }
        Log("LSOCAM    camera pointer found in globalView object at:%s (scanned 0x%zX bytes)",
            offs.empty() ? " none" : offs.c_str(), readable);
        g_cam.lastCam = cam;
        g_cam.lastCamVtbl = cv;
    }
    g_cam.lastWasNull = false;
    _snprintf_s(line, _countof(line), _TRUNCATE, "camera %p pos (%.1f, %.1f, %.1f) +0x10=%.4f +0x18=%.4f", cam, pose[12],
                pose[13], pose[14], fov10, fov18);
    return line;
}

std::string LsoKeepPlatStart() {
    if (EngineDisabled()) return "engine calls disabled";
    void* view = nullptr;
    const void* const* vtbl = nullptr;
    std::string why;
    if (!ResolveGlobalView(&view, &vtbl, &why)) return why;
    if (g_keep.view == view && vtbl == &g_keep.table[1]) {
        g_keep.enabled = 1;
        return "already installed";
    }
    if (g_keep.view) return "installed on a different globalView object; stop first";
    // vtable must live in DCS.exe (the verified IView implementation seen by the probe)
    if (DescribeAddress(vtbl).rfind("DCS.exe+", 0) != 0) return "globalView vtable not in DCS.exe: " + DescribeAddress(vtbl);
    if (!SafeRead(vtbl - 1, g_keep.table, sizeof(g_keep.table))) return "globalView vtable copy unreadable";
    const void* orig = g_keep.table[1 + kSlotGetLsoCamera];
    if (DescribeAddress(orig).rfind("DCS.exe+", 0) != 0) return "slot 31 not in DCS.exe";
    g_keep.origSlot31 = reinterpret_cast<GetCamFn>(const_cast<void*>(orig));
    g_keep.table[1 + kSlotGetLsoCamera] = reinterpret_cast<const void*>(&HookGetLsoCamera);
    // Observation only, so only with verbose logging: the Supercarrier view-state answers (see AnswerHook) and
    // the call counting through generated thunks (VIEWCALLS). Without them every other slot is DCS's own.
    g_ansInstalled = false;
    if (Verbose()) {
        const std::string ansWhy = InstallAnswerHooks();
        if (!ansWhy.empty()) return ansWhy;
        InstrumentTable();
    }
    g_keep.origVtbl = vtbl;
    g_keep.cached = nullptr;
    g_keep.cachedVtbl = nullptr;
    g_keep.dcsNull = g_keep.substituted = g_keep.passed = 0;
    g_keep.view = view;
    g_keep.enabled = 1;
    if (HMODULE vis = GetModuleHandleW(L"Visualizer.dll"); vis && CompatOk()) {
        g_viewSetupRet = reinterpret_cast<uintptr_t>(vis) + kPlatSetupRetRva;
        if (!ModuleRange(L"Visualizer.dll", &g_visLo, &g_visHi)) g_visLo = g_visHi = 0;
    } else {
        g_visLo = g_visHi = 0;  // nobody gets the kept camera
    }
    // single aligned pointer store; readers see either the old or the new table, both valid
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(view), const_cast<void**>(&g_keep.table[1]));
    Log("KEEPPLAT installed: globalView %p vptr %s -> private copy %p (slot31 %s -> hook, kept camera to Visualizer only%s)", view,
        DescribeAddress(vtbl).c_str(), &g_keep.table[1], DescribeAddress(orig).c_str(),
        g_ansInstalled ? "; slots 12/13 hooked, calls counted (verbose)" : "");
    return "";
}

std::string LsoViewCallStats() {
    if (!g_thunks) return "not instrumented";
    std::string all, sc;
    for (int s = 0; s < kThunkSlots; ++s) {
        LONG64 c = g_slotRec[s].count, d = c - g_slotPrev[s];
        LONG64 k = g_slotRec[s].scCount, e = k - g_slotScPrev[s];
        g_slotPrev[s] = c;
        g_slotScPrev[s] = k;
        char b[160];
        if (d) {
            _snprintf_s(b, _countof(b), _TRUNCATE, " %d:%lld", s, d);
            all += b;
        }
        if (e) {
            _snprintf_s(b, _countof(b), _TRUNCATE, " slot%d(+0x%X)=%lld from %s", s, s * 8, e,
                        DescribeAddress(reinterpret_cast<const void*>(g_slotRec[s].scLastCaller)).c_str());
            sc += b;
        }
    }
    std::string ans;
    for (int i = 0; i < 2; ++i) {
        char b[96];
        _snprintf_s(b, _countof(b), _TRUNCATE, " | slot%d to SC: overridden=%lld station[", kAnswerSlots[i], g_ans[i].overridden);
        ans += b;
        for (int m = 0; m < 2; ++m) {
            if (m) ans += "] away[";
            for (auto& v : g_ans[i].seen[m]) {
                if (!v.n) break;
                _snprintf_s(b, _countof(b), _TRUNCATE, " 0x%llX x%lld", static_cast<unsigned long long>(v.v), v.n);
                ans += b;
            }
        }
        ans += "]";
    }
    // Who called each busy slot since the last report (one log line per slot, most frequent first).
    for (int s = 0; s < kThunkSlots; ++s) {
        struct Top {
            LONG64 addr, n;
        } top[12] = {};
        LONG64 total = 0;
        int distinct = 0;
        for (auto& e : g_callers[s]) {
            LONG64 n = e.count, a = e.addr;
            e.count = 0;
            if (!n || !a) continue;
            total += n;
            ++distinct;
            for (int k = 0; k < 12; ++k) {
                if (n > top[k].n) {
                    for (int j = 11; j > k; --j) top[j] = top[j - 1];
                    top[k] = Top{a, n};
                    break;
                }
            }
        }
        if (total < 200) continue;  // roughly: less than once every few frames
        std::string line;
        for (auto& t : top) {
            if (!t.n) break;
            char b[160];
            _snprintf_s(b, _countof(b), _TRUNCATE, " %s x%lld", DescribeAddress(reinterpret_cast<const void*>(t.addr)).c_str(), t.n);
            line += b;
        }
        Log("VIEWCALLERS mode12=0x%llX mode13=0x%llX slot %d (+0x%X) total=%lld sites=%d:%s",
            static_cast<unsigned long long>(g_lastMode12), static_cast<unsigned long long>(g_lastMode13), s, s * 8, total,
            distinct, line.c_str());
    }
    return "override=" + std::to_string(g_scOverride) + ans + " | supercarrier:" + (sc.empty() ? std::string(" none") : sc) + " | all slots (count since last):" + all;
}

void LsoSupercarrierOverride(bool on) {
    std::string why;
    if (on && g_keep.view && !g_ansInstalled) why = InstallAnswerHooks();  // the slot 12/13 hooks, on first use
    InterlockedExchange(&g_scOverride, on ? 1 : 0);
    Log("SCVIEW Supercarrier view-state override %s%s%s", on ? "ON" : "OFF", why.empty() ? "" : ": hooks not installed, ",
        why.c_str());
}

std::string LsoKeepPlatStop() {
    if (!g_keep.view) return "not installed";
    g_keep.enabled = 0;
    const void* cur = nullptr;
    SafeReadPtr(g_keep.view, &cur);
    char line[256];
    if (cur == &g_keep.table[1]) {
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(g_keep.view), const_cast<void**>(g_keep.origVtbl));
        _snprintf_s(line, _countof(line), _TRUNCATE, "restored (dcs-null=%lld substituted=%lld passed=%lld)",
                    g_keep.dcsNull, g_keep.substituted, g_keep.passed);
    } else {
        // object's vptr changed under us (destroyed / reconstructed): leave it alone
        _snprintf_s(line, _countof(line), _TRUNCATE, "vptr no longer ours (%s); left untouched",
                    DescribeAddress(cur).c_str());
    }
    Log("KEEPPLAT %s", line);
    g_keep.cached = nullptr;
    g_keep.view = nullptr;  // table and hook stay in memory (DLL is pinned) in case a call is in flight
    g_ansInstalled = false; // the next start builds a fresh table
    return line;
}

}  // namespace lsodiag
