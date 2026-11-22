#include "night_sky.h"

#include "common.h"
#include "compat.h"
#include "engine.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lsodiag {
namespace {

// SpaceRenderer.dll 2.9.31.29434 (same file in bin and bin-mt). See night_sky.h and docs/FINDINGS.md.
constexpr uintptr_t kVtblSlotRva = 0xC500;   // celestial bodies renderable vtable +0xC4F8, slot 1
constexpr uintptr_t kRenderRva = 0x32A0;     // its per-view call: (this, SceneRenderContext*)
constexpr uintptr_t kStarDrawRva = 0x87D0;   // StarMaterial::draw(const StarObject&), exported
constexpr uintptr_t kCallStarsRva = 0x6931;  // SpaceRendererInstance::draw: the stars
constexpr uintptr_t kCallLayerRva = 0x69A5;  // ... the second star layer (drawn when instance +0x90 is set)
constexpr const char* kStarDrawName = "?draw@StarMaterial@Graphics@@QEAAXAEBUStarObject@2@@Z";
// mov rax,rsp / save rbx rbp rsi rdi / push r14 / sub rsp,80h / mov rdi,rcx / mov rbx,rdx / mov rcx,[rcx+10h] /
// test rcx,rcx / je (global instance path)
constexpr unsigned char kRenderHead[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48,
                                         0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x56, 0x48, 0x81, 0xEC,
                                         0x80, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0xDA, 0x48, 0x8B,
                                         0x49, 0x10, 0x48, 0x85, 0xC9, 0x0F, 0x84, 0x85, 0x00, 0x00, 0x00};
constexpr unsigned char kStarDrawHead[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57,
                                           0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D,
                                           0x68, 0xA1, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00};
// SceneRenderContext (clouds_probe.cpp): +0x104 P (row-vector; reverse Z: z_ndc = P22 + P32 / z_view),
// +0x308 camera (Vec3d).
constexpr size_t kCtxP = 0x104, kCtxCam = 0x308;
// StarObject (built on the stack by SpaceRendererInstance::draw; StarMaterial::draw reads +0x00 .. +0x5F):
// +0x04 world matrix (rows 0-2 = rotation x scale, row 3 = camera - origin). StarMaterial: +0x08 / +0x10
// begin / end of the star vertices (20 bytes: float3 position, float2 magnitude / colour).
constexpr size_t kStarObjectSize = 0x60, kObjMatrix = 0x04, kMatVerts = 0x08, kVertexSize = 20;
constexpr size_t kInstance = 0x10;           // renderable +0x10: the provider's instance, null = global path
// StarMaterial (operator new(0x138), +0x57F7): +0x00 enabled byte, +0x08 / +0x10 the vertex list (draw draws
// (end - begin) / 20 vertices, +0x8BB8), +0x20 the effect (every effect call in draw is a virtual call on it),
// +0x128 the pixelSize parameter handle (set through effect vtable +0xA8 from 1 / render target size, +0x8A4D).
constexpr size_t kMaterialSize = 0x138, kMatEnd = 0x10, kMatEffect = 0x20, kMatPixelSize = 0x128;
constexpr size_t kVertexNorm = 0x0C;         // vertex float: normalized magnitude (1 = brightest)
// StarObject +0x58: gives the milky way's opacity clamp((-v - 0.05) x 10, 0, 1) (+0x8AA9); +0x5C: the view's
// P11, uScale = pow(1 / v, 0.8) (+0x88D3), and Star.fx divides every star's alpha by uScale.
constexpr size_t kObjMilky = 0x58, kObjZoom = 0x5C;
constexpr double kUScalePow = 0.8;
constexpr size_t kMaxVerts = 1000000;
// The effect proxy: a forwarding vtable (every slot: this = the real effect, jump to its slot) with slot
// +0xA8 (set a vector parameter: this, handle, const float*) scaling pixelSize on the way.
// StarMaterial::draw calls the effect up to slot 0x128 / 8 = 37 (and 0x1B8 / 8 = 55 on another object); 32 slots
// crashed DCS on the first faded draw (1.1.0.1, 2026-11-20: call [rax+128h] at +0x8B61 read past the vtable).
// night_sky_test scans the real function and checks every call slot is below this.
constexpr int kProxySlots = kStarEffectSlots;
constexpr size_t kProxyVtBytes = kProxySlots * 8, kProxyThunk = 0x40,
                 kProxyBytes = (kProxyVtBytes + kProxySlots * kProxyThunk + 4095) & ~size_t(4095);
constexpr int kPixelSizeSlot = 0xA8 / 8;
// cmp rdx,[rcx+10h] / jne fwd / movss xmm4,[r8+4] / movaps xmm5,xmm4 / mulss xmm4,[rcx+18h] /
// mulss xmm5,[rcx+1Ch] / movss [rcx+20h],xmm4 / movss [rcx+24h],xmm5 / lea r8,[rcx+20h] / fwd:
constexpr unsigned char kPixelThunk[] = {0x48, 0x3B, 0x51, 0x10, 0x75, 0x21, 0xF3, 0x41, 0x0F, 0x10, 0x60, 0x04,
                                         0x0F, 0x28, 0xEC, 0xF3, 0x0F, 0x59, 0x61, 0x18, 0xF3, 0x0F, 0x59, 0x69,
                                         0x1C, 0xF3, 0x0F, 0x11, 0x61, 0x20, 0xF3, 0x0F, 0x11, 0x69, 0x24, 0x4C,
                                         0x8D, 0x41, 0x20};
// mov rcx,[rcx+8] / mov rax,[rcx] / jmp [rax+disp32]
constexpr unsigned char kForwardThunk[] = {0x48, 0x8B, 0x49, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0xA0};

struct EffectProxy {
    const void* vtbl;   // +0x00 the proxy vtable
    void* real;         // +0x08 DCS's effect
    uint64_t handle;    // +0x10 the pixelSize handle
    float fx, fy;       // +0x18 / +0x1C: pixelSize becomes (y x fx, y x fy)
    float out[2];       // +0x20
};
static_assert(offsetof(EffectProxy, real) == 0x08 && offsetof(EffectProxy, handle) == 0x10 &&
                  offsetof(EffectProxy, fx) == 0x18 && offsetof(EffectProxy, fy) == 0x1C &&
                  offsetof(EffectProxy, out) == 0x20,
              "kPixelThunk offsets");

constexpr int kMaxViews = 16;   // ILARTS views per build (PLAT + 6 cameras)
constexpr int kGenerations = 4; // builds remembered: a view may render after the next build has started
constexpr double kMatchM = 0.05;             // context camera within this of a listed view camera
constexpr double kNearM = 50.0;              // ... a "near miss" when only within this (diagnostic)

using RenderFn = void (*)(void* self, const void* ctx, void* a3, void* a4);
using StarDrawFn = void (*)(void* material, const void* starObject);

struct Views {
    int n = 0;
    double cam[kMaxViews][3] = {};
    bool plat[kMaxViews] = {};
    bool haveOrigin = false;   // the views at the carrier had their own origin in this build (far view fix)
    float origin[3] = {};
};

bool g_installed = false;
volatile long g_mode = static_cast<long>(NightSkyMode::Hide);
RenderFn g_render = nullptr;
StarDrawFn g_starDraw = nullptr;
void** g_vtblSlot = nullptr;
unsigned char* g_calls[2] = {};
unsigned char g_callOrig[2][5] = {};
unsigned char* g_stub = nullptr;
unsigned char* g_proxyCode = nullptr;   // the proxy vtable (kProxySlots pointers), then its thunks

SRWLOCK g_viewLock = SRWLOCK_INIT;
Views g_pending, g_gen[kGenerations];

// Per thread, set while the renderable's call runs: 1 = one of ILARTS's views, 2 = DCS's own; whether it is
// the PLAT's; the view's far plane and projection scale, and whether the provider has its own instance.
thread_local int t_kind = 0;
thread_local bool t_plat = false;
thread_local double t_far = 0, t_p00 = 0, t_p11 = 0;
thread_local int t_ownInstance = 0;

// Counters since the last stats line.
volatile long long g_rendersOurs = 0, g_rendersDcs = 0, g_nearMiss = 0;
volatile long long g_drawsHidden = 0, g_drawsScaled = 0, g_drawsOurs = 0, g_drawsDcs = 0, g_scaleRefused = 0;
volatile long long g_drawsFaded = 0, g_drawsPoints = 0;
// The last star draw in one of ILARTS's views (stats line): field of view, limiting magnitude, gain, stars
// drawn of the list (-1 = the whole list).
volatile double g_lastHfov = 0, g_lastMag = 0, g_lastGain = 0;
volatile long long g_lastCount = -1, g_lastTotal = 0;

// The star lists seen (per material), for the limiting magnitude: the normalized magnitudes when the list is
// sorted brightest first. A list is identified by its material, its bounds and its first and last values.
struct VertList {
    const void* mat = nullptr;
    uintptr_t begin = 0, end = 0;
    uint32_t first = 0, last = 0;
    bool sorted = false;
    std::vector<float> norm;
};
SRWLOCK g_listLock = SRWLOCK_INIT;
VertList g_lists[4];
int g_listNext = 0;
constexpr int kMaxListLines = 8;
volatile long g_listLines = 0;

// Star geometry last seen per kind (0 = ILARTS's views, 1 = DCS's), for the stats line; and the geometry
// last written as a NIGHTSKY line, so a change gets its own line (at most kMaxGeomLines).
struct Geom {
    double farM = 0, radius = 0, p00 = 0, p11 = 0;
    int ownInstance = 0;
    bool have = false;
};
SRWLOCK g_geomLock = SRWLOCK_INIT;
Geom g_geom[2], g_geomLogged[2];
constexpr int kMaxGeomLines = 12;
volatile long g_geomLines = 0;

bool Near(const double* a, const double* b, double m) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz < m * m;
}

