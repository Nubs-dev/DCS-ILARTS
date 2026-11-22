// Offline test of src/night_sky.cpp (no DCS):
//  1. the ring geometry (stars at 100 km, ILARTS's 80 km far plane) against the 2026-11-19 screenshots;
//  2. the auto curve: no star at the start angle or wider, monotonic and continuous while zooming in (no star
//     comes in brighter than kStarsAppearBrightness), the whole catalogue at DCS's brightness when zoomed in;
//  3. with a DCS install given, Space.dll's star catalogue (mapped, no code runs): sorted brightest first, and
//     how many stars each zoom shows;
//  4. the renderable wrapper and the StarMaterial::draw filter, driven with stand-ins (a material, a star
//     object and an effect with a 32-slot vtable): which views are ILARTS's (camera list per render graph build,
//     kept for 4 builds; the PLAT flagged), hide / full / stock / auto, the copies (sphere at half the far plane,
//     the star count for the limiting magnitude, uScale and milky way opacity for the gain, pixelSize through the
//     effect proxy), DCS's own views and DCS's data untouched, a draw outside the wrapper passed through, the
//     stats line;
//  5. with a DCS install given, installing and removing the patches on the real SpaceRenderer.dll (twice, as
//     for a mission restart), mapped with DONT_RESOLVE_DLL_REFERENCES (no DCS code runs).
//
//   night_sky_test.exe ["C:\Program Files\Eagle Dynamics\DCS World"]
#include "../src/night_sky.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::vector<std::string> g_lines;
}

namespace lsodiag {
void Log(const char* format, ...) {
    char b[4096];
    va_list ap;
    va_start(ap, format);
    vsnprintf(b, sizeof(b), format, ap);
    va_end(ap);
    g_lines.push_back(b);
    printf("  log: %s\n", b);
}
bool SafeRead(const void* address, void* out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &got) && got == size;
}
bool CompatOk() { return true; }
std::string CompatWhy() { return {}; }
bool EngineDisabled() { return false; }
}  // namespace lsodiag

using namespace lsodiag;

namespace {

int g_fail = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            printf("  FAIL line %d: %s\n", __LINE__, #c);         \
            ++g_fail;                                             \
        }                                                         \
    } while (0)

constexpr double kDeg = 57.29577951308232;

// ---- 1. geometry ----
// Where the ring's edge falls in a picture (NDC), for a horizontal field of view and aspect: the star at
// angle a off the axis is drawn at tan(a) in the view plane; the edge is tan(ring) away from the centre.
void RingInPicture(const char* what, double hfovDeg, double aspect, double farM, double radius) {
    const double tx = std::tan(hfovDeg / 2 / kDeg), ty = tx / aspect;
    const double p00 = 1 / tx, p11 = 1 / ty;
    double corner = 0;
    const double ring = NightSkyRingDeg(farM, radius, p00, p11, &corner);
    const double t = std::tan(ring / kDeg);
    printf("  %s: hfov %.1f deg -> ring %.1f deg, corner %.1f deg; edge on the horizontal axis at x = %.2f, on the "
           "vertical axis at y = %.2f (NDC; > 1 = outside the picture)\n",
           what, hfovDeg, ring, corner, t / tx, t / ty);
}

void TestGeometry() {
    printf("geometry\n");
    double corner = 0;
    // ILARTS's views: far plane 80 km (P32 0.400002, P22 -5.00002e-06 in the captured PLAT contexts); stars at 100 km.
    const double ring = NightSkyRingDeg(80000, 100000, 1.0, 1.0, &corner);
    CHECK(std::fabs(ring - 36.8699) < 0.001);
    CHECK(std::fabs(corner - 54.7356) < 0.001);
    // DCS's main view: far plane 150 km -> no ring; a view with its own instance: stars at half the far plane.
    CHECK(NightSkyRingDeg(150000, 100000, 1.2349, 1.9758, &corner) == 0);
    CHECK(NightSkyRingDeg(80000, 40000, 1, 1, nullptr) == 0);
    CHECK(NightSkyRingDeg(0, 100000, 1, 1, nullptr) == 0);
    // The cameras of the 2026-11-19 run (48 / 70 deg, 4:3) and their widest zoom (x1.6, extra_cams.cpp).
    RingInPicture("island, rest", 48, 4.0 / 3, 80000, 100000);
    RingInPicture("island, zoomed out", 76.8, 4.0 / 3, 80000, 100000);
    RingInPicture("crotch, rest", 70, 4.0 / 3, 80000, 100000);
    RingInPicture("crotch, wide", 100, 4.0 / 3, 80000, 100000);
    RingInPicture("crotch, zoomed out", 112, 4.0 / 3, 80000, 100000);
    // At rest the island camera shows no stars (corner inside the ring); zoomed out it does, as in the screenshot.
    {
        const double tx = std::tan(24 / kDeg), ty = tx * 0.75;
        double c = 0;
        CHECK(NightSkyRingDeg(80000, 100000, 1 / tx, 1 / ty, &c) > c);
        const double tx2 = std::tan(38.4 / kDeg), ty2 = tx2 * 0.75;
        CHECK(NightSkyRingDeg(80000, 100000, 1 / tx2, 1 / ty2, &c) < c);
    }
}

