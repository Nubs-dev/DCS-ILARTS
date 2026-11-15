#include "plat_origin.h"

#include "clouds_probe.h"
#include "common.h"
#include "compat.h"
#include "engine.h"
#include "extra_cams.h"
#include "share.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace lsodiag {
namespace {

// SceneRenderer.dll 2.9.31.29434 disassembles to exactly the same code as 2.9.25.21123, so every RVA
// and offset below is unchanged.
// View setup: (view, camera, const Vec3d* origin, a4, 4 stack args). Camera position (Vec3d) at
// camera+0x60. It builds the view matrix from camera - origin, the culling planes, and calls +0x14FA0
// with float(origin). Hooked to observe only (which views are the PLAT's, how far their origin is).
constexpr uintptr_t kSetupRva = 0x14940;
// mov rax,rsp / mov [rax+18h],rbx / push rbp / push rsi / push rdi / push r14 / push r15
constexpr unsigned char kPrologue[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55,
                                       0x56, 0x57, 0x41, 0x56, 0x41, 0x57};
constexpr size_t kStolen = 7;            // the first two instructions (position independent)
constexpr size_t kCamPosOffset = 0x60;
constexpr double kSnap = 64.0;
constexpr double kMatchMetres = 50.0;    // view camera within this of the PLAT camera: the PLAT view
// DCS shares an object's placement between the views of a frame (the "ghost carrier" of 2026-11-10; and on
// 2026-11-13, with the extra cameras: the PLAT and the island camera each had their own origin, the catapult
// camera the frame's, and the hull - placed for the catapult view - was 184 km off in the other two). So
// every view that shows the carrier gets the SAME substituted origin: every view whose camera is within
// kCarrierMetres of the PLAT camera or at one of the extra cameras (extra_cams.h), all or none per frame.
constexpr double kCarrierMetres = 600.0;
constexpr double kExtraCamMetres = 10.0;
constexpr int kMaxSwapped = 8;
// Only replace when the main camera is this far from the PLAT. NOT a precision threshold: while the main
// view also draws the carrier, the PLAT's carrier (LOD 0, placed relative to the PLAT origin) shows up
// in the main view next to the player's camera, flickering (run 2026-11-10 02:5x, player 2-13 km away).
// The carrier's last LOD ends at 50 km (Nimitz_CVN_7x.lods); 70 km also clears the 65.5 km float step
// boundary, so below it the stock placement steps are at most 3.9 mm.
constexpr double kFarMetres = 70000.0;
constexpr ULONGLONG kStaleMs = 2000;     // PLAT position older than this: no matching

// Frame origin. One origin per frame, floor(DCSFrameDesc+0x80 / 64) * 64 (+0x80 = main camera, Vec3d):
//  - prepareRenderGraph (+0x1F7A0): +0x1F881 calls +0x108D0(out, in) for the snap; the result goes to the
//    view setups (+0x248B0 -> +0x14940, main views and the 0x330-byte extra views such as the PLAT) and,
//    as floats, to the frame data (+0x1F3F0, frame+0x164).
//  - buildRenderGraph (+0x24650): snaps inline and passes the float origin to +0x1F3F0 at +0x2474A.
// While enabled and the main camera is far from the PLAT camera, both get an origin next to the PLAT
// camera instead - for the WHOLE frame, so the main view then has the precision problem (experiment).
constexpr uintptr_t kSnapCallRva = 0x1F881, kSnapFnRva = 0x108D0;
constexpr uintptr_t kFrameCallRva = 0x2474A, kFrameFnRva = 0x1F3F0;
constexpr size_t kDescMainCam = 0x80;
// Per-view mode. Visualizer calls the exported DCSSceneRenderer::buildRenderGraph itself (not through
// prepareRenderGraph - run 2026-11-10 00:37), then has the scene (Scene.dll, ISceneObjectsProvider)
// collect objects, then bakeRenderGraph. The scene works from two arrays of the render graph
// (this+0x8A8): CollectionInfo (graph+0x408..+0x410, 0x6D8 bytes each) and LightsCollectionInfo
// (graph+0x420..+0x428, 0x698 each). Visualizer's import of buildRenderGraph is redirected to BuildWrap.
// Scene.dll subtracts a float origin stored in those (+0x4FC) from double object positions. The view
// contexts carry two origins: the view's own (+0x32C, from the view setup) and a copy of the frame block
// (main camera Vec3d, main camera - origin, origin; +0x4D8 / +0x4F0 / +0x4FC). In this mode the PLAT
// view setup gets an origin next to the PLAT camera and every copy of the frame origin inside the PLAT's
// view struct and collection entries is replaced with it (found by value, not by offset).
constexpr const char* kBuildName = "?buildRenderGraph@DCSSceneRenderer@@QEAAXAEBUDCSFrameDesc@@@Z";
constexpr size_t kGraphOffset = 0x8A8;
constexpr size_t kCollBegin = 0x408, kCollStride = 0x6D8, kLightBegin = 0x420, kLightStride = 0x698;
constexpr size_t kViewSize = 0x578;

using SetupFn = uint64_t (*)(void* view, const void* cam, const double* origin, void* a4, uint64_t a5, uint64_t a6,
                             uint64_t a7, uint64_t a8);
using SnapFn = double* (*)(double* out, const double* in);
using FrameFn = void* (*)(void* self, void* out, const char* desc, const float* origin);
using BuildFn = void (*)(void* self, const void* desc);

struct CallPatch {
    unsigned char* qword = nullptr;
    unsigned long long orig = 0;
    bool on = false;
};

unsigned char* g_entry = nullptr;
unsigned long long g_origQword = 0;
SetupFn g_tramp = nullptr;
SnapFn g_snapFn = nullptr;
FrameFn g_frameFn = nullptr;
BuildFn g_buildFn = nullptr;
CallPatch g_snapPatch, g_framePatch;
void** g_buildSlot = nullptr;      // Visualizer's import slot for buildRenderGraph
volatile long long g_buildCalls = 0;
bool g_installed = false;
std::atomic<int> g_mode{0};   // 0 off, 1 whole frame, 2 PLAT view only

// Per-frame state of the per-view mode (all written on the thread that builds the render graph).
double g_mainCam[3] = {}, g_frameOrigin[3] = {};
bool g_frameKnown = false;
bool g_viewSwapped = false;        // this frame at least one view setup got the substituted origin
double g_viewCam[3] = {};          // the PLAT view's camera this frame
bool g_viewCamSet = false;         // ... seen at least once
float g_subst[3] = {};             // substituted origin (shared by every swapped view of the frame)
void* g_platView = nullptr;
// The views swapped in this build: view setups may run on several threads.
SRWLOCK g_swapLock = SRWLOCK_INIT;
bool g_substSet = false;           // g_subst / g_substD chosen for this build
double g_substD[3] = {};
struct SwappedView {
    void* view;
    double cam[3];
};
SwappedView g_swapped[kMaxSwapped];
int g_swappedN = 0;
volatile long g_swapPeak = 0;      // most views swapped in one build since the last stats call
int g_loggedSwapN = -1;            // number of swapped views last written to the log
struct PatchStat {
    volatile long long regions = 0, origins = 0, rels = 0;
    volatile long offs[6] = {-1, -1, -1, -1, -1, -1};
};
PatchStat g_psView, g_psColl, g_psLight;
volatile long long g_viewReplaced = 0, g_collSeen = 0, g_lightSeen = 0;
bool g_platSeen = false;           // this frame had a PLAT view setup (g_viewCam / g_platView valid)
// Dump of the PLAT's view struct and collection entries, a number of frames after each mode change.
std::atomic<int> g_dumpIn{0};
constexpr int kDumpDelayFrames = 150;
std::atomic<double> g_px{0}, g_py{0}, g_pz{0};
std::atomic<ULONGLONG> g_pTick{0};
volatile long long g_calls = 0, g_plat = 0;
volatile double g_lastDist = 0;
volatile long long g_snapCalls = 0, g_snapReplaced = 0, g_frameCalls = 0, g_frameReplaced = 0;
volatile double g_mainDist = 0;
// Callers of the view setup for PLAT views (return address RVA in SceneRenderer), up to 4 distinct.
uintptr_t g_srBase = 0;
volatile uintptr_t g_platCaller[4] = {};
volatile long long g_platCallerN[4] = {};

bool Same3f(const unsigned char* at, const float* v) { return memcmp(at, v, 12) == 0; }

// Replaces every copy of the frame origin (float3) in [p, p+size) with the substituted origin, and
// every float3 (main camera - frame origin) with (main camera - substituted origin).
void PatchRegion(void* p, size_t size, PatchStat& st) {
    unsigned char buf[0x700];
    if (!p || size > sizeof(buf) || !SafeRead(p, buf, size)) return;
    float fo[3], rel[3], nrel[3];
    for (int i = 0; i < 3; ++i) {
        fo[i] = static_cast<float>(g_frameOrigin[i]);
        rel[i] = static_cast<float>(g_mainCam[i] - static_cast<double>(fo[i]));
        nrel[i] = static_cast<float>(g_mainCam[i] - static_cast<double>(g_subst[i]));
    }
    InterlockedIncrement64(&st.regions);
    auto dst = static_cast<unsigned char*>(p);
    for (size_t off = 0; off + 12 <= size; off += 4) {
        const float* with = nullptr;
        if (Same3f(buf + off, fo)) {
            with = g_subst;
            InterlockedIncrement64(&st.origins);
            for (int i = 0; i < 6; ++i) {
                if (st.offs[i] == static_cast<long>(off)) break;
                if (st.offs[i] < 0) {
                    st.offs[i] = static_cast<long>(off);
                    break;
                }
            }
        } else if (Same3f(buf + off, rel)) {
            with = nrel;
            InterlockedIncrement64(&st.rels);
        }
        if (with) {
            memcpy(dst + off, with, 12);
            off += 8;
        }
    }
}

// True when the region belongs to the PLAT view: it holds the substituted origin (float3) or the PLAT
// view's camera position (Vec3d).
bool IsPlatRegion(const void* p, size_t size) {
    unsigned char buf[0x700];
    if (!p || size > sizeof(buf) || !SafeRead(p, buf, size)) return false;
    for (size_t off = 0; off + 24 <= size; off += 4) {
        if (g_viewSwapped && memcmp(buf + off, g_subst, 12) == 0) return true;
        // (the PLAT camera only once one has been seen: 24 zero bytes are in every region)
        if (g_viewCamSet && memcmp(buf + off, g_viewCam, 24) == 0) return true;
        for (int k = 0; k < g_swappedN; ++k)
            if (memcmp(buf + off, g_swapped[k].cam, 24) == 0) return true;
    }
    return false;
}

void PatchArray(const char* graph, size_t beginOff, size_t stride, PatchStat& st, volatile long long& seen) {
    unsigned char* range[2];
    if (!SafeRead(graph + beginOff, range, sizeof(range)) || !range[0] || range[1] < range[0]) return;
    size_t bytes = static_cast<size_t>(range[1] - range[0]);
    if (bytes % stride != 0 || bytes / stride > 64) return;
    for (unsigned char* e = range[0]; e < range[1]; e += stride) {
        InterlockedIncrement64(&seen);
        if (IsPlatRegion(e, stride)) PatchRegion(e, stride, st);
    }
}

void DumpRegion(const char* tag, int index, const void* p, size_t size) {
    unsigned char buf[0x700];
    if (!p || size > sizeof(buf) || !SafeRead(p, buf, size)) return;
    for (size_t off = 0; off < size; off += 32) {
        char line[160];
        int n = _snprintf_s(line, _countof(line), _TRUNCATE, "PLATDUMP %s %d +%03zX", tag, index, off);
        for (size_t k = off; k < off + 32 && k + 4 <= size; k += 4) {
            uint32_t v;
            memcpy(&v, buf + k, 4);
            n += _snprintf_s(line + n, _countof(line) - n, _TRUNCATE, " %08X", v);
        }
        Log("%s", line);
    }
}

void DumpArray(const char* tag, const char* graph, size_t beginOff, size_t stride) {
    unsigned char* range[2];
    if (!SafeRead(graph + beginOff, range, sizeof(range)) || !range[0] || range[1] < range[0]) return;
    size_t bytes = static_cast<size_t>(range[1] - range[0]);
    if (bytes % stride != 0 || bytes / stride > 64) return;
    int index = 0, plat = 0;
    bool mainDone = false;
    for (unsigned char* e = range[0]; e < range[1]; e += stride, ++index) {
        if (IsPlatRegion(e, stride)) {
            if (plat++ < 2) DumpRegion(tag, index, e, stride);
        } else if (!mainDone) {
            mainDone = true;   // first entry of another view, for comparison
            char other[24];
            _snprintf_s(other, _countof(other), _TRUNCATE, "%s-other", tag);
            DumpRegion(other, index, e, stride);
        }
    }
    Log("PLATDUMP %s entries=%d plat=%d", tag, index, plat);
}

void BuildWrap(void* self, const void* desc) {
    InterlockedIncrement64(&g_buildCalls);
    g_viewSwapped = false;
    g_platSeen = false;
    AcquireSRWLockExclusive(&g_swapLock);
    g_substSet = false;
    g_swappedN = 0;
    ReleaseSRWLockExclusive(&g_swapLock);
    g_buildFn(self, desc);
    if (!self) return;
    if (g_swappedN > g_swapPeak) g_swapPeak = g_swappedN;
    static ULONGLONG s_swapLogTick = 0;
    if (g_swappedN != g_loggedSwapN && GetTickCount64() - s_swapLogTick > 1000) {
        // a line when the number of views on the carrier's origin changes (at most one a second)
        s_swapLogTick = GetTickCount64();
        g_loggedSwapN = g_swappedN;
        if (g_swappedN)
            Log("PLATORIGIN %d view(s) at the carrier share the origin (%.0f, %.0f, %.0f); frame origin (%.0f, %.0f, %.0f)", g_swappedN,
                g_substD[0], g_substD[1], g_substD[2], g_frameOrigin[0], g_frameOrigin[1], g_frameOrigin[2]);
        else
            Log("PLATORIGIN no view on the carrier's own origin (main camera within %.0f km, or no PLAT)", kFarMetres / 1000.0);
    }
    {
        // What this frame actually did, for the viewer's sidebar (screenshots).
        const int mode = g_mode.load(std::memory_order_relaxed);
        const bool isFar = g_platSeen && g_lastDist > kFarMetres;
        const char* o = g_viewSwapped              ? "ORIGIN FIX ON"
                        : mode == 1 && isFar        ? "ORIGIN FIX FRAME"
                        : isFar                    ? "ORIGIN FIX OFF"
                        : g_platSeen               ? "origin fix off (<70 km)"
                                                   : "";
        char text[96] = {};
        if (*o) _snprintf_s(text, _countof(text), _TRUNCATE, "%s | CLOUD RAY FIX %s", o,
                            CloudsRebaseMode() == 2 ? "ON" : "OFF");
        ShareSetStatusText(text);
    }
    const char* graph = static_cast<const char*>(self) + kGraphOffset;
    if (g_viewSwapped) {
        for (int k = 0; k < g_swappedN; ++k) PatchRegion(g_swapped[k].view, kViewSize, g_psView);
        PatchArray(graph, kCollBegin, kCollStride, g_psColl, g_collSeen);
        PatchArray(graph, kLightBegin, kLightStride, g_psLight, g_lightSeen);
    }
    if (g_platSeen && g_dumpIn.load(std::memory_order_relaxed) > 0 && g_dumpIn.fetch_sub(1) == 1 && Verbose()) {
        Log("PLATDUMP begin mode=%d swapped=%d frameOrigin=%.0f %.0f %.0f subst=%.0f %.0f %.0f mainCam=%.3f %.3f %.3f "
            "platCam=%.3f %.3f %.3f",
            g_mode.load(), g_viewSwapped ? 1 : 0, g_frameOrigin[0], g_frameOrigin[1], g_frameOrigin[2], g_subst[0],
            g_subst[1], g_subst[2], g_mainCam[0], g_mainCam[1], g_mainCam[2], g_viewCam[0], g_viewCam[1], g_viewCam[2]);
        DumpRegion("view", 0, g_platView, kViewSize);
        DumpArray("coll", graph, kCollBegin, kCollStride);
        DumpArray("light", graph, kLightBegin, kLightStride);
        Log("PLATDUMP end");
    }
}

uint64_t Wrap(void* view, const void* cam, const double* origin, void* a4, uint64_t a5, uint64_t a6, uint64_t a7,
              uint64_t a8) {
    InterlockedIncrement64(&g_calls);
    if (cam && origin && GetTickCount64() - g_pTick.load(std::memory_order_relaxed) < kStaleMs) {
        auto c = reinterpret_cast<const double*>(static_cast<const char*>(cam) + kCamPosOffset);
        const double plat[3] = {g_px.load(std::memory_order_relaxed), g_py.load(std::memory_order_relaxed),
                                g_pz.load(std::memory_order_relaxed)};
        double dx = c[0] - plat[0], dy = c[1] - plat[1], dz = c[2] - plat[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        const bool isPlat = d2 < kMatchMetres * kMatchMetres;
        if (isPlat) {
            InterlockedIncrement64(&g_plat);
            uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_srBase;
            for (int i = 0; i < 4; ++i) {
                if (g_platCaller[i] == 0)
                    InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(&g_platCaller[i]),
                                                 static_cast<long long>(ra), 0);
                if (g_platCaller[i] == ra) {
                    InterlockedIncrement64(&g_platCallerN[i]);
                    break;
                }
            }
            double ox = c[0] - origin[0], oy = c[1] - origin[1], oz = c[2] - origin[2];
            g_lastDist = std::sqrt(ox * ox + oy * oy + oz * oz);
            for (int i = 0; i < 3; ++i) g_viewCam[i] = c[i];
            g_viewCamSet = true;
            g_platView = view;
            g_platSeen = true;
        }
        // A view at the carrier. Far or not is decided from the PLAT camera, so it is the same answer for
        // every view of the frame; the origin is the PLAT camera's cell, chosen once per build.
        const bool carrier = isPlat || d2 < kCarrierMetres * kCarrierMetres || ExtraCamsNear(c, kExtraCamMetres);
        if (carrier && g_mode.load(std::memory_order_relaxed) == 2 && g_frameKnown) {
            const double fx = plat[0] - origin[0], fy = plat[1] - origin[1], fz = plat[2] - origin[2];
            if (fx * fx + fy * fy + fz * fz > kFarMetres * kFarMetres) {
                double local[3];
                AcquireSRWLockExclusive(&g_swapLock);
                if (!g_substSet) {
                    for (int i = 0; i < 3; ++i) {
                        g_substD[i] = std::floor(plat[i] / kSnap) * kSnap;
                        g_subst[i] = static_cast<float>(g_substD[i]);
                    }
                    g_substSet = true;
                }
                for (int i = 0; i < 3; ++i) local[i] = g_substD[i];
                int at = 0;
                while (at < g_swappedN && g_swapped[at].view != view) ++at;   // a view may be set up more than once
                if (at < kMaxSwapped) {
                    g_swapped[at].view = view;
                    for (int i = 0; i < 3; ++i) g_swapped[at].cam[i] = c[i];
                    if (at == g_swappedN) ++g_swappedN;
                }
                g_viewSwapped = true;
                ReleaseSRWLockExclusive(&g_swapLock);
                uint64_t r = g_tramp(view, cam, local, a4, a5, a6, a7, a8);
                InterlockedIncrement64(&g_viewReplaced);
                PatchRegion(view, kViewSize, g_psView);
                return r;
            }
        }
    }
    return g_tramp(view, cam, origin, a4, a5, a6, a7, a8);
}

// True (and `snapped` = origin next to the PLAT camera) when the fix is on, the PLAT position is fresh
// and the main camera is far from it.
bool PlatOriginFor(const double* mainCam, double* snapped) {
    if (GetTickCount64() - g_pTick.load(std::memory_order_relaxed) >= kStaleMs) return false;
    const double p[3] = {g_px.load(std::memory_order_relaxed), g_py.load(std::memory_order_relaxed),
                         g_pz.load(std::memory_order_relaxed)};
    double dx = p[0] - mainCam[0], dy = p[1] - mainCam[1], dz = p[2] - mainCam[2];
    double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    g_mainDist = dist;
    if (g_mode.load(std::memory_order_relaxed) != 1 || !(dist > kFarMetres)) return false;
    for (int i = 0; i < 3; ++i) snapped[i] = std::floor(p[i] / kSnap) * kSnap;
    return true;
}

double* SnapWrap(double* out, const double* in) {
    double* r = g_snapFn(out, in);
    InterlockedIncrement64(&g_snapCalls);
    double s[3];
    g_frameKnown = out && in;
    if (g_frameKnown) {
        for (int i = 0; i < 3; ++i) {
            g_mainCam[i] = in[i];
            g_frameOrigin[i] = out[i];
        }
    }
    if (out && in && PlatOriginFor(in, s)) {
        out[0] = s[0];
        out[1] = s[1];
        out[2] = s[2];
        InterlockedIncrement64(&g_snapReplaced);
    }
    return r;
}

void* FrameWrap(void* self, void* out, const char* desc, const float* origin) {
    InterlockedIncrement64(&g_frameCalls);
    float local[3];
    double s[3];
    if (desc && origin && PlatOriginFor(reinterpret_cast<const double*>(desc + kDescMainCam), s)) {
        for (int i = 0; i < 3; ++i) local[i] = static_cast<float>(s[i]);
        origin = local;
        InterlockedIncrement64(&g_frameReplaced);
    }
    return g_frameFn(self, out, desc, origin);
}

// Executable memory within +-2 GB of `anchor` for rel32 jumps.
unsigned char* AllocNear(const void* anchor, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t base = reinterpret_cast<uintptr_t>(anchor) & ~(gran - 1);
    for (uintptr_t d = gran; d < 0x70000000; d += gran) {
        for (uintptr_t a : {base - d, base + d}) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p) return static_cast<unsigned char*>(p);
        }
    }
    return nullptr;
}