// 1 = an ILARTS view's camera (*plat: the PLAT's), 0 = not, and *nearMiss when it is only near one.
int Ours(const double* cam, bool* nearMiss, bool* plat) {
    int ours = 0;
    bool nearHit = false;
    *plat = false;
    AcquireSRWLockShared(&g_viewLock);
    for (int g = 0; g < kGenerations && !ours; ++g)
        for (int i = 0; i < g_gen[g].n && !ours; ++i) {
            if (Near(cam, g_gen[g].cam[i], kMatchM)) {
                ours = 1;
                *plat = g_gen[g].plat[i];
            } else if (Near(cam, g_gen[g].cam[i], kNearM)) {
                nearHit = true;
            }
        }
    ReleaseSRWLockShared(&g_viewLock);
    *nearMiss = !ours && nearHit;
    return ours;
}

// Builds the effect proxy's vtable and thunks (once; the page stays allocated).
bool MakeProxyCode() {
    if (g_proxyCode) return true;
    auto mem = static_cast<unsigned char*>(VirtualAlloc(nullptr, kProxyBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!mem) return false;
    auto vt = reinterpret_cast<unsigned char**>(mem);
    for (int s = 0; s < kProxySlots; ++s) {
        unsigned char* t = mem + kProxyVtBytes + s * kProxyThunk;
        unsigned char* p = t;
        if (s == kPixelSizeSlot) {
            memcpy(p, kPixelThunk, sizeof(kPixelThunk));
            p += sizeof(kPixelThunk);
        }
        memcpy(p, kForwardThunk, sizeof(kForwardThunk));
        p += sizeof(kForwardThunk);
        const int32_t disp = s * 8;
        memcpy(p, &disp, 4);
        p += 4;
        *p = 0xCC;
        vt[s] = t;
    }
    DWORD old = 0;
    if (!VirtualProtect(mem, kProxyBytes, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, kProxyBytes);
    g_proxyCode = mem;
    return true;
}

// How many vertices of the material's list [begin, end) have a normalized magnitude of at least nLim, when the
// list is sorted brightest first; -1 when it is not (or cannot be read): then the whole list is drawn.
long long CountBright(const void* material, uintptr_t begin, uintptr_t end, double nLim, long long* total) {
    *total = 0;
    if (end <= begin || (end - begin) % kVertexSize) return -1;
    const size_t n = (end - begin) / kVertexSize;
    *total = static_cast<long long>(n);
    if (n > kMaxVerts) return -1;
    uint32_t first = 0, last = 0;
    if (!SafeRead(reinterpret_cast<const void*>(begin + kVertexNorm), &first, 4) ||
        !SafeRead(reinterpret_cast<const void*>(end - kVertexSize + kVertexNorm), &last, 4))
        return -1;
    auto count = [nLim](const VertList& v) -> long long {
        if (!v.sorted) return -1;
        const auto it = std::partition_point(v.norm.begin(), v.norm.end(), [nLim](float x) { return x >= nLim; });
        return static_cast<long long>(it - v.norm.begin());
    };
    long long r = -1;
    bool found = false;
    AcquireSRWLockShared(&g_listLock);
    for (const VertList& v : g_lists)
        if (v.mat == material && v.begin == begin && v.end == end && v.first == first && v.last == last) {
            r = count(v);
            found = true;
            break;
        }
    ReleaseSRWLockShared(&g_listLock);
    if (found) return r;
    // First sight of this list: read it whole and check the order.
    VertList v;
    v.mat = material;
    v.begin = begin;
    v.end = end;
    v.first = first;
    v.last = last;
    std::vector<unsigned char> raw(n * kVertexSize);
    v.sorted = SafeRead(reinterpret_cast<const void*>(begin), raw.data(), raw.size());
    if (v.sorted) {
        v.norm.resize(n);
        for (size_t i = 0; i < n && v.sorted; ++i) {
            float x = 0;
            memcpy(&x, raw.data() + i * kVertexSize + kVertexNorm, 4);
            v.norm[i] = x;
            if (!(x >= 0 && x <= 1) || (i > 0 && x > v.norm[i - 1])) v.sorted = false;
        }
    }
    if (!v.sorted) v.norm.clear();
    r = count(v);
    if (InterlockedIncrement(&g_listLines) <= kMaxListLines) {
        if (v.sorted)
            Log("NIGHTSKY star list %p: %zu stars, brightest first (normalized magnitude %.3f .. %.3f): the zoom sets a "
                "limiting magnitude",
                material, n, v.norm.front(), v.norm.back());
        else
            Log("NIGHTSKY star list %p: %zu stars, not sorted by brightness: the zoom fades them by brightness only",
                material, n);
    }
    AcquireSRWLockExclusive(&g_listLock);
    g_lists[g_listNext] = std::move(v);
    g_listNext = (g_listNext + 1) % static_cast<int>(_countof(g_lists));
    ReleaseSRWLockExclusive(&g_listLock);
    return r;
}

enum class Drew { Yes, Nothing, Refused };

// One of ILARTS's views' star layers: the sphere at half the far plane, the stars down to look.magLimit at
// look.gain, square points on screen. Copies only: DCS's material, star object and effect are not written.
Drew DrawOurs(void* material, const void* starObject, const NightSkyLook& look) {
    alignas(16) unsigned char obj[kStarObjectSize];
    const void* verts = nullptr;
    float v0[3];
    if (!(SafeRead(starObject, obj, sizeof(obj)) &&
          SafeReadPtr(static_cast<const unsigned char*>(material) + kMatVerts, &verts) && verts &&
          SafeRead(verts, v0, sizeof(v0)) && t_far > 100 && t_far < 1e7))
        return Drew::Refused;
    // The sphere at half the far plane (DCS's rule for a view with its own instance).
    float* m = reinterpret_cast<float*>(obj + kObjMatrix);
    const double scale = std::sqrt(double(m[0]) * m[0] + double(m[1]) * m[1] + double(m[2]) * m[2]);
    const double len = std::sqrt(double(v0[0]) * v0[0] + double(v0[1]) * v0[1] + double(v0[2]) * v0[2]);
    const double radius = scale * len;
    if (!(radius > 1) || !std::isfinite(radius)) return Drew::Refused;
    const double scaleBy = 0.5 * t_far / radius;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m[r * 4 + c] = static_cast<float>(m[r * 4 + c] * scaleBy);
    if (!(look.gain > 0)) return Drew::Nothing;
    bool faded = false;
    if (look.gain < 1) {
        // alpha x gain: uScale = pow(1 / z, 0.8) -> z x gain^(1 / 0.8)
        float z = 0;
        memcpy(&z, obj + kObjZoom, 4);
        if (z > 0 && std::isfinite(z)) {
            z = static_cast<float>(z * std::pow(look.gain, 1 / kUScalePow));
            memcpy(obj + kObjZoom, &z, 4);
            faded = true;
        }
        // the milky way's opacity x gain
        float mw = 0;
        memcpy(&mw, obj + kObjMilky, 4);
        const double op = std::clamp((-double(mw) - 0.05) * 10, 0.0, 1.0);
        if (op > 0) {
            mw = static_cast<float>(-(op * look.gain / 10 + 0.05));
            memcpy(obj + kObjMilky, &mw, 4);
        }
    }
    alignas(16) unsigned char mat[kMaterialSize];
    EffectProxy proxy{};
    void* drawMat = material;
    long long count = -1, total = 0;
    bool points = false;
    if (SafeRead(material, mat, sizeof(mat))) {
        uintptr_t b = 0, e = 0;
        memcpy(&b, mat + kMatVerts, 8);
        memcpy(&e, mat + kMatEnd, 8);
        if (look.magLimit < kStarsFullMag) {
            count = CountBright(material, b, e, NightSkyNormMag(look.magLimit), &total);
            if (count == 0) {
                g_lastCount = 0;
                g_lastTotal = total;
                return Drew::Nothing;
            }
            if (count > 0 && count < total) {
                const uintptr_t e2 = b + static_cast<uintptr_t>(count) * kVertexSize;
                memcpy(mat + kMatEnd, &e2, 8);
                faded = true;
            }
        }
        void* fx = nullptr;
        memcpy(&fx, mat + kMatEffect, 8);
        if (fx && g_proxyCode && t_p00 > 0 && t_p11 > 0 && std::isfinite(t_p00 / t_p11)) {
            proxy.vtbl = g_proxyCode;
            proxy.real = fx;
            memcpy(&proxy.handle, mat + kMatPixelSize, 8);
            // square on screen: the picture is shown at the projection's aspect (P11 / P00)
            proxy.fx = static_cast<float>(t_p00 / t_p11);
            proxy.fy = 1;
            void* pp = &proxy;
            memcpy(mat + kMatEffect, &pp, 8);
            points = true;
        }
        drawMat = mat;
    }
    if (faded) InterlockedIncrement64(&g_drawsFaded);
    if (points) InterlockedIncrement64(&g_drawsPoints);
    g_lastMag = look.magLimit;
    g_lastGain = look.gain;
    g_lastCount = count;
    g_lastTotal = total;
    g_starDraw(drawMat, obj);
    return Drew::Yes;
}

std::string GeomText(const Geom& g) {
    if (!g.have) return "not seen";
    double corner = 0;
    const double ring = NightSkyRingDeg(g.farM, g.radius, g.p00, g.p11, &corner);
    char b[256];
    if (ring > 0)
        _snprintf_s(b, _countof(b), _TRUNCATE,
                    "far %.0f m, stars at %.0f m (%s) -> beyond the far plane within %.1f deg of the axis, drawn only outside it "
                    "(picture corner %.1f deg)",
                    g.farM, g.radius, g.ownInstance ? "own instance" : "global instance", ring, corner);
    else
        _snprintf_s(b, _countof(b), _TRUNCATE, "far %.0f m, stars at %.0f m (%s) -> all in front of the far plane", g.farM,
                    g.radius, g.ownInstance ? "own instance" : "global instance");
    return b;
}

// Called for the first draw of a view kind after the stats line asked for a sample: where the stars are.
void Sample(int kind, const void* material, const unsigned char* obj) {
    float row0[3];
    const void* verts = nullptr;
    float v0[3];
    if (!SafeRead(obj + kObjMatrix, row0, sizeof(row0)) ||
        !SafeReadPtr(static_cast<const unsigned char*>(material) + kMatVerts, &verts) || !verts ||
        !SafeRead(verts, v0, sizeof(v0)))
        return;
    const double scale = std::sqrt(double(row0[0]) * row0[0] + double(row0[1]) * row0[1] + double(row0[2]) * row0[2]);
    const double len = std::sqrt(double(v0[0]) * v0[0] + double(v0[1]) * v0[1] + double(v0[2]) * v0[2]);
    Geom g;
    g.farM = t_far;
    g.radius = scale * len;
    g.p00 = t_p00;
    g.p11 = t_p11;
    g.ownInstance = t_ownInstance;
    g.have = true;
    bool line = false;
    Geom logged;
    AcquireSRWLockExclusive(&g_geomLock);
    g_geom[kind] = g;
    logged = g_geomLogged[kind];
    if (!logged.have || std::fabs(logged.farM - g.farM) > 1 || std::fabs(logged.radius - g.radius) > 0.001 * g.radius + 1 ||
        std::fabs(logged.p11 - g.p11) > 0.05 * std::fabs(logged.p11) || logged.ownInstance != g.ownInstance) {
        g_geomLogged[kind] = g;
        line = true;
    }
    ReleaseSRWLockExclusive(&g_geomLock);
    if (line && InterlockedIncrement(&g_geomLines) <= kMaxGeomLines)
        Log("NIGHTSKY %s view: %s", kind == 0 ? "ILARTS" : "DCS", GeomText(g).c_str());
}

volatile long g_wantSample[2] = {1, 1};

void StarWrap(void* material, const void* starObject) {
    const int kind = t_kind;
    if (kind == 0) {   // not inside the renderable's call (another caller of SpaceRendererInstance::draw)
        g_starDraw(material, starObject);
        return;
    }
    const int k = kind == 1 ? 0 : 1;
    if (g_wantSample[k] && InterlockedExchange(&g_wantSample[k], 0))
        Sample(k, material, static_cast<const unsigned char*>(starObject));
    if (kind != 1) {
        InterlockedIncrement64(&g_drawsDcs);
        g_starDraw(material, starObject);
        return;
    }
    InterlockedIncrement64(&g_drawsOurs);
    const auto mode = static_cast<NightSkyMode>(g_mode);
    if (mode == NightSkyMode::Stock) {
        g_starDraw(material, starObject);
        return;
    }
    NightSkyLook look;   // hide, and auto in the PLAT: nothing
    const double hfov = NightSkyHfovDeg(t_p00);
    if (mode == NightSkyMode::Full) {
        look.magLimit = kStarsFullMag;
        look.gain = 1;
    } else if (mode == NightSkyMode::Auto && !t_plat) {
        look = NightSkyAutoLook(hfov);
    }
    if (look.gain > 0) g_lastHfov = hfov;
    if (!(look.gain > 0)) {
        InterlockedIncrement64(&g_drawsHidden);
        return;
    }
    switch (DrawOurs(material, starObject, look)) {
    case Drew::Yes:
        InterlockedIncrement64(&g_drawsScaled);
        return;
    case Drew::Nothing:
        InterlockedIncrement64(&g_drawsHidden);
        return;
    case Drew::Refused:
        break;
    }
    InterlockedIncrement64(&g_scaleRefused);
    g_starDraw(material, starObject);
}

void RenderWrap(void* self, const void* ctx, void* a3, void* a4) {
    int kind = 2;
    double farM = 0, p00 = 0, p11 = 0;
    int own = 0;
    double cam[3];
    float P[16];
    bool plat = false;
    if (ctx && SafeRead(static_cast<const unsigned char*>(ctx) + kCtxCam, cam, sizeof(cam))) {
        bool nearMiss = false;
        if (Ours(cam, &nearMiss, &plat)) kind = 1;
        if (nearMiss) InterlockedIncrement64(&g_nearMiss);
    }
    if (ctx && SafeRead(static_cast<const unsigned char*>(ctx) + kCtxP, P, sizeof(P)) && P[10] != 0) {
        farM = -double(P[14]) / P[10];
        p00 = P[0];
        p11 = P[5];
    }
    const void* inst = nullptr;
    if (SafeReadPtr(static_cast<const unsigned char*>(self) + kInstance, &inst)) own = inst ? 1 : 0;
    InterlockedIncrement64(kind == 1 ? &g_rendersOurs : &g_rendersDcs);
    const int prevKind = t_kind;
    const bool prevPlat = t_plat;
    const double prevFar = t_far, prev00 = t_p00, prev11 = t_p11;
    const int prevOwn = t_ownInstance;
    t_kind = kind;
    t_plat = kind == 1 && plat;
    t_far = farM;
    t_p00 = p00;
    t_p11 = p11;
    t_ownInstance = own;
    g_render(self, ctx, a3, a4);
    t_kind = prevKind;
    t_plat = prevPlat;
    t_far = prevFar;
    t_p00 = prev00;
    t_p11 = prev11;
    t_ownInstance = prevOwn;
}

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

// Replaces the 5 bytes at `at` (inside one 16-byte aligned block) atomically.
bool Write5(unsigned char* at, const unsigned char* expect, const unsigned char* bytes) {
    auto block = reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(at) & ~static_cast<uintptr_t>(15));
    const size_t off = static_cast<size_t>(at - block);
    if (off + 5 > 16) return false;
    DWORD old = 0;
    if (!VirtualProtect(block, 16, PAGE_EXECUTE_READWRITE, &old)) return false;
    alignas(16) long long cur[2];
    memcpy(cur, block, 16);
    bool ok = memcmp(reinterpret_cast<unsigned char*>(cur) + off, expect, 5) == 0;
    if (ok) {
        alignas(16) long long want[2];
        memcpy(want, cur, 16);
        memcpy(reinterpret_cast<unsigned char*>(want) + off, bytes, 5);
        alignas(16) long long cmp[2] = {cur[0], cur[1]};
        ok = InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(block), want[1], want[0], cmp) != 0;
    }
    DWORD tmp = 0;
    VirtualProtect(block, 16, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), block, 16);
    return ok;
}