// ---- 2. the auto curve ----
// Brightness of a star of magnitude mag as drawn in auto (Star.fx's, x the gain; DCS's own zoom gain on top),
// 0 when not drawn.
double ShownBrightness(double hfov, double mag) {
    const NightSkyLook l = NightSkyAutoLook(hfov);
    if (!(l.gain > 0) || NightSkyNormMag(mag) < NightSkyNormMag(l.magLimit)) return 0;
    return l.gain * NightSkyStarBrightness(mag);
}

void TestCurve() {
    printf("auto curve\n");
    // nothing at the start angle or wider (the island's 48 deg rest zoom), or unknown
    for (double f : {kStarsStartFovDeg, 24.5, 30.0, 48.0, 76.8, 112.0, 0.0, -1.0}) CHECK(NightSkyAutoLook(f).gain == 0);
    // the whole catalogue at DCS's brightness from kStarsFullFovDeg in (the PTZ zooms in to 2 deg)
    for (double f : {kStarsFullFovDeg, 5.0, 4.0, 2.0, 1.0}) {
        const NightSkyLook l = NightSkyAutoLook(f);
        CHECK(l.magLimit >= kStarsFullMag && l.gain == 1);
    }
    // monotonic and continuous while zooming in: limit and gain never drop, no big step, every star's
    // brightness never drops, and a star comes in no brighter than kStarsAppearBrightness
    NightSkyLook prev = NightSkyAutoLook(kStarsStartFovDeg);
    double maxStep = 0, maxAppear = 0;
    const double mags[] = {-1.44, -0.62, 0.03, 1.0, 2.0, 3.5, 5.0, 6.5, 8.0};
    double prevB[9] = {};
    for (double f = kStarsStartFovDeg; f >= 1.0; f -= 0.01) {
        const NightSkyLook l = NightSkyAutoLook(f);
        CHECK(l.gain >= prev.gain - 1e-12);
        CHECK(l.gain == 0 || prev.gain == 0 || l.magLimit >= prev.magLimit - 1e-12);
        maxStep = std::max(maxStep, l.gain - prev.gain);
        for (int i = 0; i < 9; ++i) {
            const double b = ShownBrightness(f, mags[i]);
            CHECK(b >= prevB[i] - 1e-12);
            if (prevB[i] == 0 && b > 0) maxAppear = std::max(maxAppear, b);
            prevB[i] = b;
        }
        prev = l;
    }
    printf("  largest gain step per 0.01 deg: %.4f; brightest a star comes in at: %.5f (DCS's magnitude 8 star: %.5f)\n",
           maxStep, maxAppear, NightSkyStarBrightness(8));
    CHECK(maxStep < 0.01);
    CHECK(maxAppear <= kStarsAppearBrightness * 1.01);   // within one 0.01 deg step
    // Sirius fades in from nothing: just inside the start angle it is a small fraction of the appear level
    CHECK(ShownBrightness(kStarsStartFovDeg - 0.3, -1.44) > 0);
    CHECK(ShownBrightness(kStarsStartFovDeg - 0.3, -1.44) < 0.1 * kStarsAppearBrightness);
    // the brightest first: at 20 deg only first-magnitude stars
    CHECK(NightSkyAutoLook(20).magLimit < 1.0 && NightSkyAutoLook(20).magLimit > -1.44);
    // Star.fx's normalized magnitude and brightness
    CHECK(std::fabs(NightSkyNormMag(-4.2) - 1) < 1e-12 && NightSkyNormMag(8) == 0 && NightSkyNormMag(9) == 0);
    CHECK(std::fabs(NightSkyStarBrightness(8) - 0.00065) < 1e-12);
    // the PLAT's 17 deg is inside the curve: auto keeps the PLAT starless by identity (wrappers below)
    CHECK(NightSkyAutoLook(17).gain > 0);
}

// ---- 3. Space.dll's catalogue ----
// The table getStars walks (+0x11680, 32 bytes per star, magnitude at +0x18), in order; empty when the install
// is not there.
std::vector<double> Catalogue(const std::wstring& dcs) {
    std::vector<double> mags;
    const std::wstring path = dcs + L"\\bin\\Space.dll";
    HMODULE sp = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!sp) return mags;
    const auto base = reinterpret_cast<const unsigned char*>(sp);
    for (uintptr_t a = 0x11680; a + 8 < 0x1556E8; a += 32) {   // getStars: entry + 8 < 0x1556E8
        double mag = 0;
        memcpy(&mag, base + a + 0x18, 8);
        mags.push_back(mag);
    }
    FreeLibrary(sp);
    return mags;
}