void PutAbsJump(unsigned char* at, const void* target) {
    at[0] = 0xFF;
    at[1] = 0x25;
    memset(at + 2, 0, 4);
    auto t = reinterpret_cast<uint64_t>(target);
    memcpy(at + 6, &t, 8);
}

// Swaps the aligned qword at `at` atomically.
bool SwapQword(unsigned char* at, unsigned long long expect, unsigned long long value) {
    DWORD old = 0;
    if (!VirtualProtect(at, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(at), static_cast<long long>(value),
                                           static_cast<long long>(expect)) == static_cast<long long>(expect);
    DWORD tmp = 0;
    VirtualProtect(at, 8, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), at, 8);
    return ok;
}

// Redirects the `call rel32` at base+callRva (which must call base+fnRva) to `stub`. The five bytes lie
// inside one aligned qword, which is swapped atomically.
std::string PatchCall(unsigned char* base, uintptr_t callRva, uintptr_t fnRva, const unsigned char* stub,
                      CallPatch& cp) {
    unsigned char* call = base + callRva;
    unsigned char ins[5];
    int32_t rel = 0;
    if (!SafeRead(call, ins, sizeof(ins)) || ins[0] != 0xE8) return "no call instruction";
    memcpy(&rel, ins + 1, 4);
    if (call + 5 + rel != base + fnRva) return "call has an unexpected target";
    long long nrel = reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(call + 5);
    if (nrel < INT32_MIN || nrel > INT32_MAX) return "stub out of rel32 range";
    auto q = reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(call) & ~static_cast<uintptr_t>(7));
    size_t off = static_cast<size_t>(call - q);
    if (off + 5 > 8) return "call crosses an 8-byte boundary";
    unsigned char b[8];
    memcpy(&cp.orig, q, 8);
    memcpy(b, &cp.orig, 8);
    int32_t n32 = static_cast<int32_t>(nrel);
    memcpy(b + off + 1, &n32, 4);
    unsigned long long patched;
    memcpy(&patched, b, 8);
    if (!SwapQword(q, cp.orig, patched)) return "could not patch";
    cp.qword = q;
    cp.on = true;
    return "";
}