bool WritePtr(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    return true;
}

bool CallsTo(const unsigned char* site, const unsigned char* target, unsigned char out[5]) {
    if (!SafeRead(site, out, 5) || out[0] != 0xE8) return false;
    int32_t rel = 0;
    memcpy(&rel, out + 1, 4);
    return site + 5 + rel == target;
}

}  // namespace

double NightSkyRingDeg(double farM, double starRadius, double p00, double p11, double* cornerDeg) {
    constexpr double kDeg = 57.29577951308232;
    if (cornerDeg) {
        *cornerDeg = 0;
        if (p00 > 0 && p11 > 0) *cornerDeg = std::atan(std::sqrt(1 / (p00 * p00) + 1 / (p11 * p11))) * kDeg;
    }
    if (!(farM > 0) || !(starRadius > farM)) return 0;
    return std::acos(farM / starRadius) * kDeg;
}

void NightSkyNoteView(const double cam[3], bool plat) {
    if (!cam) return;
    AcquireSRWLockExclusive(&g_viewLock);
    bool known = false;
    for (int j = 0; j < g_pending.n && !known; ++j)
        if (g_pending.cam[j][0] == cam[0] && g_pending.cam[j][1] == cam[1] && g_pending.cam[j][2] == cam[2]) {
            known = true;
            g_pending.plat[j] = g_pending.plat[j] && plat;   // also listed as a camera: a camera's
        }
    if (!known && g_pending.n < kMaxViews) {
        for (int i = 0; i < 3; ++i) g_pending.cam[g_pending.n][i] = cam[i];
        g_pending.plat[g_pending.n] = plat;
        ++g_pending.n;
    }
    ReleaseSRWLockExclusive(&g_viewLock);
}