void TestCatalogue(const std::vector<double>& mags) {
    printf("catalogue\n");
    bool sorted = true;
    for (size_t i = 1; i < mags.size(); ++i) sorted = sorted && mags[i] >= mags[i - 1];
    printf("  %zu stars, magnitude %.2f .. %.2f, %s\n", mags.size(), mags.front(), mags.back(),
           sorted ? "brightest first" : "NOT sorted");
    CHECK(mags.size() == 41475 && sorted && mags.front() == -1.44 && mags.back() == 8.0);
    // stars in a 4:3 picture (the whole sky is 41253 square degrees)
    for (double f : {24.0, 22.0, 20.0, 17.0, 15.0, 12.0, 10.0, 8.0, 7.0, 6.0, 4.0, 2.0}) {
        const NightSkyLook l = NightSkyAutoLook(f);
        long long n = 0;
        for (double mag : mags) n += l.gain > 0 && NightSkyNormMag(mag) >= NightSkyNormMag(l.magLimit) ? 1 : 0;
        printf("  hfov %4.1f deg: mag limit %5.2f, gain %.3f, %5lld stars in the sky, %6.2f in the picture\n", f,
               l.gain > 0 ? l.magLimit : -99.0, l.gain, n, n * f * f * 0.75 / 41253.0);
    }
}

// ---- 4. wrappers ----
using StarFn = void (*)(void*, const void*);
using RenderWrapFn = void (*)(void*, const void*, void*, void*);

// DCS's effect: a vtable of 32 slots; +0xA8 sets a vector parameter, +0xB8 a float, the rest take this only.
constexpr uint64_t kPixHandle = 0x5150, kOtherHandle = 0x999;
struct FakeEffect {
    void** vtbl;
};
void* g_fxVtbl[32];
FakeEffect g_fx = {g_fxVtbl};
struct FxCall {
    const void* self;
    uint64_t handle;
    float v[2];
};
std::vector<FxCall> g_fxCalls;
void FxSetVec(void* self, uint64_t handle, const float* v) { g_fxCalls.push_back({self, handle, {v[0], v[1]}}); }
void FxSetFloat(void* self, uint64_t handle, float f) { g_fxCalls.push_back({self, handle, {f, 0}}); }
const void* g_fxOtherSelf = nullptr;
void FxOther(void* self) { g_fxOtherSelf = self; }

// StarMaterial: 0x138 bytes; +0x00 enabled, +0x08 / +0x10 the vertex list, +0x20 the effect, +0x128 pixelSize handle.
struct alignas(8) FakeMaterial {
    unsigned char b[0x138];
};
void SetMaterial(FakeMaterial& m, const float* begin, const float* end) {
    memset(m.b, 0, sizeof(m.b));
    m.b[0] = 1;
    memcpy(m.b + 0x08, &begin, 8);
    memcpy(m.b + 0x10, &end, 8);
    void* fx = &g_fx;
    memcpy(m.b + 0x20, &fx, 8);
    memcpy(m.b + 0x128, &kPixHandle, 8);
}
// The star list: 20-byte vertices (position at 100 km, normalized magnitude, twinkle), in catalogue order as
// SpaceRenderer builds it; and a second, unsorted list.
std::vector<float> g_verts;
float g_verts2[3][5] = {{100000, 0, 0, 0.2f, 0.1f}, {0, 0, 100000, 0.6f, 0.3f}, {0, 100000, 0, 0.1f, 0.5f}};
FakeMaterial g_mat, g_mat2;
alignas(16) unsigned char g_obj[0x60];

void BuildList(const std::vector<double>& mags) {
    g_verts.clear();
    for (size_t i = 0; i < mags.size(); ++i) {
        const float v[5] = {i % 2 ? 100000.0f : 0.0f, i % 2 ? 0.0f : 100000.0f, 0.0f,
                            static_cast<float>(NightSkyNormMag(mags[i])), 0.5f};
        g_verts.insert(g_verts.end(), v, v + 5);
    }
    SetMaterial(g_mat, g_verts.data(), g_verts.data() + g_verts.size());
    SetMaterial(g_mat2, &g_verts2[0][0], &g_verts2[0][0] + 15);
}

struct Drawn {
    const void* material;
    const void* obj;
    double radius;   // |row 0| x 100 km
    float row3[4];
    long long count;
    float zoom, milky;
    float pix[2];         // pixelSize as DCS's effect received it
    const void* fxSelf;   // the object DCS's effect calls ran on
    float uScale;
    bool otherVecIntact;
};
std::vector<Drawn> g_drawn;