// The slot in `mod`'s import address table that holds `target`.
void** FindImportSlot(HMODULE mod, const void* target) {
    auto b = reinterpret_cast<unsigned char*>(mod);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    if (!dir.VirtualAddress || dir.Size < sizeof(void*)) return nullptr;
    auto slots = reinterpret_cast<void**>(b + dir.VirtualAddress);
    for (size_t i = 0; i < dir.Size / sizeof(void*); ++i) {
        void* v = nullptr;
        if (SafeRead(&slots[i], &v, sizeof(v)) && v == target) return &slots[i];
    }
    return nullptr;
}

bool WriteSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    return true;
}

std::string PatchBuildImport(HMODULE sr) {
    HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
    if (!vis) return "Visualizer.dll not loaded";
    void* fn = reinterpret_cast<void*>(GetProcAddress(sr, kBuildName));
    if (!fn) return "buildRenderGraph not exported";
    void** slot = FindImportSlot(vis, fn);
    if (!slot) return "Visualizer does not import buildRenderGraph";
    g_buildFn = reinterpret_cast<BuildFn>(fn);
    if (!WriteSlot(slot, reinterpret_cast<void*>(&BuildWrap))) return "could not write the import slot";
    g_buildSlot = slot;
    return "";
}

void UnpatchCall(CallPatch& cp) {
    if (!cp.on) return;
    unsigned long long cur;
    memcpy(&cur, cp.qword, 8);
    SwapQword(cp.qword, cur, cp.orig);
    cp.on = false;
}

}  // namespace