void NightSkyNoteOrigin(const float origin[3]) {
    if (!origin) return;
    AcquireSRWLockExclusive(&g_viewLock);
    for (int i = 0; i < 3; ++i) g_pending.origin[i] = origin[i];
    g_pending.haveOrigin = true;
    ReleaseSRWLockExclusive(&g_viewLock);
}

void NightSkyBuildEnd() {
    AcquireSRWLockExclusive(&g_viewLock);
    for (int g = kGenerations - 1; g > 0; --g) g_gen[g] = g_gen[g - 1];
    g_gen[0] = g_pending;
    g_pending.n = 0;
    g_pending.haveOrigin = false;
    ReleaseSRWLockExclusive(&g_viewLock);
}

bool NightSkyCarrierOrigin(const float origin[3]) {
    if (!origin) return false;
    bool hit = false;
    AcquireSRWLockShared(&g_viewLock);
    for (int g = 0; g < kGenerations && !hit; ++g)
        hit = g_gen[g].haveOrigin && memcmp(g_gen[g].origin, origin, sizeof(g_gen[g].origin)) == 0;
    ReleaseSRWLockShared(&g_viewLock);
    return hit;
}

int NightSkyOurView(const double cam[3], bool* plat, bool* nearMiss) {
    bool p = false, nm = false;
    const int ours = cam ? Ours(cam, &nm, &p) : 0;
    if (plat) *plat = p;
    if (nearMiss) *nearMiss = nm;
    return ours;
}

