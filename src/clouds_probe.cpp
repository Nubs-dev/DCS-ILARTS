#include "clouds_probe.h"

#include "extra_cams.h"

#include "plat_origin.h"

#include "common.h"
#include "compat.h"
#include "engine.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

namespace lsodiag {
namespace {

constexpr const char* kEnlight = "enlight.dll";
constexpr const char* kAddPass =
    "?addRenderingPass@CloudsRenderer@render@@QEAAXAEAVRenderGraph@2@AEBUISceneRenderContextBinder@2@AEBU"
    "SceneRenderContext@2@VRenderGraphTexture@2@3333PEBVIRenderGraphDependencyList@2@_N5AEBVVec2f@osg@@AEBVVec4f@9@@Z";
constexpr const char* kCreate =
    "?createRenderer@CloudsModule@render@@QEAA?AVCloudsRenderer@2@HHW4MSAA_ENUM@2@PEBUCloudsDetailPreset@enlight@@PEBD@Z";

// Win64: textures (RenderGraphTexture by value, > 8 bytes) are passed by hidden pointer.
using AddPassFn = void (*)(void* self, void* graph, void* binder, const void* ctx, void* t1, void* t2, void* t3, void* t4,
                           void* t5, void* deps, bool b1, bool b2, const float* v2, const float* v4);
// Member function returning a class by value: hidden return pointer follows `this`.
using CreateFn = void* (*)(void* self, void* ret, int w, int h, int msaa, const void* preset, const char* name);

constexpr int kMaxRenderers = 24;
constexpr size_t kCtxDump = 0x600;
constexpr int kHist = 6;

struct Rec {
    const void* impl = nullptr;   // first qword of the CloudsRenderer object (stable identity)
    const void* self = nullptr;
    uintptr_t site = 0;
    long long calls = 0, b1[2] = {}, b2[2] = {}, v2Changes = 0, v4Changes = 0;
    float v2[2] = {}, v4[4] = {};
    float v2Hist[kHist][2] = {};
    int histN = 0;
    unsigned long tid = 0;
    bool dumpWanted = false;
    bool dumpReady = false;
    unsigned char ctx[kCtxDump] = {};
};

struct Created {
    const void* impl;
    int w, h, msaa;
    char name[48];
    uintptr_t site;
    long long count;
};

SRWLOCK g_lock = SRWLOCK_INIT;
Rec g_rec[kMaxRenderers];
int g_recN = 0;
Created g_created[kMaxRenderers];
int g_createdN = 0;
long long g_createTotal = 0, g_overflow = 0;
void** g_addSlot = nullptr;
void** g_createSlot = nullptr;
AddPassFn g_origAdd = nullptr;
CreateFn g_origCreate = nullptr;
HMODULE g_sr = nullptr;

const char* NameOf(const void* impl);

// ---- PLAT camera trace (deck jitter investigation) ----
// Per PLAT cloud pass (once per PLAT frame): QPC time, the camera position the renderer uses (ctx+0x308,
// double, world) and the view's forward axis. Armed by CloudsTrace(true), dumped to the log by
// CloudsTraceDump.
struct CamSample {
    double t;
    double cam[3];
    float fwd[3];
};
constexpr int kTraceMax = 600;
CamSample g_trace[kTraceMax];
int g_traceN = 0;
bool g_traceOn = false;

double QpcSeconds() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// ---- PLAT cloud pass precision fix (experiment) ----
// SceneRenderContext (0x588 bytes; row-vector matrices, translation in row 3), verified on dumps:
//   +0x04 / +0x44  altitude frame (translation y = origin.y) and inverse
//   +0x84 V (world-rel -> view)   +0xC4 V^-1   +0x104 P   +0x144 P^-1   +0x184 V*P   +0x1C4 P^-1*V^-1
//   +0x284 copy of V*P   +0x308 camera (Vec3d)   +0x320 camera - origin   +0x32C origin   +0x4F0 main
//   camera - origin   +0x4FC origin   +0x540 origin.y
// enlight's cloud shaders (enlight/clouds.fx, clouddetail.fx, from Bazar/shaders/fxo) build each pixel's
// ray as normalize(unproject(ndc.xy, z = 0.5, viewProjInv) - cameraPos), viewProjInv = ctx+0x1C4 and
// cameraPos = ctx+0x320. With the PLAT's reverse-Z projection z = 0.5 is ~4 cm in front of the camera,
// so with the camera 16 km from the origin (float resolution ~2 mm) the ray directions are off by up to
// a few percent: the blocky, jittering PLAT clouds. The origin, cloudVolumeOffset etc. reach the shader
// from elsewhere (cbCloudsParams.origin, cPerView), so moving the origin (mode 1) shifts the clouds.
// Mode 2 ("dir") leaves everything else alone and rebuilds only P^-1*V^-1 in double with its depth row
// scaled so that z = 0.5 unprojects ~4 km out: same ray directions, ~1e-6 rad error. The only other
// reader, cloudsupsampling's invWVP, skips the depth row (.xyw).
constexpr size_t kCtxSize = 0x600;   // context is 0x588; copy a little more
// Copies handed to the cloud passes, used in turn: one per pass, and with the extra cameras there are several
// passes per frame, so enough of them that a copy outlives the frames in which its pass may still read it.
constexpr int kRebaseBufs = 16;
alignas(16) unsigned char g_rebaseBuf[kRebaseBufs][kCtxSize];
int g_rebaseFlip = 0;
std::atomic<int> g_rebase{0};   // 0 off, 1 full rebase (moves the clouds), 2 ray-direction fix
long long g_rebased = 0, g_rebaseSkipped = 0, g_unrebased = 0;
double g_rebaseDist = 0;
double g_dirErrOld = 0, g_dirErrNew = 0;   // worst ray-direction error seen this interval (radians)

// Ray matrix substitution (2026-11-10). Writing the scaled-depth-row matrix into the context also
// changed what cloudspreprocess / cloudsupsampling use to turn scene depth into distance (cPerView
// gViewProjInv): every object read as hundreds of km away and the clouds drew in front of everything.
// So the context stays true, and only enlight's own reader gets the fixed matrix: in its pass function
// `lea r12,[rsi+1C4h]` at enlight+0x21644 (rsi = context; r12 feeds cbCloudsParams.invWVP via +0x28B10
// and $Globals.viewProjInv) is replaced by a call to a stub that asks PickInvVP. The fixed matrices are
// found by content (true matrix + camera - origin), since enlight may hold its own copy of the context.
constexpr uintptr_t kPickRva = 0x21644;
constexpr unsigned char kPickOrig[7] = {0x4C, 0x8D, 0xA6, 0xC4, 0x01, 0x00, 0x00};
struct PickEntry {
    unsigned char key[64 + 12];
    float fixed[16];
    bool used;
};
PickEntry g_pick[32];   // a few frames' worth with the PLAT and the extra cameras
int g_pickNext = 0;
SRWLOCK g_pickLock = SRWLOCK_INIT;
std::atomic<bool> g_pickOn{false};
volatile long long g_pickCalls = 0, g_pickHits = 0;
unsigned char* g_pickSite = nullptr;          // 16-aligned block holding the patched instruction
alignas(16) long long g_pickOrigBlock[2], g_pickNewBlock[2];

void PickStore(const unsigned char* trueM, const float* camRel, const float* fixed) {
    AcquireSRWLockExclusive(&g_pickLock);
    PickEntry& e = g_pick[g_pickNext = (g_pickNext + 1) % _countof(g_pick)];
    memcpy(e.key, trueM, 64);
    memcpy(e.key + 64, camRel, 12);
    memcpy(e.fixed, fixed, sizeof(e.fixed));
    e.used = true;
    ReleaseSRWLockExclusive(&g_pickLock);
}

// Diagnostic: cbCloudsParams (enlight object +0x16C0, 0x190 bytes, filled and uploaded per pass by
// +0x28B10 right after the hooked instruction) as left by the previous pass, kept separately for the
// PLAT's pass [1] and the others [0]. Logged by CloudsRebaseStats.
constexpr size_t kCbSize = 0x190;
unsigned char g_cbSnap[2][kCbSize];
volatile long g_cbHave[2] = {0, 0};
int g_prevKind = -1;
const unsigned char* g_enBase = nullptr;

void SnapCloudCb(const unsigned char* ctx) {
    if (g_prevKind >= 0 && g_enBase) {
        const void* self = nullptr;
        if (SafeReadPtr(g_enBase + 0xA08D8, &self) && self &&
            SafeRead(static_cast<const unsigned char*>(self) + 0x16C0, g_cbSnap[g_prevKind], kCbSize))
            g_cbHave[g_prevKind] = 1;
    }
    double pc[3], c[3];
    memcpy(c, ctx + 0x308, sizeof(c));
    g_prevKind = 0;
    if (PlatOriginPlatCamera(pc)) {
        const double dx = c[0] - pc[0], dy = c[1] - pc[1], dz = c[2] - pc[2];
        if (dx * dx + dy * dy + dz * dz < 50.0 * 50.0) g_prevKind = 1;
    }
}

void DumpCloudCb() {
    static const char* const kTag[2] = {"other", "plat"};
    for (int k = 0; k < 2; ++k) {
        if (!g_cbHave[k]) continue;
        float f[kCbSize / 4];
        memcpy(f, g_cbSnap[k], kCbSize);
        for (size_t i = 0; i < kCbSize / 4; i += 8) {
            char line[220];
            int n = _snprintf_s(line, _countof(line), _TRUNCATE, "CLOUDCB %s +%03zu:", kTag[k], i * 4);
            for (size_t j = i; j < i + 8 && j < kCbSize / 4; ++j)
                n += _snprintf_s(line + n, _countof(line) - n, _TRUNCATE, " %.7g", f[j]);
            Log("%s", line);
        }
    }
}

// Called from the stub in enlight's pass function: the matrix r12 should point to.
const void* PickInvVP(const unsigned char* ctx) {
    InterlockedIncrement64(&g_pickCalls);
    SnapCloudCb(ctx);
    const void* stock = ctx + 0x1C4;
    if (g_rebase.load(std::memory_order_relaxed) != 2) return stock;
    unsigned char key[64 + 12];
    memcpy(key, ctx + 0x1C4, 64);
    memcpy(key + 64, ctx + 0x320, 12);
    static thread_local float t_fixed[16];
    bool hit = false;
    AcquireSRWLockShared(&g_pickLock);
    for (const PickEntry& e : g_pick)
        if (e.used && memcmp(e.key, key, sizeof(key)) == 0) {
            memcpy(t_fixed, e.fixed, sizeof(t_fixed));
            hit = true;
            break;
        }
    ReleaseSRWLockShared(&g_pickLock);
    if (!hit) return stock;
    InterlockedIncrement64(&g_pickHits);
    return t_fixed;
}

unsigned char* PickAllocNear(const void* anchor, size_t size) {
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

bool PickSwapBlock(const long long* expect, const long long* put) {
    DWORD old = 0;
    if (!VirtualProtect(g_pickSite, 16, PAGE_EXECUTE_READWRITE, &old)) return false;
    alignas(16) long long cmp[2] = {expect[0], expect[1]};
    const bool ok = _InterlockedCompareExchange128(reinterpret_cast<long long*>(g_pickSite), put[1], put[0], cmp) != 0;
    DWORD tmp = 0;
    VirtualProtect(g_pickSite, 16, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), g_pickSite, 16);
    return ok;
}

std::string PickInstall(HMODULE en) {
    if (g_pickOn) return "";
    if (!CompatOk()) return CompatWhy();
    unsigned char* site = reinterpret_cast<unsigned char*>(en) + kPickRva;
    g_enBase = reinterpret_cast<const unsigned char*>(en);
    unsigned char cur[7];
    if (!SafeRead(site, cur, sizeof(cur)) || memcmp(cur, kPickOrig, sizeof(cur)) != 0)
        return "enlight+0x21644 is not the expected instruction";
    static unsigned char* stub = nullptr;
    if (!stub) {
        unsigned char* mem = PickAllocNear(site, 128);
        if (!mem) return "no memory near enlight.dll";
        unsigned char code[128];
        size_t n = 0;
        auto put = [&](std::initializer_list<unsigned char> bytes) {
            for (unsigned char c : bytes) code[n++] = c;
        };
        put({0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});          // push r8..r11
        put({0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00});                // sub rsp,88h
        for (unsigned char i = 0; i < 6; ++i)                           // movups [rsp+20h+16i],xmm_i
            put({0x0F, 0x11, static_cast<unsigned char>(0x44 + 8 * i), 0x24, static_cast<unsigned char>(0x20 + 16 * i)});
        put({0x48, 0x89, 0xF1});                                        // mov rcx,rsi
        put({0x48, 0xB8});                                              // mov rax,imm64
        auto fn = reinterpret_cast<uint64_t>(&PickInvVP);
        memcpy(code + n, &fn, 8);
        n += 8;
        put({0xFF, 0xD0});                                              // call rax
        put({0x49, 0x89, 0xC4});                                        // mov r12,rax
        for (unsigned char i = 0; i < 6; ++i)                           // movups xmm_i,[rsp+20h+16i]
            put({0x0F, 0x10, static_cast<unsigned char>(0x44 + 8 * i), 0x24, static_cast<unsigned char>(0x20 + 16 * i)});
        put({0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00});                // add rsp,88h
        put({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58});          // pop r11..r8
        put({0xC3});
        memcpy(mem, code, n);
        DWORD old = 0;
        if (!VirtualProtect(mem, 128, PAGE_EXECUTE_READ, &old)) return "VirtualProtect (stub) failed";
        FlushInstructionCache(GetCurrentProcess(), mem, 128);
        stub = mem;
    }
    const long long rel = reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(site + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return "stub out of rel32 range";
    g_pickSite = reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(site) & ~uintptr_t(15));
    const size_t off = static_cast<size_t>(site - g_pickSite);
    if (off + 7 > 16) return "instruction crosses the 16-byte block";
    memcpy(g_pickOrigBlock, g_pickSite, 16);
    unsigned char nb[16];
    memcpy(nb, g_pickOrigBlock, 16);
    nb[off] = 0xE8;                                                     // call stub ; 66 90
    const int32_t r32 = static_cast<int32_t>(rel);
    memcpy(nb + off + 1, &r32, 4);
    nb[off + 5] = 0x66;
    nb[off + 6] = 0x90;
    memcpy(g_pickNewBlock, nb, 16);
    if (!PickSwapBlock(g_pickOrigBlock, g_pickNewBlock)) return "could not patch enlight+0x21644";
    g_pickOn = true;
    return "";
}

void PickRemove() {
    if (!g_pickOn.exchange(false)) return;
    PickSwapBlock(g_pickNewBlock, g_pickOrigBlock);
}

void LoadM(const unsigned char* b, size_t off, double m[4][4]) {
    const float* f = reinterpret_cast<const float*>(b + off);
    for (int i = 0; i < 16; ++i) m[i / 4][i % 4] = f[i];
}
void StoreM(unsigned char* b, size_t off, const double m[4][4]) {
    float* f = reinterpret_cast<float*>(b + off);
    for (int i = 0; i < 16; ++i) f[i] = static_cast<float>(m[i / 4][i % 4]);
}
void Mul(const double a[4][4], const double c[4][4], double o[4][4]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a[i][k] * c[k][j];
            o[i][j] = s;
        }
}

// Re-expresses the context copy `b` relative to the origin `on`: view matrices, camera - origin, the
// view's origin and the altitude fields. The frame block (+0x4F0 / +0x4FC) is left alone.
void RebaseBufTo(unsigned char* b, const double on[3]) {
    double cam[3], c[3];
    memcpy(cam, b + 0x308, sizeof(cam));
    for (int i = 0; i < 3; ++i) c[i] = cam[i] - on[i];
    double V[4][4], Vi[4][4], P[4][4], Pi[4][4], VP[4][4], VPi[4][4];
    LoadM(b, 0x84, V);
    LoadM(b, 0xC4, Vi);
    LoadM(b, 0x104, P);
    LoadM(b, 0x144, Pi);
    for (int j = 0; j < 3; ++j) V[3][j] = -(c[0] * V[0][j] + c[1] * V[1][j] + c[2] * V[2][j]);
    V[3][3] = 1;
    for (int j = 0; j < 3; ++j) Vi[3][j] = c[j];
    Vi[3][3] = 1;
    Mul(V, P, VP);
    Mul(Pi, Vi, VPi);
    StoreM(b, 0x84, V);
    StoreM(b, 0xC4, Vi);
    StoreM(b, 0x184, VP);
    StoreM(b, 0x1C4, VPi);
    StoreM(b, 0x284, VP);
    float* f = reinterpret_cast<float*>(b + 0x320);
    for (int i = 0; i < 3; ++i) f[i] = static_cast<float>(c[i]);
    f = reinterpret_cast<float*>(b + 0x32C);
    for (int i = 0; i < 3; ++i) f[i] = static_cast<float>(on[i]);
    reinterpret_cast<float*>(b + 0x04)[13] = static_cast<float>(on[1]);
    reinterpret_cast<float*>(b + 0x44)[13] = static_cast<float>(-on[1]);
    *reinterpret_cast<float*>(b + 0x540) = static_cast<float>(on[1]);
}

// Returns the rebased copy, or null to pass DCS's context unchanged. Caller holds g_lock.
const void* RebaseCtx(const void* ctx) {
    unsigned char* b = g_rebaseBuf[g_rebaseFlip = (g_rebaseFlip + 1) % kRebaseBufs];
    if (!SafeRead(ctx, b, kCtxSize)) return nullptr;
    double cam[3];
    memcpy(cam, b + 0x308, sizeof(cam));
    const float* o = reinterpret_cast<const float*>(b + 0x32C);
    double dx = cam[0] - o[0], dy = cam[1] - o[1], dz = cam[2] - o[2];
    g_rebaseDist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(g_rebaseDist > 500.0 && g_rebaseDist < 5.0e6)) return nullptr;
    double on[3], d[3], c[3];
    for (int i = 0; i < 3; ++i) {
        on[i] = std::floor(cam[i] / 64.0) * 64.0;
        d[i] = o[i] - on[i];        // old origin - new origin
        c[i] = cam[i] - on[i];      // camera relative to the new origin
    }
    double V[4][4], Vi[4][4], P[4][4], Pi[4][4], VP[4][4], VPi[4][4];
    LoadM(b, 0x84, V);
    LoadM(b, 0xC4, Vi);
    LoadM(b, 0x104, P);
    LoadM(b, 0x144, Pi);
    for (int j = 0; j < 3; ++j) V[3][j] = -(c[0] * V[0][j] + c[1] * V[1][j] + c[2] * V[2][j]);
    V[3][3] = 1;
    for (int j = 0; j < 3; ++j) Vi[3][j] = c[j];
    Vi[3][3] = 1;
    Mul(V, P, VP);
    Mul(Pi, Vi, VPi);
    StoreM(b, 0x84, V);
    StoreM(b, 0xC4, Vi);
    StoreM(b, 0x184, VP);
    StoreM(b, 0x1C4, VPi);
    StoreM(b, 0x284, VP);
    float* f = nullptr;
    f = reinterpret_cast<float*>(b + 0x320);
    for (int i = 0; i < 3; ++i) f[i] = static_cast<float>(c[i]);
    f = reinterpret_cast<float*>(b + 0x4F0);
    for (int i = 0; i < 3; ++i) f[i] = static_cast<float>(f[i] + d[i]);
    for (size_t off : {size_t(0x32C), size_t(0x4FC)}) {
        f = reinterpret_cast<float*>(b + off);
        for (int i = 0; i < 3; ++i) f[i] = static_cast<float>(on[i]);
    }
    reinterpret_cast<float*>(b + 0x04)[13] = static_cast<float>(on[1]);
    reinterpret_cast<float*>(b + 0x44)[13] = static_cast<float>(-on[1]);
    *reinterpret_cast<float*>(b + 0x540) = static_cast<float>(on[1]);
    return b;
}