// What StarMaterial::draw does with the material, the star object and the effect (the parts ILARTS relies on).
void FakeDraw(void* material, const void* obj) {
    const unsigned char* mb = static_cast<const unsigned char*>(material);
    const float* m = reinterpret_cast<const float*>(static_cast<const unsigned char*>(obj) + 4);
    Drawn d{};
    d.material = material;
    d.obj = obj;
    d.radius = std::sqrt(double(m[0]) * m[0] + double(m[1]) * m[1] + double(m[2]) * m[2]) * 100000.0;
    memcpy(d.row3, m + 12, sizeof(d.row3));
    const float *b = nullptr, *e = nullptr;
    memcpy(&b, mb + 0x08, 8);
    memcpy(&e, mb + 0x10, 8);
    d.count = (e - b) / 5;
    memcpy(&d.zoom, static_cast<const unsigned char*>(obj) + 0x5C, 4);
    memcpy(&d.milky, static_cast<const unsigned char*>(obj) + 0x58, 4);
    void* fx = nullptr;
    memcpy(&fx, mb + 0x20, 8);
    uint64_t h = 0;
    memcpy(&h, mb + 0x128, 8);
    void** vt = *static_cast<void***>(fx);
    g_fxCalls.clear();
    g_fxOtherSelf = nullptr;
    const float ps[2] = {1 / 768.0f, 1 / 768.0f};
    reinterpret_cast<void (*)(void*, uint64_t, const float*)>(vt[0xA8 / 8])(fx, h, ps);
    reinterpret_cast<void (*)(void*, uint64_t, float)>(vt[0xB8 / 8])(fx, 0x110, 0.25f);
    reinterpret_cast<void (*)(void*, uint64_t, const float*)>(vt[0xA8 / 8])(fx, kOtherHandle, ps);
    reinterpret_cast<void (*)(void*)>(vt[0x28 / 8])(fx);
    CHECK(g_fxCalls.size() == 3 && g_fxOtherSelf == &g_fx);
    if (g_fxCalls.size() == 3) {
        d.pix[0] = g_fxCalls[0].v[0];
        d.pix[1] = g_fxCalls[0].v[1];
        d.fxSelf = g_fxCalls[0].self;
        d.uScale = g_fxCalls[1].self == &g_fx && g_fxCalls[1].handle == 0x110 ? g_fxCalls[1].v[0] : -1;
        d.otherVecIntact = g_fxCalls[2].self == &g_fx && g_fxCalls[2].handle == kOtherHandle &&
                           g_fxCalls[2].v[0] == ps[0] && g_fxCalls[2].v[1] == ps[1];
    }
    g_drawn.push_back(d);
}
void FakeRender(void*, const void*, void*, void*) {
    // what SpaceRendererInstance::draw does through the two patched call sites
    auto star = reinterpret_cast<StarFn>(NightSkyTestStarWrap());
    star(&g_mat, g_obj);
    star(&g_mat2, g_obj);
}

alignas(16) unsigned char g_ctx[0x600];
alignas(16) unsigned char g_self[0x20];

void SetCtx(const double cam[3], double p00, double p11, double farM) {
    memset(g_ctx, 0, sizeof(g_ctx));
    memcpy(g_ctx + 0x308, cam, 24);
    float P[16] = {};
    const double n = 0.4;
    P[0] = static_cast<float>(p00);
    P[5] = static_cast<float>(p11);
    P[10] = static_cast<float>(-n / (farM - n));
    P[11] = 1;
    P[14] = static_cast<float>(n * farM / (farM - n));
    memcpy(g_ctx + 0x104, P, sizeof(P));
}
// A 4:3 view (an extra camera) with this horizontal field of view, 80 km far plane.
void SetCtxFov(const double cam[3], double hfovDeg) {
    const double p00 = 1 / std::tan(hfovDeg / 2 / kDeg);
    SetCtx(cam, p00, p00 * 4 / 3, 80000);
}

void Render() {
    auto wrap = reinterpret_cast<RenderWrapFn>(NightSkyTestRenderWrap());
    wrap(g_self, g_ctx, nullptr, nullptr);
}

long long CountTo(const std::vector<double>& mags, double magLimit) {
    long long n = 0;
    for (double mag : mags) n += NightSkyNormMag(mag) >= NightSkyNormMag(magLimit) ? 1 : 0;
    return n;
}