std::string NightSkyInstall() {
    if (EngineDisabled()) return "engine calls disabled";
    if (g_installed) return "";
    HMODULE sr = GetModuleHandleW(L"SpaceRenderer.dll");
    if (!sr) return "SpaceRenderer.dll not loaded";
    if (!CompatOk()) return CompatWhy();
    auto base = reinterpret_cast<unsigned char*>(sr);
    unsigned char head[sizeof(kRenderHead)];
    if (!SafeRead(base + kRenderRva, head, sizeof(head)) || memcmp(head, kRenderHead, sizeof(head)) != 0)
        return "SpaceRenderer+0x32A0 is not the expected code";
    unsigned char shead[sizeof(kStarDrawHead)];
    if (!SafeRead(base + kStarDrawRva, shead, sizeof(shead)) || memcmp(shead, kStarDrawHead, sizeof(shead)) != 0)
        return "SpaceRenderer+0x87D0 is not the expected code";
    if (reinterpret_cast<unsigned char*>(GetProcAddress(sr, kStarDrawName)) != base + kStarDrawRva)
        return "StarMaterial::draw is not at SpaceRenderer+0x87D0";
    void** slot = reinterpret_cast<void**>(base + kVtblSlotRva);
    void* cur = nullptr;
    if (!SafeRead(slot, &cur, sizeof(cur)) || cur != base + kRenderRva) return "the renderable vtable slot is not +0x32A0";
    const uintptr_t sites[2] = {kCallStarsRva, kCallLayerRva};
    unsigned char orig[2][5];
    for (int i = 0; i < 2; ++i)
        if (!CallsTo(base + sites[i], base + kStarDrawRva, orig[i])) return "a StarMaterial::draw call is not as expected";
    g_render = reinterpret_cast<RenderFn>(base + kRenderRva);
    g_starDraw = reinterpret_cast<StarDrawFn>(base + kStarDrawRva);
    // the effect proxy (square points); without it the stars are drawn at DCS's size
    if (!MakeProxyCode()) Log("NIGHTSKY no memory for the effect proxy: stars at DCS's size");
    if (!g_stub) {
        unsigned char* mem = AllocNear(base, 64);
        if (!mem) return "no memory nearHit SpaceRenderer.dll";
        mem[0] = 0xFF;  // jmp [rip+0] ; dq StarWrap
        mem[1] = 0x25;
        memset(mem + 2, 0, 4);
        const uint64_t target = reinterpret_cast<uint64_t>(&StarWrap);
        memcpy(mem + 6, &target, 8);
        DWORD old = 0;
        if (!VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &old)) return "VirtualProtect (stub) failed";
        FlushInstructionCache(GetCurrentProcess(), mem, 64);
        g_stub = mem;
    }
    unsigned char nb[2][5];
    for (int i = 0; i < 2; ++i) {
        const long long nrel = reinterpret_cast<intptr_t>(g_stub) - reinterpret_cast<intptr_t>(base + sites[i] + 5);
        if (nrel < INT32_MIN || nrel > INT32_MAX) return "stub out of rel32 range";
        const int32_t r32 = static_cast<int32_t>(nrel);
        nb[i][0] = 0xE8;
        memcpy(nb[i] + 1, &r32, 4);
    }
    // The call sites first (the filter passes every draw through until the wrapper marks a view), then the
    // renderable's slot.
    for (int i = 0; i < 2; ++i) {
        if (!Write5(base + sites[i], orig[i], nb[i])) {
            for (int j = 0; j < i; ++j) Write5(base + sites[j], nb[j], orig[j]);
            return "could not patch a StarMaterial::draw call";
        }
        g_calls[i] = base + sites[i];
        memcpy(g_callOrig[i], orig[i], 5);
    }
    if (!WritePtr(slot, reinterpret_cast<void*>(&RenderWrap))) {
        for (int i = 0; i < 2; ++i) Write5(g_calls[i], nb[i], g_callOrig[i]);
        return "could not patch the renderable vtable";
    }
    g_vtblSlot = slot;
    g_installed = true;
    g_wantSample[0] = g_wantSample[1] = 1;
    g_geomLines = 0;
    g_listLines = 0;
    AcquireSRWLockExclusive(&g_geomLock);
    g_geom[0] = g_geom[1] = g_geomLogged[0] = g_geomLogged[1] = Geom{};
    ReleaseSRWLockExclusive(&g_geomLock);
    AcquireSRWLockExclusive(&g_listLock);
    for (auto& l : g_lists) l = VertList{};
    ReleaseSRWLockExclusive(&g_listLock);
    Log("NIGHTSKY installed: SpaceRenderer renderable (vtable +0xC500) -> wrapper, StarMaterial::draw calls +0x6931 / +0x69A5 -> "
        "stub %p",
        g_stub);
    return "";
}