// Ray direction the shader gets for ndc (x, y) with a float matrix: normalize((ndc,0.5,1)*M / w - cam).
void ShaderRay(const float* m, const float* cam, float x, float y, double out[3]) {
    float h[4];
    for (int j = 0; j < 4; ++j) h[j] = x * m[0 * 4 + j] + y * m[1 * 4 + j] + 0.5f * m[2 * 4 + j] + m[3 * 4 + j];
    float d[3];
    for (int j = 0; j < 3; ++j) d[j] = h[j] / h[3] - cam[j];
    double l = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
    for (int j = 0; j < 3; ++j) out[j] = l > 0 ? d[j] / l : 0;
}

bool Invert(const double a[4][4], double o[4][4]) {
    double m[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) m[i][j] = j < 4 ? a[i][j] : (j - 4 == i ? 1 : 0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
        if (std::fabs(m[p][c]) < 1e-30) return false;
        if (p != c)
            for (int j = 0; j < 8; ++j) std::swap(m[p][j], m[c][j]);
        double inv = 1.0 / m[c][c];
        for (int j = 0; j < 8; ++j) m[c][j] *= inv;
        for (int r = 0; r < 4; ++r)
            if (r != c && m[r][c] != 0) {
                double f = m[r][c];
                for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j];
            }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) o[i][j] = m[i][j + 4];
    return true;
}