void PlatOriginNoteCamera(double x, double y, double z) {
    g_px.store(x, std::memory_order_relaxed);
    g_py.store(y, std::memory_order_relaxed);
    g_pz.store(z, std::memory_order_relaxed);
    g_pTick.store(GetTickCount64(), std::memory_order_relaxed);
}

bool PlatOriginViewFrameOrigin(double origin[3]) {
    if (!g_viewSwapped || !g_frameKnown) return false;
    for (int i = 0; i < 3; ++i) origin[i] = static_cast<double>(static_cast<float>(g_frameOrigin[i]));
    return true;
}

bool PlatOriginContextSwapped(const void* ctx) {
    if (!g_viewSwapped || !ctx) return false;
    float o[3];
    if (!SafeRead(static_cast<const char*>(ctx) + 0x32C, o, sizeof(o))) return false;
    return memcmp(o, g_subst, sizeof(o)) == 0;
}

bool PlatOriginPlatCamera(double cam[3]) {
    for (int i = 0; i < 3; ++i) cam[i] = g_viewCam[i];
    return cam[0] != 0 || cam[1] != 0 || cam[2] != 0;
}

std::string PlatOriginInstall() {
    if (EngineDisabled()) return "engine calls disabled";
    if (g_installed) return "";
    HMODULE sr = GetModuleHandleW(L"SceneRenderer.dll");
    if (!sr) return "SceneRenderer.dll not loaded";
    if (!CompatOk()) return CompatWhy();
    auto base = reinterpret_cast<unsigned char*>(sr);
    g_srBase = reinterpret_cast<uintptr_t>(base);
    unsigned char* entry = base + kSetupRva;
    if ((reinterpret_cast<uintptr_t>(entry) & 7) != 0) return "entry not 8-byte aligned";
    unsigned char pro[sizeof(kPrologue)];
    if (!SafeRead(entry, pro, sizeof(pro)) || memcmp(pro, kPrologue, sizeof(pro)) != 0)
        return "SceneRenderer+0x14940 is not the expected function";
    // [0..20]  trampoline: stolen 7 bytes + jmp [entry+7]
    // [32..45] jump to Wrap   [48..61] jump to SnapWrap   [64..77] jump to FrameWrap
    unsigned char* mem = AllocNear(entry, 96);
    if (!mem) return "no executable memory near SceneRenderer.dll";
    memcpy(mem, kPrologue, kStolen);
    PutAbsJump(mem + kStolen, entry + kStolen);
    PutAbsJump(mem + 32, reinterpret_cast<const void*>(&Wrap));
    PutAbsJump(mem + 48, reinterpret_cast<const void*>(&SnapWrap));
    PutAbsJump(mem + 64, reinterpret_cast<const void*>(&FrameWrap));
    long long rel = reinterpret_cast<intptr_t>(mem + 32) - reinterpret_cast<intptr_t>(entry + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return "stub out of rel32 range";
    DWORD old = 0;
    if (!VirtualProtect(mem, 96, PAGE_EXECUTE_READ, &old)) return "VirtualProtect (stubs) failed";
    FlushInstructionCache(GetCurrentProcess(), mem, 96);
    g_tramp = reinterpret_cast<SetupFn>(mem);
    g_snapFn = reinterpret_cast<SnapFn>(base + kSnapFnRva);
    g_frameFn = reinterpret_cast<FrameFn>(base + kFrameFnRva);
    // New first qword: jmp rel32 (5) + nop nop (2) + original 8th byte (push rbp).
    memcpy(&g_origQword, entry, 8);
    unsigned char q[8];
    memcpy(q, &g_origQword, 8);
    q[0] = 0xE9;
    int32_t r32 = static_cast<int32_t>(rel);
    memcpy(q + 1, &r32, 4);
    q[5] = 0x90;
    q[6] = 0x90;
    unsigned long long patched;
    memcpy(&patched, q, 8);
    g_entry = entry;
    if (!SwapQword(entry, g_origQword, patched)) return "could not patch SceneRenderer+0x14940";
    g_installed = true;
    Log("PLATORIGIN installed: SceneRenderer+0x14940 (view setup, observe) -> wrapper (trampoline %p)", mem);
    std::string a = PatchCall(base, kSnapCallRva, kSnapFnRva, mem + 48, g_snapPatch);
    std::string b = a.empty() ? PatchCall(base, kFrameCallRva, kFrameFnRva, mem + 64, g_framePatch) : "skipped";
    std::string c = a.empty() && b.empty() ? PatchBuildImport(sr) : "skipped";
    if (!a.empty() || !b.empty() || !c.empty()) {
        UnpatchCall(g_snapPatch);
        UnpatchCall(g_framePatch);
        Log("PLATORIGIN origin hooks NOT installed: +0x1F881 %s / +0x2474A %s / buildRenderGraph import %s",
            a.empty() ? "ok" : a.c_str(), b.empty() ? "ok" : b.c_str(), c.empty() ? "ok" : c.c_str());
    } else {
        Log("PLATORIGIN installed: frame origin at SceneRenderer+0x1F881 (buildRenderGraph) and +0x2474A (aux "
            "buffers only), Visualizer's buildRenderGraph import -> wrapper (collection patch)");
    }
    return "";
}

void PlatOriginRemove() {
    if (!g_installed) return;
    g_mode = 0;
    if (g_buildSlot) {
        WriteSlot(g_buildSlot, reinterpret_cast<void*>(g_buildFn));
        g_buildSlot = nullptr;
    }
    UnpatchCall(g_snapPatch);
    UnpatchCall(g_framePatch);
    unsigned long long cur;
    memcpy(&cur, g_entry, 8);
    SwapQword(g_entry, cur, g_origQword);
    g_installed = false;
    Log("PLATORIGIN removed (SceneRenderer code restored; stubs stay allocated)");
}

void PlatOriginEnable(int mode) {
    if (g_mode.exchange(mode) != mode) {
        g_dumpIn = kDumpDelayFrames;
        Log("PLATORIGIN %s", mode == 1   ? "FRAME (whole frame gets an origin next to the PLAT camera while it is far away)"
                             : mode == 2 ? "VIEW (the views at the carrier - PLAT and extra cameras - share an origin next to the PLAT camera)"
                                         : "OFF (stock origin)");
    }
}

std::string PlatOriginStats() {
    if (!g_installed) return "not installed";
    char b[240];
    _snprintf_s(b, _countof(b), _TRUNCATE, "%s | view setups=%lld plat=%lld originDist=%.0f",
                g_mode == 1 ? "frame" : g_mode == 2 ? "view" : "off", InterlockedExchange64(&g_calls, 0), InterlockedExchange64(&g_plat, 0),
                g_lastDist);
    std::string out = b;
    for (int i = 0; i < 4 && g_platCaller[i]; ++i) {
        _snprintf_s(b, _countof(b), _TRUNCATE, " | PLAT caller +0x%llX x%lld", static_cast<unsigned long long>(g_platCaller[i]),
                    g_platCallerN[i]);
        out += b;
    }
    if (!g_snapPatch.on || !g_framePatch.on || !g_buildSlot) return out + " || origin hooks not installed";
    _snprintf_s(b, _countof(b), _TRUNCATE,
                " || frame origin: prepare calls=%lld replaced=%lld | build calls=%lld replaced=%lld | main camera to PLAT=%.0f",
                InterlockedExchange64(&g_snapCalls, 0), InterlockedExchange64(&g_snapReplaced, 0),
                InterlockedExchange64(&g_frameCalls, 0), InterlockedExchange64(&g_frameReplaced, 0), g_mainDist);
    out += b;
    _snprintf_s(b, _countof(b), _TRUNCATE,
                " || view mode: build calls=%lld setups replaced=%lld (up to %ld views per frame) | entries seen coll=%lld light=%lld",
                InterlockedExchange64(&g_buildCalls, 0), InterlockedExchange64(&g_viewReplaced, 0), InterlockedExchange(&g_swapPeak, 0),
                InterlockedExchange64(&g_collSeen, 0), InterlockedExchange64(&g_lightSeen, 0));
    out += b;
    struct {
        const char* name;
        PatchStat* st;
    } all[] = {{"view", &g_psView}, {"coll", &g_psColl}, {"light", &g_psLight}};
    for (auto& a : all) {
        _snprintf_s(b, _countof(b), _TRUNCATE, " | %s patched=%lld origins=%lld rels=%lld at", a.name,
                    InterlockedExchange64(&a.st->regions, 0), InterlockedExchange64(&a.st->origins, 0),
                    InterlockedExchange64(&a.st->rels, 0));
        out += b;
        for (int i = 0; i < 6 && a.st->offs[i] >= 0; ++i) {
            _snprintf_s(b, _countof(b), _TRUNCATE, " +0x%lX", a.st->offs[i]);
            out += b;
        }
    }
    return out;
}

}  // namespace lsodiag