void NightSkyRemove() {
    AcquireSRWLockExclusive(&g_viewLock);
    g_pending.n = 0;
    for (auto& g : g_gen) g.n = 0;
    ReleaseSRWLockExclusive(&g_viewLock);
    if (!g_installed) return;
    // The slot first (no new view is marked), then the calls. A render worker still inside the wrapper or the
    // filter finishes normally: the code stays loaded and the stub stays allocated.
    WritePtr(g_vtblSlot, reinterpret_cast<void*>(g_render));
    for (int i = 0; i < 2; ++i) {
        unsigned char nb[5];
        memcpy(nb, g_calls[i], 5);
        Write5(g_calls[i], nb, g_callOrig[i]);
    }
    g_installed = false;
    Log("NIGHTSKY removed");
}

void NightSkySetMode(NightSkyMode m) {
    const long v = static_cast<long>(m);
    if (InterlockedExchange(&g_mode, v) != v)
    {
        char autoText[200];
        _snprintf_s(autoText, _countof(autoText), _TRUNCATE,
                    "with the zoom: none at %.0f deg or wider, brightest first fading in, the full star field at %.0f deg or "
                    "narrower (square points; none in the PLAT)",
                    kStarsStartFovDeg, kStarsFullFovDeg);
        Log("NIGHTSKY stars in ILARTS's views: %s", m == NightSkyMode::Hide   ? "hidden"
                                                   : m == NightSkyMode::Full ? "full star field (sphere at half the far plane, square points)"
                                                   : m == NightSkyMode::Auto ? autoText
                                                                             : "as DCS draws them");
    }
}