// Mode 2: replaces only P^-1*V^-1 in the context copy `b` (see above). False = left unchanged.
bool DirFixBuf(unsigned char* b) {
    const float* camRel = reinterpret_cast<const float*>(b + 0x320);   // what the shader subtracts
    g_rebaseDist = std::sqrt(double(camRel[0]) * camRel[0] + double(camRel[1]) * camRel[1] +
                             double(camRel[2]) * camRel[2]);
    if (!(g_rebaseDist > 200.0 && g_rebaseDist < 5.0e6)) return false;
    // V^-1 in double: rotation from V (orthonormal), translation = the float camRel the shader will
    // subtract, so the two agree exactly.
    double V[4][4], Vi[4][4], P[4][4], Pi[4][4], VPi[4][4];
    LoadM(b, 0x84, V);
    LoadM(b, 0x104, P);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) Vi[i][j] = V[j][i];
    for (int i = 0; i < 3; ++i) Vi[i][3] = 0;
    for (int j = 0; j < 3; ++j) Vi[3][j] = camRel[j];
    Vi[3][3] = 1;
    if (!Invert(P, Pi)) return false;
    Mul(Pi, Vi, VPi);
    // Scale the depth row so z = 0.5 unprojects to distance ~L: 0.5*s*VPi[2][3] + VPi[3][3] = 1/L.
    constexpr double L = 4000.0;
    if (!(std::fabs(VPi[2][3]) > 1e-12)) return false;
    double s = (1.0 / L - VPi[3][3]) / (0.5 * VPi[2][3]);
    if (!(s > 0 && s <= 1)) return false;
    for (int j = 0; j < 4; ++j) VPi[2][j] *= s;
    // Measure the ray error of DCS's matrix and of ours at a few screen points against the exact ray.
    float oldM[16], newM[16];
    memcpy(oldM, b + 0x1C4, sizeof(oldM));
    for (int i = 0; i < 16; ++i) newM[i] = static_cast<float>(VPi[i / 4][i % 4]);
    double eOld = 0, eNew = 0;
    const float pts[5][2] = {{0, 0}, {-0.9f, -0.9f}, {0.9f, 0.9f}, {-0.9f, 0.9f}, {0.5f, -0.3f}};
    for (const auto& pt : pts) {
        // exact ray: view-space point (x', y', 1) on the pixel's ray, rotated to world
        double vx = pt[0] * Pi[0][0] + pt[1] * Pi[1][0];
        double vy = pt[0] * Pi[0][1] + pt[1] * Pi[1][1];
        double ex[3], l = 0;
        for (int j = 0; j < 3; ++j) {
            ex[j] = vx * Vi[0][j] + vy * Vi[1][j] + Vi[2][j];
            l += ex[j] * ex[j];
        }
        l = std::sqrt(l);
        double r1[3], r2[3];
        ShaderRay(oldM, camRel, pt[0], pt[1], r1);
        ShaderRay(newM, camRel, pt[0], pt[1], r2);
        double d1 = 0, d2 = 0;
        for (int j = 0; j < 3; ++j) {
            d1 += (r1[j] - ex[j] / l) * (r1[j] - ex[j] / l);
            d2 += (r2[j] - ex[j] / l) * (r2[j] - ex[j] / l);
        }
        eOld = (std::max)(eOld, std::sqrt(d1));
        eNew = (std::max)(eNew, std::sqrt(d2));
    }
    g_dirErrOld = (std::max)(g_dirErrOld, eOld);
    g_dirErrNew = (std::max)(g_dirErrNew, eNew);
    if (g_pickOn.load(std::memory_order_relaxed))
        PickStore(b + 0x1C4, camRel, newM);        // context stays true; enlight's reader gets newM
    else
        memcpy(b + 0x1C4, newM, sizeof(newM));     // fallback: old behaviour (breaks scene distances)
    return true;
}