void TestWrappers(const std::vector<double>& catalogue) {
    printf("wrappers\n");
    for (auto& s : g_fxVtbl) s = reinterpret_cast<void*>(&FxOther);
    g_fxVtbl[0xA8 / 8] = reinterpret_cast<void*>(&FxSetVec);
    g_fxVtbl[0xB8 / 8] = reinterpret_cast<void*>(&FxSetFloat);
    BuildList(catalogue);
    const long long total = static_cast<long long>(catalogue.size());
    CHECK(NightSkyTestProxyCode());
    NightSkyTestSet(&FakeRender, &FakeDraw);
    NightSkyTestInstalled(true);
    // StarObject: rows 0-2 identity (stars at the catalogue's 100 km), row 3 = camera - origin, +0x58 the milky
    // way value (opacity 0.7), +0x5C the view's P11.
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 12.5f, 20.0f, -7.0f, 1};
    memcpy(g_obj + 4, m, sizeof(m));
    const float milky = -0.12f, zoom = 15.0f;
    memcpy(g_obj + 0x58, &milky, 4);
    memcpy(g_obj + 0x5C, &zoom, 4);
    unsigned char objBefore[sizeof(g_obj)];
    memcpy(objBefore, g_obj, sizeof(g_obj));
    const FakeMaterial matBefore = g_mat, mat2Before = g_mat2;
    const std::vector<float> vertsBefore = g_verts;
    auto dcsUntouched = [&] {
        return memcmp(objBefore, g_obj, sizeof(g_obj)) == 0 && memcmp(&matBefore, &g_mat, sizeof(g_mat)) == 0 &&
               memcmp(&mat2Before, &g_mat2, sizeof(g_mat2)) == 0 && g_verts == vertsBefore && g_fx.vtbl == g_fxVtbl;
    };
    // a draw exactly as DCS makes it: its material, object and effect; pixelSize as the render target gives it
    auto asDcs = [&](const Drawn& d) {
        return (d.material == &g_mat || d.material == &g_mat2) && d.obj == g_obj && std::fabs(d.radius - 100000.0) < 1.0 &&
               d.fxSelf == &g_fx && d.pix[0] == 1 / 768.0f && d.pix[1] == 1 / 768.0f && d.uScale == 0.25f &&
               d.otherVecIntact;
    };

    const double plat[3] = {10188.47045823225, 20.49999995553165, -183623.10993724316};
    const double island[3] = {10100.25, 31.5, -183600.75};
    const double mainCam[3] = {12072.451589730637, 821.749999937205743, -183628.87029950644};
    const double nearIsland[3] = {island[0] + 1.0, island[1], island[2]};
    NightSkyNoteView(plat, true);
    NightSkyNoteView(island, false);
    NightSkyNoteView(island, false);   // a view set up twice: listed once
    NightSkyBuildEnd();
    NightSkyStats();                   // reset counters

    // hide (the default): ILARTS's views draw nothing, DCS's draw both layers unchanged
    CHECK(NightSkyGetMode() == NightSkyMode::Hide);
    g_drawn.clear();
    SetCtx(island, 1 / std::tan(50 / kDeg), 0.75 / std::tan(50 / kDeg), 80000);
    Render();
    CHECK(g_drawn.empty());
    SetCtx(plat, 6.6912, 8.9215, 80000);
    Render();
    CHECK(g_drawn.empty());
    SetCtx(mainCam, 1.2349, 1.9758, 150000);
    Render();
    CHECK(g_drawn.size() == 2);
    for (const Drawn& d : g_drawn) CHECK(asDcs(d));
    CHECK(g_drawn.size() == 2 && g_drawn[0].count == total && g_drawn[1].count == 3);
    CHECK(dcsUntouched());
    // 1 m from a listed camera is not one of ILARTS's views (counted as a near miss)
    g_drawn.clear();
    SetCtx(nearIsland, 1, 1, 80000);
    Render();
    CHECK(g_drawn.size() == 2);
    for (const Drawn& d : g_drawn) CHECK(asDcs(d));
    std::string st = NightSkyStats();
    printf("  stats: %s\n", st.c_str());
    CHECK(st.find("mode=hide") != std::string::npos);
    CHECK(st.find("ILARTS views: renders=2 star_draws=4 hidden=4 scaled=0") != std::string::npos);
    CHECK(st.find("near_misses=1") != std::string::npos);
    CHECK(st.find("DCS views: renders=2 star_draws=4") != std::string::npos);
    CHECK(st.find("ILARTS view: far 80000 m, stars at 100000 m (global instance) -> beyond the far plane within 36.9 deg") !=
          std::string::npos);
    CHECK(st.find("DCS view: far 150000 m, stars at 100000 m (global instance) -> all in front") != std::string::npos);

    // full: ILARTS's views get copies: the sphere at half the far plane (camera row kept), every star at DCS's
    // brightness, square points (pixelSize x scaled by P00 / P11 = 0.75: a 4:3 picture in a square target)
    NightSkySetMode(NightSkyMode::Full);
    g_drawn.clear();
    SetCtx(island, 1, 4.0 / 3, 80000);
    Render();
    CHECK(g_drawn.size() == 2);
    for (const Drawn& d : g_drawn) {
        CHECK(d.obj != g_obj && d.material != &g_mat && d.material != &g_mat2);
        CHECK(std::fabs(d.radius - 40000.0) < 1.0);
        CHECK(d.row3[0] == 12.5f && d.row3[1] == 20.0f && d.row3[2] == -7.0f && d.row3[3] == 1.0f);
        CHECK(d.zoom == zoom && d.milky == milky);
        CHECK(d.fxSelf == &g_fx);   // the proxy forwards to DCS's effect
        CHECK(std::fabs(d.pix[0] - 0.75f / 768) < 1e-9 && std::fabs(d.pix[1] - 1.0f / 768) < 1e-9);
        CHECK(d.uScale == 0.25f && d.otherVecIntact);
    }
    CHECK(g_drawn.size() == 2 && g_drawn[0].count == total && g_drawn[1].count == 3);
    CHECK(dcsUntouched());
    // ... a projection as wide as tall keeps DCS's pixelSize
    g_drawn.clear();
    SetCtx(island, 1, 1, 80000);
    Render();
    CHECK(g_drawn.size() == 2 && std::fabs(g_drawn[0].pix[0] - 1.0f / 768) < 1e-9);
    // ... DCS's own views still unchanged; the PLAT in full gets the stars
    g_drawn.clear();
    SetCtx(mainCam, 1.2349, 1.9758, 150000);
    Render();
    CHECK(g_drawn.size() == 2 && asDcs(g_drawn[0]) && asDcs(g_drawn[1]));
    g_drawn.clear();
    SetCtx(plat, 6.6912, 8.9215, 80000);
    Render();
    CHECK(g_drawn.size() == 2 && g_drawn[0].material != &g_mat && std::fabs(g_drawn[0].radius - 40000.0) < 1.0);

    // stock: ILARTS's views draw as DCS does
    NightSkySetMode(NightSkyMode::Stock);
    g_drawn.clear();
    SetCtx(plat, 6.6912, 8.9215, 80000);
    Render();
    CHECK(g_drawn.size() == 2 && asDcs(g_drawn[0]) && asDcs(g_drawn[1]));

    // auto
    NightSkySetMode(NightSkyMode::Auto);
    // fully zoomed in (2 deg): every star at DCS's brightness, at half the far plane, square points
    g_drawn.clear();
    SetCtxFov(island, 2);
    Render();
    CHECK(g_drawn.size() == 2);
    for (const Drawn& d : g_drawn) {
        CHECK(d.material != &g_mat && std::fabs(d.radius - 40000.0) < 1.0 && d.zoom == zoom && d.milky == milky);
        CHECK(std::fabs(d.pix[0] - 0.75f / 768) < 1e-9 && d.fxSelf == &g_fx);
    }
    CHECK(g_drawn.size() == 2 && g_drawn[0].count == total && g_drawn[1].count == 3);
    // in between: the stars down to the limiting magnitude, alpha x gain through uScale, the milky way x gain; the
    // unsorted second list whole, at the same gain
    for (double f : {12.0, 8.0, 20.0}) {
        const NightSkyLook l = NightSkyAutoLook(f);
        const long long want = CountTo(catalogue, l.magLimit);
        g_drawn.clear();
        SetCtxFov(island, f);
        Render();
        CHECK(g_drawn.size() == 2);
        if (g_drawn.size() != 2) continue;
        const Drawn& d = g_drawn[0];
        printf("  %.0f deg: mag limit %.2f, gain %.3f, %lld of %lld stars drawn\n", f, l.magLimit, l.gain, d.count, total);
        CHECK(d.count == want && want > 0 && want < total);
        // Star.fx: alpha / uScale, uScale = pow(1 / z, 0.8)
        const double alphaRatio = std::pow(1 / double(zoom), 0.8) / std::pow(1 / double(d.zoom), 0.8);
        CHECK(l.gain >= 1 ? d.zoom == zoom : std::fabs(alphaRatio - l.gain) < 1e-4);
        const double op = std::clamp((-double(d.milky) - 0.05) * 10, 0.0, 1.0);
        CHECK(std::fabs(op - 0.7 * l.gain) < 1e-4);
        CHECK(g_drawn[1].count == 3 && g_drawn[1].zoom == d.zoom);
        CHECK(std::fabs(d.pix[0] - 0.75f / 768) < 1e-9);
    }
    // just inside the start: Sirius not in yet, so the sorted list draws nothing; the unsorted one faintly
    g_drawn.clear();
    SetCtxFov(island, kStarsStartFovDeg - 0.05);
    Render();
    CHECK(g_drawn.size() == 1 && g_drawn[0].count == 3 && g_drawn[0].zoom < zoom * 0.01f);
    // rest zoom (48 deg) and the start angle: nothing
    g_drawn.clear();
    SetCtxFov(island, 48);
    Render();
    SetCtxFov(island, kStarsStartFovDeg + 0.01);
    Render();
    CHECK(g_drawn.empty());
    // the PLAT: nothing at its 17 deg, nor at any zoom (by identity, not by field of view)
    SetCtx(plat, 6.6912, 8.9215, 80000);
    Render();
    SetCtxFov(plat, 2);
    Render();
    CHECK(g_drawn.empty());
    // DCS's own views unchanged, also zoomed in
    SetCtx(mainCam, 1.2349, 1.9758, 150000);
    Render();
    SetCtxFov(mainCam, 2);
    Render();
    CHECK(g_drawn.size() == 4);
    for (const Drawn& d : g_drawn) CHECK(asDcs(d));
    CHECK(dcsUntouched());
    st = NightSkyStats();
    printf("  stats: %s\n", st.c_str());
    CHECK(st.find("mode=auto") != std::string::npos);
    CHECK(st.find("faded=") != std::string::npos && st.find("faded=0") == std::string::npos);
    // a view listed as both the PLAT and a camera counts as a camera
    NightSkyNoteView(plat, true);
    NightSkyNoteView(plat, false);
    NightSkyBuildEnd();
    g_drawn.clear();
    SetCtxFov(plat, 2);
    Render();
    CHECK(g_drawn.size() == 2);
    for (int b = 0; b < 4; ++b) NightSkyBuildEnd();
    NightSkyNoteView(plat, true);
    NightSkyNoteView(island, false);
    NightSkyBuildEnd();
    // a list that changes (a new mission's) is read again
    {
        const std::vector<double> few(catalogue.begin(), catalogue.begin() + std::min<size_t>(50, catalogue.size()));
        BuildList(few);
        g_drawn.clear();
        SetCtxFov(island, 12);
        Render();
        CHECK(g_drawn.size() == 2 && g_drawn[0].count == CountTo(few, NightSkyAutoLook(12).magLimit));
        BuildList(catalogue);
    }
    // the same list no longer sorted (a different DCS): drawn whole, faded by the gain only
    {
        std::vector<float> saved = g_verts;
        std::swap(g_verts[3], g_verts[(g_verts.size() / 5 - 1) * 5 + 3]);   // faintest first
        g_drawn.clear();
        SetCtxFov(island, 12);
        Render();
        CHECK(g_drawn.size() == 2 && g_drawn[0].count == total && g_drawn[0].zoom < zoom);
        g_verts = saved;
    }
    NightSkySetMode(NightSkyMode::Hide);

    // a draw outside the renderable's call (another caller of SpaceRendererInstance::draw) is passed through
    g_drawn.clear();
    reinterpret_cast<StarFn>(NightSkyTestStarWrap())(&g_mat, g_obj);
    CHECK(g_drawn.size() == 1 && asDcs(g_drawn[0]));

    // the view list is kept for 4 builds; a 5th build without the view forgets it
    for (int b = 0; b < 3; ++b) NightSkyBuildEnd();
    g_drawn.clear();
    SetCtx(island, 1, 1, 80000);
    Render();
    CHECK(g_drawn.empty());
    NightSkyBuildEnd();
    g_drawn.clear();
    Render();
    CHECK(g_drawn.size() == 2);
    // the renderable's instance decides which path DCS takes; the stats line says which
    NightSkyNoteView(island, false);
    NightSkyBuildEnd();
    NightSkyStats();
    void* inst = g_self;
    memcpy(g_self + 0x10, &inst, sizeof(inst));
    g_drawn.clear();
    Render();
    st = NightSkyStats();
    CHECK(st.find("(own instance)") != std::string::npos);
    memset(g_self, 0, sizeof(g_self));
    NightSkyTestInstalled(false);
    NightSkyRemove();   // not installed for real: only clears the lists
}