NightSkyMode NightSkyGetMode() { return static_cast<NightSkyMode>(g_mode); }

double NightSkyHfovDeg(double p00) {
    if (!(p00 > 0) || !std::isfinite(p00)) return 0;
    return 2.0 * std::atan(1.0 / p00) * 180.0 / 3.14159265358979323846;
}

double NightSkyNormMag(double mag) { return std::clamp(1.0 - (mag + 4.2) / 12.2, 0.0, 1.0); }

double NightSkyStarBrightness(double mag) {
    const double n = NightSkyNormMag(mag);
    return 0.00065 + (0.035 - 0.00065) * n * n;
}

NightSkyLook NightSkyAutoLook(double hfovDeg) {
    NightSkyLook l;
    if (!(hfovDeg > 0) || !std::isfinite(hfovDeg) || hfovDeg >= kStarsStartFovDeg) return l;
    const double t = hfovDeg <= kStarsFullFovDeg
                         ? 1.0
                         : std::log(kStarsStartFovDeg / hfovDeg) / std::log(kStarsStartFovDeg / kStarsFullFovDeg);
    l.magLimit = kStarsStartMag + (kStarsFullMag - kStarsStartMag) * t;
    // the newest stars come in faint, then everything brightens to DCS's own as the limit gets fainter
    double g = std::min(1.0, kStarsAppearBrightness / NightSkyStarBrightness(l.magLimit));
    const double r = std::clamp((l.magLimit - kStarsStartMag) / kStarsRampMag, 0.0, 1.0);
    g *= r * r * (3 - 2 * r);
    l.gain = g;
    return l;
}