// Context for the cloud pass of a view at the carrier (the PLAT's, an extra camera's). When the view has its
// own origin (plat_origin.cpp, view mode: `swapped`) the cloud shader would mix it with cbCloudsParams.origin /
// cloudVolumeOffset, which still follow the frame origin (clouds vanish - run 2026-11-10 01:0x); the copy is
// first put back on the frame origin, which is exactly what DCS hands the clouds without the origin fix. Then
// the ray-direction fix. Null = pass DCS's context unchanged. Caller holds g_lock.
const void* CloudCtx(const void* ctx, int mode, bool swapped) {
    double fo[3];
    const bool own = PlatOriginViewFrameOrigin(fo) && swapped;
    if (mode == 1 && !own) return RebaseCtx(ctx);
    if (mode != 2 && !own) return nullptr;
    unsigned char* b = g_rebaseBuf[g_rebaseFlip = (g_rebaseFlip + 1) % kRebaseBufs];
    if (!SafeRead(ctx, b, kCtxSize)) return nullptr;
    if (own) {
        RebaseBufTo(b, fo);
        ++g_unrebased;
    }
    bool dir = mode == 2 && DirFixBuf(b);
    // With the substitution hook the copy is only needed when it was put back on the frame origin.
    if (g_pickOn.load(std::memory_order_relaxed)) return own ? b : nullptr;
    return own || dir ? b : nullptr;
}