// ---- 5. the real DLL ----
void TestRealDll(const std::wstring& dcs) {
    printf("SpaceRenderer.dll\n");
    std::wstring path = dcs + L"\\bin\\SpaceRenderer.dll";
    HMODULE sr = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!sr) {
        printf("  skipped: cannot map %ls (%lu)\n", path.c_str(), GetLastError());
        return;
    }
    auto base = reinterpret_cast<unsigned char*>(sr);
    // Every virtual call in StarMaterial::draw (+0x87D0 .. +0x8C00: call [rax+disp8] FF 50 xx, call [rax+disp32]
    // FF 90 xxxxxxxx) must be inside the stand-in effect's vtable (1.1.0.1 crashed on slot 37 with 32 slots).
    {
        int maxSlot = -1, calls = 0;
        for (size_t o = 0x87D0; o + 6 < 0x8C00; ++o) {
            const unsigned char* b = base + o;
            if (b[0] != 0xFF) continue;
            int disp = -1;
            if (b[1] == 0x50) disp = b[2];
            else if (b[1] == 0x90) disp = *reinterpret_cast<const int32_t*>(b + 2);
            if (disp < 0 || disp % 8) continue;
            ++calls;
            maxSlot = (std::max)(maxSlot, disp / 8);
        }
        printf("  StarMaterial::draw: %d virtual calls, highest slot %d (stand-in has %d)\n", calls, maxSlot,
               kStarEffectSlots);
        CHECK(calls >= 10 && maxSlot >= 37 && maxSlot < kStarEffectSlots);
    }
    unsigned char before[2][16], vt[8];
    memcpy(before[0], base + 0x6930, 16);
    memcpy(before[1], base + 0x69A0, 16);
    memcpy(vt, base + 0xC500, 8);
    for (int mission = 0; mission < 2; ++mission) {
        const std::string why = NightSkyInstall();
        printf("  install: %s\n", why.empty() ? "ok" : why.c_str());
        CHECK(why.empty());
        if (!why.empty()) break;
        void* slot = nullptr;
        memcpy(&slot, base + 0xC500, 8);
        CHECK(slot == NightSkyTestRenderWrap());
        for (uintptr_t site : {uintptr_t(0x6931), uintptr_t(0x69A5)}) {
            CHECK(base[site] == 0xE8);
            int32_t rel = 0;
            memcpy(&rel, base + site + 1, 4);
            const unsigned char* stub = base + site + 5 + rel;
            CHECK(stub[0] == 0xFF && stub[1] == 0x25);
            uint64_t target = 0;
            memcpy(&target, stub + 6, 8);
            CHECK(reinterpret_cast<void*>(target) == NightSkyTestStarWrap());
        }
        // the bytes around the call sites are unchanged
        CHECK(memcmp(base + 0x6930, before[0], 1) == 0 && memcmp(base + 0x6936, before[0] + 6, 10) == 0);
        CHECK(memcmp(base + 0x69A0, before[1], 5) == 0 && memcmp(base + 0x69AA, before[1] + 10, 6) == 0);
        NightSkyRemove();
        CHECK(memcmp(base + 0x6930, before[0], 16) == 0);
        CHECK(memcmp(base + 0x69A0, before[1], 16) == 0);
        CHECK(memcmp(base + 0xC500, vt, 8) == 0);
    }
    // what the filter relies on in StarMaterial::draw (also compat fingerprints): uScale = powf(1 / obj+0x5C, 0.8),
    // the star count (+0x10 - +0x08) / 20, pixelSize through effect vtable +0xA8 with handle +0x128, the effect
    // at +0x20, the material 0x138 bytes
    {
        const unsigned char uscale[] = {0x0F, 0x28, 0xC6, 0xF3, 0x41, 0x0F, 0x5E, 0x47, 0x5C};   // +0x88D3
        CHECK(memcmp(base + 0x88D3, uscale, sizeof(uscale)) == 0);
        const unsigned char count[] = {0x48, 0x8B, 0x4F, 0x10, 0x48, 0x2B, 0x4F, 0x08};   // +0x8BB8
        CHECK(memcmp(base + 0x8BB8, count, sizeof(count)) == 0);
        const unsigned char pix[] = {0x48, 0x8B, 0x97, 0x28, 0x01, 0x00, 0x00, 0xFF, 0x90, 0xA8, 0x00, 0x00, 0x00};   // +0x8A46
        CHECK(memcmp(base + 0x8A46, pix, sizeof(pix)) == 0);
        const unsigned char size[] = {0xB9, 0x38, 0x01, 0x00, 0x00};   // +0x57F7 mov ecx,138h
        CHECK(memcmp(base + 0x57F7, size, sizeof(size)) == 0);
    }
    // refused on unexpected code
    DWORD old = 0;
    VirtualProtect(base + 0x6931, 1, PAGE_EXECUTE_READWRITE, &old);
    base[0x6931] = 0x90;
    const std::string why = NightSkyInstall();
    printf("  install on changed code: %s\n", why.c_str());
    CHECK(!why.empty());
    CHECK(memcmp(base + 0xC500, vt, 8) == 0);
    base[0x6931] = 0xE8;
    VirtualProtect(base + 0x6931, 1, old, &old);
    FreeLibrary(sr);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestGeometry();
    TestCurve();
    const std::wstring dcs = argc > 1 ? argv[1] : L"C:\\Program Files\\Eagle Dynamics\\DCS World";
    std::vector<double> mags = Catalogue(dcs);
    if (mags.empty()) {
        printf("catalogue: skipped (no Space.dll); a made-up one\n");
        for (int i = 0; i < 2000; ++i) mags.push_back(-1.44 + 9.44 * std::pow(i / 1999.0, 0.3));
    } else {
        TestCatalogue(mags);
    }
    TestWrappers(mags);
    TestRealDll(dcs);
    printf(g_fail ? "night_sky_test: %d FAILED\n" : "night_sky_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