std::string NightSkyStats() {
    const auto mode = static_cast<NightSkyMode>(g_mode);
    Geom g[2];
    AcquireSRWLockShared(&g_geomLock);
    g[0] = g_geom[0];
    g[1] = g_geom[1];
    ReleaseSRWLockShared(&g_geomLock);
    char last[160];
    const long long lc = g_lastCount, lt = g_lastTotal;
    if (lc < 0)
        _snprintf_s(last, _countof(last), _TRUNCATE, "hfov %.1f deg, mag limit %.1f, gain %.3f, all %lld stars", g_lastHfov,
                    g_lastMag, g_lastGain, lt);
    else
        _snprintf_s(last, _countof(last), _TRUNCATE, "hfov %.1f deg, mag limit %.1f, gain %.3f, %lld of %lld stars",
                    g_lastHfov, g_lastMag, g_lastGain, lc, lt);
    char b[1400];
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "installed=%d mode=%s (since the last line:) ILARTS views: renders=%lld star_draws=%lld hidden=%lld scaled=%lld "
                "faded=%lld points=%lld scale_refused=%lld near_misses=%lld | DCS views: renders=%lld star_draws=%lld | last "
                "ILARTS draw: %s | ILARTS view: %s | DCS view: %s",
                g_installed ? 1 : 0,
                mode == NightSkyMode::Hide   ? "hide"
                : mode == NightSkyMode::Full ? "full"
                : mode == NightSkyMode::Auto ? "auto"
                                             : "stock",
                InterlockedExchange64(&g_rendersOurs, 0), InterlockedExchange64(&g_drawsOurs, 0),
                InterlockedExchange64(&g_drawsHidden, 0), InterlockedExchange64(&g_drawsScaled, 0),
                InterlockedExchange64(&g_drawsFaded, 0), InterlockedExchange64(&g_drawsPoints, 0),
                InterlockedExchange64(&g_scaleRefused, 0), InterlockedExchange64(&g_nearMiss, 0),
                InterlockedExchange64(&g_rendersDcs, 0), InterlockedExchange64(&g_drawsDcs, 0), last, GeomText(g[0]).c_str(),
                GeomText(g[1]).c_str());
    // A fresh sample of each kind for the next line.
    g_wantSample[0] = g_wantSample[1] = 1;
    return b;
}

#ifdef NIGHT_SKY_TEST
void NightSkyTestSet(NightSkyRenderFn render, NightSkyStarDrawFn draw) {
    g_render = render;
    g_starDraw = draw;
}
void* NightSkyTestRenderWrap() { return reinterpret_cast<void*>(&RenderWrap); }
void* NightSkyTestStarWrap() { return reinterpret_cast<void*>(&StarWrap); }
void NightSkyTestInstalled(bool on) { g_installed = on; }
bool NightSkyTestProxyCode() { return MakeProxyCode(); }
#endif

}  // namespace lsodiag