uintptr_t SiteRva(void* ret) {
    auto r = reinterpret_cast<uintptr_t>(ret), b = reinterpret_cast<uintptr_t>(g_sr);
    return r >= b ? r - b : 0;
}

void HookAddPass(void* self, void* graph, void* binder, const void* ctx, void* t1, void* t2, void* t3, void* t4, void* t5,
                 void* deps, bool b1, bool b2, const float* v2, const float* v4) {
    void* ret = _ReturnAddress();
    const void* impl = nullptr;
    SafeReadPtr(self, &impl);
    float a2[2] = {}, a4[4] = {};
    if (v2) SafeRead(v2, a2, sizeof(a2));
    if (v4) SafeRead(v4, a4, sizeof(a4));
    AcquireSRWLockExclusive(&g_lock);
    Rec* r = nullptr;
    for (int i = 0; i < g_recN; ++i)
        if (g_rec[i].self == self && g_rec[i].impl == impl) r = &g_rec[i];
    if (!r) {
        if (g_recN < kMaxRenderers) {
            r = &g_rec[g_recN++];
            *r = Rec{};
            r->self = self;
            r->impl = impl;
        } else {
            ++g_overflow;
        }
    }
    if (r) {
        r->site = SiteRva(ret);
        r->tid = GetCurrentThreadId();
        ++r->calls;
        ++r->b1[b1 ? 1 : 0];
        ++r->b2[b2 ? 1 : 0];
        if (r->calls > 1 && (a2[0] != r->v2[0] || a2[1] != r->v2[1])) {
            ++r->v2Changes;
            int k = r->histN++ % kHist;
            r->v2Hist[k][0] = a2[0];
            r->v2Hist[k][1] = a2[1];
        }
        if (r->calls > 1 && memcmp(a4, r->v4, sizeof(a4)) != 0) ++r->v4Changes;
        memcpy(r->v2, a2, sizeof(a2));
        memcpy(r->v4, a4, sizeof(a4));
        if (r->dumpWanted && ctx) {
            r->dumpWanted = false;
            r->dumpReady = SafeRead(ctx, r->ctx, kCtxDump);
        }
    }
    if (ctx && g_traceOn && g_traceN < kTraceMax && strcmp(NameOf(impl), "LSOFRAME") == 0) {
        unsigned char c[0x320];
        if (SafeRead(ctx, c, sizeof(c))) {
            CamSample& cs = g_trace[g_traceN++];
            cs.t = QpcSeconds();
            memcpy(cs.cam, c + 0x308, sizeof(cs.cam));
            const float* V = reinterpret_cast<const float*>(c + 0x84);
            for (int i = 0; i < 3; ++i) cs.fwd[i] = V[i * 4 + 2];   // view z axis in world (column 2)
        }
    }
    const void* use = ctx;
    const int mode = g_rebase.load(std::memory_order_relaxed);
    // The PLAT's pass, an extra camera's ("mfd30"), and any other view that was given the carrier's origin.
    const char* passName = NameOf(impl);
    const bool platPass = strcmp(passName, "LSOFRAME") == 0;
    const bool camPass = !platPass && ExtraCamsTargetName(passName);
    const bool swappedPass = ctx && PlatOriginContextSwapped(ctx);
    if (ctx && (platPass || camPass || swappedPass)) {
        // (the PLAT's pass counts as swapped whenever a view was swapped this frame, as before the extra cameras,
        // and so does a camera's: every view at the carrier is swapped together, and putting a context that is
        // already on the frame origin back on it changes nothing)
        const void* rb = CloudCtx(ctx, mode, swappedPass || platPass || camPass);
        if (rb) {
            use = rb;
            ++g_rebased;
        } else {
            ++g_rebaseSkipped;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    g_origAdd(self, graph, binder, use, t1, t2, t3, t4, t5, deps, b1, b2, v2, v4);
}

void* HookCreate(void* self, void* ret, int w, int h, int msaa, const void* preset, const char* name) {
    void* site = _ReturnAddress();
    void* r = g_origCreate(self, ret, w, h, msaa, preset, name);
    const void* impl = nullptr;
    SafeReadPtr(ret, &impl);
    char nm[48] = {};
    if (name) SafeRead(name, nm, sizeof(nm) - 1);
    nm[sizeof(nm) - 1] = 0;
    AcquireSRWLockExclusive(&g_lock);
    ++g_createTotal;
    Created* c = nullptr;
    for (int i = 0; i < g_createdN; ++i)
        if (strcmp(g_created[i].name, nm) == 0 && g_created[i].w == w && g_created[i].h == h) c = &g_created[i];
    if (!c && g_createdN < kMaxRenderers) {
        c = &g_created[g_createdN++];
        *c = Created{};
        strcpy_s(c->name, nm);
        c->w = w;
        c->h = h;
    }
    if (c) {
        c->impl = impl;
        c->msaa = msaa;
        c->site = SiteRva(site);
        ++c->count;
    }
    ReleaseSRWLockExclusive(&g_lock);
    Log("CLOUDS createRenderer \"%s\" %dx%d msaa=%d -> impl %p (SceneRenderer+0x%llX)", nm, w, h, msaa, impl,
        static_cast<unsigned long long>(SiteRva(site)));
    return r;
}

const char* NameOf(const void* impl) {
    for (int i = 0; i < g_createdN; ++i)
        if (g_created[i].impl == impl) return g_created[i].name;
    return "?";
}

}  // namespace

std::string CloudsProbeInstall() {
    if (EngineDisabled()) return "engine calls disabled";
    if (g_addSlot) return "";
    g_sr = GetModuleHandleW(L"SceneRenderer.dll");
    if (!g_sr) return "SceneRenderer.dll not loaded";
    HMODULE en = GetModuleHandleW(L"enlight.dll");
    if (!en) return "enlight.dll not loaded";
    // The recorders read the scene context and the arguments at offsets taken from this build.
    if (!CompatOk()) return CompatWhy();
    void** add = FindIatSlot(g_sr, kEnlight, kAddPass);
    void** create = FindIatSlot(g_sr, kEnlight, kCreate);
    if (!add || !create) return "SceneRenderer.dll does not import the cloud renderer functions";
    void* realAdd = reinterpret_cast<void*>(GetProcAddress(en, kAddPass));
    void* realCreate = reinterpret_cast<void*>(GetProcAddress(en, kCreate));
    if (*add != realAdd || *create != realCreate) return "cloud renderer imports already redirected";
    g_origAdd = reinterpret_cast<AddPassFn>(realAdd);
    g_origCreate = reinterpret_cast<CreateFn>(realCreate);
    if (!PatchSlot(create, reinterpret_cast<void*>(&HookCreate))) return "VirtualProtect failed";
    g_createSlot = create;
    if (!PatchSlot(add, reinterpret_cast<void*>(&HookAddPass))) {
        PatchSlot(create, realCreate);
        g_createSlot = nullptr;
        return "VirtualProtect failed";
    }
    g_addSlot = add;
    Log("CLOUDS probe installed: SceneRenderer imports addRenderingPass / createRenderer -> recorders");
    std::string pick = PickInstall(en);
    Log("CLOUDS ray matrix substitution at enlight+0x21644: %s",
        pick.empty() ? "installed" : ("NOT installed (" + pick + "); falling back to the context matrix").c_str());
    return "";
}

void CloudsProbeRemove() {
    PickRemove();
    if (g_addSlot) PatchSlot(g_addSlot, reinterpret_cast<void*>(g_origAdd));
    if (g_createSlot) PatchSlot(g_createSlot, reinterpret_cast<void*>(g_origCreate));
    if (g_addSlot || g_createSlot) Log("CLOUDS probe removed");
    g_addSlot = g_createSlot = nullptr;
    AcquireSRWLockExclusive(&g_lock);
    g_recN = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

int CloudsRebaseMode() { return g_rebase.load(std::memory_order_relaxed); }

void CloudsRebase(int mode) {
    static const char* const kNames[] = {"OFF", "FULL (origin moved next to the PLAT camera; shifts the clouds)",
                                         "DIR (PLAT cloud ray directions rebuilt in double)"};
    if (mode < 0 || mode > 2) mode = 0;
    if (g_rebase.exchange(mode) != mode) Log("CLOUDS rebase %s", kNames[mode]);
}

double CloudsQpc() { return QpcSeconds(); }

void CloudsTrace(bool on) {
    AcquireSRWLockExclusive(&g_lock);
    g_traceOn = on;
    if (on) g_traceN = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

int CloudsTraceDump(const char* label) {
    static CamSample copy[kTraceMax];
    AcquireSRWLockExclusive(&g_lock);
    int n = g_traceN;
    memcpy(copy, g_trace, sizeof(CamSample) * n);
    g_traceOn = false;
    g_traceN = 0;
    ReleaseSRWLockExclusive(&g_lock);
    for (int i = 0; i < n; ++i)
        Log("CAMTRACE %s %.6f %.4f %.4f %.4f %.6f %.6f %.6f", label ? label : "", copy[i].t, copy[i].cam[0],
            copy[i].cam[1], copy[i].cam[2], copy[i].fwd[0], copy[i].fwd[1], copy[i].fwd[2]);
    return n;
}

std::string CloudsRebaseStats() {
    AcquireSRWLockExclusive(&g_lock);
    char b[300];
    static const char* const kModes[] = {"off", "full", "dir"};
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "rebase %s: rebased=%lld skipped=%lld dist=%.0f rayErr(DCS)=%.2f mrad rayErr(fixed)=%.4f mrad | "
                "ray matrix hook %s calls=%lld substituted=%lld",
                kModes[g_rebase.load()], g_rebased, g_rebaseSkipped, g_rebaseDist, g_dirErrOld * 1000,
                g_dirErrNew * 1000, g_pickOn.load() ? "on" : "OFF", InterlockedExchange64(&g_pickCalls, 0),
                InterlockedExchange64(&g_pickHits, 0));
    g_rebased = g_rebaseSkipped = 0;
    g_dirErrOld = g_dirErrNew = 0;
    ReleaseSRWLockExclusive(&g_lock);
    // Measurement for the "PLAT clouds differ when far away" question: enlight's cloud volume. The
    // renderer object is taken from the global at enlight+0xA08D8 (assumed to be the one the pass
    // function uses): volume centre Vec3d +0x20E0, cbuffer radius +0x177C, scale +0x1780, source +0x1B2C.
    std::string out = b;
    static int dumpTick = 0;
    if (++dumpTick % 6 == 1) DumpCloudCb();   // every 30 s
    if (HMODULE en = GetModuleHandleW(L"enlight.dll")) {
        const void* selfp = nullptr;
        double c[3];
        float f[3];
        SafeReadPtr(reinterpret_cast<const unsigned char*>(en) + 0xA08D8, &selfp);
        const unsigned char* self = static_cast<const unsigned char*>(selfp);
        if (self &&
            SafeRead(self + 0x20E0, c, sizeof(c)) && SafeRead(self + 0x177C, f, 8) &&
            SafeRead(self + 0x1B2C, f + 2, 4)) {
            char v[200];
            _snprintf_s(v, _countof(v), _TRUNCATE,
                        " | cloud volume centre=(%.0f, %.0f, %.0f) radius=%.4g scale=%.4g src=%.4g", c[0], c[1],
                        c[2], f[0], f[1], f[2]);
            out += v;
        }
        // Second-volume feasibility (FINDINGS 2026-11-10): density map dimensions (+0x21B8 w, +0x21BC h,
        // +0x21C0 d) with their neighbours, and whether the 0x2F850-byte side object (+0xA08E8) exists.
        int dims[16] = {};
        const void* side = nullptr;
        if (self && dumpTick % 6 == 1 && SafeRead(self + 0x21A8, dims, sizeof(dims))) {
            SafeReadPtr(reinterpret_cast<const unsigned char*>(en) + 0xA08E8, &side);
            std::string d = "CLOUDIMPL self=";
            char t[64];
            _snprintf_s(t, _countof(t), _TRUNCATE, "%p side=%p ints@+0x21A8:", selfp, side);
            d += t;
            for (int v : dims) {
                _snprintf_s(t, _countof(t), _TRUNCATE, " %d", v);
                d += t;
            }
            Log("%s", d.c_str());
        }
    }
    return out;
}

std::string CloudsProbeStats(bool dump) {
    if (!g_addSlot) return "not installed";
    std::string out;
    char b[512];
    struct Dump {
        int idx;
        unsigned char ctx[kCtxDump];
    };
    static Dump dumps[kMaxRenderers];
    int dumpN = 0;
    AcquireSRWLockExclusive(&g_lock);
    _snprintf_s(b, _countof(b), _TRUNCATE, "renderers=%d created=%lld overflow=%lld", g_recN, g_createTotal, g_overflow);
    out = b;
    for (int i = 0; i < g_recN; ++i) {
        Rec& r = g_rec[i];
        _snprintf_s(b, _countof(b), _TRUNCATE,
                    " || #%d \"%s\" self=%p impl=%p site=+0x%llX tid=%lu calls=%lld b1[0:%lld 1:%lld] b2[0:%lld 1:%lld] "
                    "v2=(%.4g,%.4g) v2chg=%lld v4=(%.4g,%.4g,%.4g,%.4g) v4chg=%lld",
                    i, NameOf(r.impl), r.self, r.impl, static_cast<unsigned long long>(r.site), r.tid, r.calls, r.b1[0],
                    r.b1[1], r.b2[0], r.b2[1], r.v2[0], r.v2[1], r.v2Changes, r.v4[0], r.v4[1], r.v4[2], r.v4[3],
                    r.v4Changes);
        out += b;
        if (r.histN > 0) {
            out += " v2hist";
            int n = r.histN < kHist ? r.histN : kHist;
            for (int k = 0; k < n; ++k) {
                _snprintf_s(b, _countof(b), _TRUNCATE, " (%.4g,%.4g)", r.v2Hist[k][0], r.v2Hist[k][1]);
                out += b;
            }
        }
        if (r.dumpReady && dumpN < kMaxRenderers) {
            dumps[dumpN].idx = i;
            memcpy(dumps[dumpN].ctx, r.ctx, kCtxDump);
            ++dumpN;
            r.dumpReady = false;
        }
        if (dump) r.dumpWanted = true;
        // reset per-interval counters
        r.calls = r.b1[0] = r.b1[1] = r.b2[0] = r.b2[1] = r.v2Changes = r.v4Changes = 0;
        r.histN = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);
    // Context dumps (from the previous request) go to the log only: 16 dwords per line, as hex.
    for (int d = 0; d < dumpN; ++d) {
        Log("CLOUDS ctx dump renderer #%d (%zu bytes):", dumps[d].idx, kCtxDump);
        const uint32_t* w = reinterpret_cast<const uint32_t*>(dumps[d].ctx);
        for (size_t off = 0; off < kCtxDump / 4; off += 16) {
            char line[256];
            int n = _snprintf_s(line, _countof(line), _TRUNCATE, "CLOUDS ctx#%d +%03zX:", dumps[d].idx, off * 4);
            for (size_t k = 0; k < 16 && n > 0; ++k)
                n += _snprintf_s(line + n, _countof(line) - n, _TRUNCATE, " %08X", w[off + k]);
            Log("%s", line);
        }
    }
    return out;
}

}  // namespace lsodiag
