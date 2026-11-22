// Offline test of src/rain.cpp (no DCS):
//  1. the wrapper, driven with stand-ins: a Rain object, renderables and contexts, and a fake of DCS's per-view
//     call that does what Rain::update does with cameraVel (a view with the update mark works it out from its
//     camera and the one stored at the last update; every view uploads whatever +0x150 holds) and what
//     collectInstances does with the cell centre. A flight at 150 m/s with the PLAT and an island camera on a
//     ship at 15 m/s, 60 frames a second: stock reproduces the bug (the PLAT gets your 150 m/s), auto gives each
//     ILARTS view its own 15 m/s, still gives 0, hide draws nothing; DCS's views see exactly what they would
//     without the wrapper (also when ILARTS's views draw first, and for a DCS view drawn after them); a mark on
//     one of ILARTS's views is drawn as DCS does (no copy); camera positions
//     updated in 10 Hz steps; a camera moved to another spot (a new view, no spike); speeds cut at 60 m/s; a near
//     miss; an unreadable Rain object; a value changed during the call left alone; the sample and stats lines;
//     the precipitation type per view; your camera in pure snow (DCS draws no rain: ILARTS's views draw DCS's rain
//     object through their snow renderable, one of them running the simulation); the far views' cloud lookup;
//  2. with a DCS install given, Visualizer.dll and Effects2.dll mapped with DONT_RESOLVE_DLL_REFERENCES (no DCS
//     code runs): every reference to the renderable's vtable and to the Rain::draw import (one call site: the
//     wrapped one), every write to cameraVel in Rain::update and the private draw (only the two stores behind the
//     update mark), install refused while the import is unresolved, then installed and removed twice (as for a
//     mission restart), refused on changed code or a slot holding something else.
//
//   rain_test.exe ["C:\Program Files\Eagle Dynamics\DCS World"]
#include "../src/night_sky.h"
#include "../src/rain.h"

#include <windows.h>

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
// extra_cams.h: one camera at a hangar deck position, when set
bool g_hangarSet = false;
double g_hangarPos[3] = {};
bool ExtraCamsHangarNear(const double* w, double metres) {
    if (!g_hangarSet) return false;
    const double d[3] = {w[0] - g_hangarPos[0], w[1] - g_hangarPos[1], w[2] - g_hangarPos[2]};
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < metres * metres;
}
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

using WrapFn = void (*)(void* self, const void* ctx, void* a3, void* a4);

// ---- stand-ins ----
// Effects::Rain: +0x18 cell centre (float3), +0xA8 effect time (double), +0x110 stored camera (Vec3d), +0x150 cameraVel.
alignas(16) unsigned char g_rain[0x540];
double g_time = 0;          // the clock (s): the wrapper's and the fake effect time
double Clock() { return g_time; }

float* CamVel() { return reinterpret_cast<float*>(g_rain + 0x150); }
double* Stored() { return reinterpret_cast<double*>(g_rain + 0x110); }
double* EffTime() { return reinterpret_cast<double*>(g_rain + 0xA8); }

struct Upload {
    const void* self;
    double cam[3];
    float vel[3];   // what the constant buffer got
    int mark;
    const void* rain = nullptr;   // the Rain object drawn and the power passed
    float power = 0;
};
std::vector<Upload> g_uploads;
bool g_meddle = false;   // the fake writes cameraVel itself after the upload (another thread)

// What Visualizer +0xC9C60 and Effects2's Rain::draw do with the parts the wrapper touches.
void FakeDraw(void* self, const void* ctx, void*, void*) {
    auto s = static_cast<unsigned char*>(self);
    void* rain = nullptr;
    memcpy(&rain, s + 0x10, 8);
    auto r = static_cast<unsigned char*>(rain);
    double cam[3];
    memcpy(cam, static_cast<const unsigned char*>(ctx) + 0x308, 24);
    // collectInstances: the cells around this view's camera (12.5 m grid)
    float c[3];
    for (int i = 0; i < 3; ++i) c[i] = static_cast<float>(std::floor(cam[i] / 12.5) * 12.5 + 6.25);
    memcpy(r + 0x18, c, 12);
    const int mark = s[0x24];
    if (mark) {
        double t = 0;
        memcpy(&t, r + 0xA8, 8);
        const double dT = g_time - t;
        if (dT > 1e-6) {
            double prev[3];
            memcpy(prev, r + 0x110, 24);
            float v[3];
            for (int i = 0; i < 3; ++i) v[i] = static_cast<float>((cam[i] - prev[i]) / dT);
            memcpy(r + 0x150, v, 12);
            memcpy(r + 0x110, cam, 24);
            memcpy(r + 0xA8, &g_time, 8);
        }
    }
    Upload u;
    u.self = self;
    memcpy(u.cam, cam, 24);
    memcpy(u.vel, r + 0x150, 12);
    u.mark = mark;
    u.rain = rain;
    memcpy(&u.power, s + 0x18, 4);
    g_uploads.push_back(u);
    if (g_meddle) {
        const float other[3] = {1, 2, 3};
        memcpy(r + 0x150, other, 12);
    }
}

struct View {
    unsigned char rend[0x28] = {};
    unsigned char ctx[0x600] = {};
    double cam[3] = {};
    void Set(int mark, void* rain = g_rain) {
        memset(rend, 0, sizeof(rend));
        memcpy(rend + 0x10, &rain, 8);
        const float power = 0.8f, lo = 300, hi = 3000;
        memcpy(rend + 0x18, &power, 4);
        memcpy(rend + 0x1C, &lo, 4);
        memcpy(rend + 0x20, &hi, 4);
        rend[0x24] = static_cast<unsigned char>(mark);
    }
    void Place(const double* p) {
        memcpy(cam, p, 24);
        memcpy(ctx + 0x308, p, 24);
    }
};

double Speed(const float* v) { return std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]); }
double Speed(const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

const Upload* Last(const View& v) {
    for (auto it = g_uploads.rbegin(); it != g_uploads.rend(); ++it)
        if (std::memcmp(it->cam, v.cam, 24) == 0) return &*it;
    return nullptr;
}

bool Logged(const char* needle) {
    for (const auto& l : g_lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

// A frame: the build lists ILARTS's views at this frame's positions, then the draws in the given order
// ('m' your view with the mark, 'p' PLAT, 'i' island, 'o' another DCS view without the mark).
struct World {
    View mainV, platV, islandV, otherV;
    double mainPos[3] = {12000, 800, -183000}, ship[3] = {10188.47, 20.5, -183623.11};
    double mainVel[3] = {-120, -5, 90};   // 150 m/s
    double shipVel[3] = {9, 0, -12};      // 15 m/s
    double platOff[3] = {-14.0, 0, -14.87}, islandOff[3] = {-80, 11, 30};
    bool islandSteps = false;             // the island camera's position updated 10 times a second only
    double islandHeld[3] = {};
    double lastStep = -1;
    void Frame(const char* order, WrapFn wrap) {
        g_time += 1.0 / 60;
        for (int i = 0; i < 3; ++i) {
            mainPos[i] += mainVel[i] / 60;
            ship[i] += shipVel[i] / 60;
        }
        double p[3], is[3];
        for (int i = 0; i < 3; ++i) {
            p[i] = ship[i] + platOff[i];
            is[i] = ship[i] + islandOff[i];
        }
        if (islandSteps) {
            if (lastStep < 0 || g_time - lastStep >= 0.1 - 1e-9) {
                memcpy(islandHeld, is, 24);
                lastStep = g_time;
            }
            memcpy(is, islandHeld, 24);
        }
        mainV.Place(mainPos);
        platV.Place(p);
        islandV.Place(is);
        const double other[3] = {mainPos[0] + 3, mainPos[1], mainPos[2]};
        otherV.Place(other);
        NightSkyNoteView(platV.cam, true);
        NightSkyNoteView(islandV.cam, false);
        NightSkyBuildEnd();
        for (const char* c = order; *c; ++c) {
            View& v = *c == 'm' ? mainV : *c == 'p' ? platV : *c == 'i' ? islandV : otherV;
            wrap(v.rend, v.ctx, nullptr, nullptr);
        }
    }
};

void TestWrapper() {
    printf("wrapper\n");
    WrapFn wrap = reinterpret_cast<WrapFn>(RainTestWrap());
    RainTestSet(&FakeDraw, &Clock);
    RainTestInstalled(true);
    memset(g_rain, 0, sizeof(g_rain));
    World w;
    w.mainV.Set(1);
    w.platV.Set(0);
    w.islandV.Set(0);
    w.otherV.Set(0);
    g_time = 100;
    *EffTime() = g_time;
    memcpy(Stored(), w.mainPos, 24);

    // stock: the bug - every view gets your camera's 150 m/s
    RainSetMode(RainMode::Stock);
    for (int f = 0; f < 60; ++f) w.Frame("mpio", wrap);
    {
        const Upload* m = Last(w.mainV);
        const Upload* p = Last(w.platV);
        const Upload* i = Last(w.islandV);
        CHECK(m && p && i);
        if (m && p && i) {
            printf("  stock: your view %.1f m/s, PLAT %.1f m/s, island %.1f m/s\n", Speed(m->vel), Speed(p->vel), Speed(i->vel));
            CHECK(std::fabs(Speed(m->vel) - 150) < 0.5);
            CHECK(std::fabs(Speed(p->vel) - 150) < 0.5 && std::fabs(Speed(i->vel) - 150) < 0.5);
        }
    }

    // auto: the PLAT and the island get the ship's 15 m/s; your view and another DCS view are unchanged
    RainSetMode(RainMode::Auto);
    for (int f = 0; f < 60; ++f) {
        g_uploads.clear();
        w.Frame("mpio", wrap);
        if (f < 4) continue;   // the window fills (>= 0.05 s)
        const Upload* m = Last(w.mainV);
        const Upload* p = Last(w.platV);
        const Upload* i = Last(w.islandV);
        const Upload* o = Last(w.otherV);
        CHECK(m && p && i && o);
        if (!(m && p && i && o)) break;
        CHECK(std::fabs(Speed(m->vel) - 150) < 0.5);
        CHECK(std::fabs(Speed(p->vel) - 15) < 0.05 && std::fabs(Speed(i->vel) - 15) < 0.05);
        for (int k = 0; k < 3; ++k) CHECK(std::fabs(p->vel[k] - w.shipVel[k]) < 0.05);
        CHECK(memcmp(o->vel, m->vel, 12) == 0);       // a DCS view after ILARTS's: DCS's value
        CHECK(memcmp(CamVel(), m->vel, 12) == 0);     // left as DCS had it
        CHECK(p->self == w.platV.rend && i->self == w.islandV.rend);   // no mark: DCS's own renderable
        if (f == 59) printf("  auto: your view %.1f m/s, PLAT %.2f m/s, island %.2f m/s, other DCS view %.1f m/s\n",
                            Speed(m->vel), Speed(p->vel), Speed(i->vel), Speed(o->vel));
    }
    // ILARTS's views drawn before yours: yours still gets its own motion from its own stored camera
    for (int f = 0; f < 10; ++f) {
        g_uploads.clear();
        w.Frame("piom", wrap);
        const Upload* m = Last(w.mainV);
        const Upload* p = Last(w.platV);
        CHECK(m && p && std::fabs(Speed(m->vel) - 150) < 0.5 && std::fabs(Speed(p->vel) - 15) < 0.05);
    }
    {
        double st[3];
        memcpy(st, Stored(), 24);
        CHECK(memcmp(st, w.mainPos, 24) == 0);   // DCS's stored camera is your camera's
    }

    // still: 0 in ILARTS's views
    RainSetMode(RainMode::Still);
    RainTestResetStatsClock();
    RainStats();   // asks for a sample line per kind
    g_uploads.clear();
    w.Frame("mpio", wrap);
    {
        const Upload* m = Last(w.mainV);
        const Upload* p = Last(w.platV);
        const Upload* i = Last(w.islandV);
        CHECK(m && p && i && Speed(p->vel) == 0 && Speed(i->vel) == 0 && std::fabs(Speed(m->vel) - 150) < 0.5);
        CHECK(m && memcmp(CamVel(), m->vel, 12) == 0);
    }
    // hide: ILARTS's views draw nothing
    RainSetMode(RainMode::Hide);
    RainTestResetStatsClock();
    RainStats();
    g_uploads.clear();
    w.Frame("mpio", wrap);
    CHECK(g_uploads.size() == 2 && Last(w.platV) == nullptr && Last(w.islandV) == nullptr);
    RainSetMode(RainMode::Auto);
    RainTestResetStatsClock();
    RainStats();

    // a mark on one of ILARTS's views: drawn exactly as DCS does (DCS's own renderable, mark kept, no write)
    {
        w.platV.Set(1);
        unsigned char before[0x28];
        memcpy(before, w.platV.rend, sizeof(before));
        g_uploads.clear();
        w.Frame("mpio", wrap);
        const Upload* p = Last(w.platV);
        CHECK(p && p->self == w.platV.rend && p->mark == 1);
        CHECK(memcmp(before, w.platV.rend, sizeof(before)) == 0);
        CHECK(Logged("carries DCS's update mark: drawn as DCS does"));
        RainTestResetStatsClock();
        CHECK(RainStats().find("marked=1") != std::string::npos);
        w.platV.Set(0);
        for (int f = 0; f < 70; ++f) w.Frame("mpio", wrap);   // let the motion windows settle again
        RainStats();
    }

    // camera positions updated 10 times a second: still about the ship's speed (window 1 s)
    {
        w.islandSteps = true;
        double worst = 0;
        for (int f = 0; f < 180; ++f) {
            g_uploads.clear();
            w.Frame("mpio", wrap);
            const Upload* i = Last(w.islandV);
            if (f >= 70 && i) worst = std::fmax(worst, std::fabs(Speed(i->vel) - 15));
        }
        printf("  island camera moved in 10 Hz steps: speed within %.2f m/s of 15\n", worst);
        CHECK(worst < 2.0);
        w.islandSteps = false;
    }

    // the island camera moved to another spot (200 m): a new view, given the last tracked view's velocity
    {
        RainStats();
        w.islandOff[0] += 200;
        double maxSpeed = 0;
        for (int f = 0; f < 30; ++f) {
            g_uploads.clear();
            w.Frame("mpio", wrap);
            const Upload* i = Last(w.islandV);
            if (i) maxSpeed = std::fmax(maxSpeed, Speed(i->vel));
        }
        printf("  after a 200 m camera move: highest speed used %.2f m/s\n", maxSpeed);
        CHECK(maxSpeed < 15.1);
    }

    // a view moving at 100 m/s: cut to 60
    {
        const double saved[3] = {w.shipVel[0], w.shipVel[1], w.shipVel[2]};
        w.shipVel[0] = 100;
        w.shipVel[1] = w.shipVel[2] = 0;
        for (int f = 0; f < 70; ++f) {
            g_uploads.clear();
            w.Frame("mpio", wrap);
        }
        const Upload* p = Last(w.platV);
        CHECK(p && std::fabs(Speed(p->vel) - kRainMaxSpeed) < 0.01);
        memcpy(w.shipVel, saved, 24);
        for (int f = 0; f < 70; ++f) w.Frame("mpio", wrap);
    }

    // 1 m from a listed camera: a DCS view (near miss), drawn as DCS does
    {
        View nearV;
        nearV.Set(0);
        const double n[3] = {w.platV.cam[0] + 1, w.platV.cam[1], w.platV.cam[2]};
        nearV.Place(n);
        g_uploads.clear();
        wrap(nearV.rend, nearV.ctx, nullptr, nullptr);
        CHECK(g_uploads.size() == 1 && memcmp(g_uploads[0].vel, CamVel(), 12) == 0);
    }

    // a renderable without a Rain object, or one whose Rain cannot be read: passed through
    {
        View bad;
        bad.Set(0, nullptr);
        bad.Place(w.platV.cam);
        int calls = 0;
        RainTestSet(
            [](void*, const void*, void*, void*) { g_uploads.push_back(Upload{}); }, &Clock);
        g_uploads.clear();
        wrap(bad.rend, bad.ctx, nullptr, nullptr);
        calls = static_cast<int>(g_uploads.size());
        CHECK(calls == 1);
        bad.Set(0, reinterpret_cast<void*>(0x10));   // unreadable
        g_uploads.clear();
        wrap(bad.rend, bad.ctx, nullptr, nullptr);
        CHECK(g_uploads.size() == 1);
        RainTestSet(&FakeDraw, &Clock);
    }

    // cameraVel changed during the call (another thread): left as found
    {
        g_meddle = true;
        g_uploads.clear();
        w.Frame("p", wrap);
        CHECK(CamVel()[0] == 1 && CamVel()[1] == 2 && CamVel()[2] == 3);
        g_meddle = false;
        w.Frame("m", wrap);
    }

    // sample and stats lines
    CHECK(Logged("RAIN PLAT (ILARTS) view: camera"));
    CHECK(Logged("RAIN camera (ILARTS) view: camera"));
    CHECK(Logged("RAIN DCS updating view: camera"));
    CHECK(Logged("RAIN DCS other view: camera"));
    CHECK(Logged("drawn with this view's own motion (auto)"));
    CHECK(Logged("drawn as DCS does (stock: the updating view's motion)"));
    CHECK(Logged("drawn as for a camera at rest (still)"));
    CHECK(Logged("drawn not (hide)"));
    CHECK(Logged("RAIN anomaly: cameraVel changed during one of ILARTS's draws"));
    RainTestResetStatsClock();
    std::string st = RainStats();
    printf("  stats: %s\n", st.c_str());
    CHECK(st.find("mode=auto") != std::string::npos);
    CHECK(st.find("near_misses=1") != std::string::npos);
    CHECK(st.find("conflicts=1") != std::string::npos);
    CHECK(st.find("new_views=") != std::string::npos && st.find("new_views=0") == std::string::npos);
    CHECK(st.find("speed used: PLAT 15.0, camera 15.0") != std::string::npos);
    CHECK(st.find("streak velocity 150.1") != std::string::npos);
    CHECK(RainStats().empty());               // within 60 s of the last line
    g_time += 61;
    CHECK(RainStats().empty());               // nothing drawn since
    w.Frame("mp", wrap);
    g_time += 61;
    CHECK(!RainStats().empty());
    const size_t lines = g_lines.size();
    w.Frame("mp", wrap);                     // a sample per kind after each stats line
    CHECK(g_lines.size() == lines + 2);
    RainTestInstalled(false);
    RainRemove();   // not installed for real: nothing
    for (int b = 0; b < 4; ++b) NightSkyBuildEnd();
}

// ---- a camera at a hangar deck position: no rain or snow, in every mode ----
void TestHangar() {
    printf("hangar cameras\n");
    WrapFn wrap = reinterpret_cast<WrapFn>(RainTestWrap());
    RainTestSet(&FakeDraw, &Clock);
    RainTestInstalled(true);
    memset(g_rain, 0, sizeof(g_rain));
    World w;
    w.mainV.Set(1);
    w.platV.Set(0);
    w.islandV.Set(0);
    w.otherV.Set(0);
    g_time = 300;
    *EffTime() = g_time;
    memcpy(Stored(), w.mainPos, 24);
    RainSetMode(RainMode::Auto);
    for (int f = 0; f < 5; ++f) w.Frame("mpio", wrap);
    CHECK(Last(w.islandV) != nullptr);
    RainTestResetStatsClock();
    RainStats();
    for (RainMode m : {RainMode::Auto, RainMode::Still, RainMode::Stock}) {
        RainSetMode(m);
        g_uploads.clear();
        w.Frame("", wrap);   // positions for this frame
        g_hangarSet = true;
        memcpy(g_hangarPos, w.islandV.cam, 24);
        w.Frame("mpio", wrap);
        memcpy(g_hangarPos, w.islandV.cam, 24);
        g_uploads.clear();
        wrap(w.mainV.rend, w.mainV.ctx, nullptr, nullptr);
        wrap(w.platV.rend, w.platV.ctx, nullptr, nullptr);
        wrap(w.islandV.rend, w.islandV.ctx, nullptr, nullptr);
        CHECK(Last(w.islandV) == nullptr && Last(w.platV) != nullptr && Last(w.mainV) != nullptr);
        g_hangarSet = false;
    }
    // the PLAT is never a hangar camera, even at the same spot
    g_hangarSet = true;
    memcpy(g_hangarPos, w.platV.cam, 24);
    g_uploads.clear();
    wrap(w.platV.rend, w.platV.ctx, nullptr, nullptr);
    CHECK(Last(w.platV) != nullptr);
    g_hangarSet = false;
    RainSetMode(RainMode::Auto);
    RainTestResetStatsClock();
    const std::string st = RainStats();
    CHECK(st.find("hangar=") != std::string::npos && st.find("hangar=0") == std::string::npos);
    RainTestInstalled(false);
    for (int b = 0; b < 4; ++b) NightSkyBuildEnd();
}

// ---- precipitation type: DCS's split comes from your camera; ILARTS's views draw their own ----
alignas(16) unsigned char g_snow[0x540];

void TestType() {
    printf("precipitation type\n");
    WrapFn wrap = reinterpret_cast<WrapFn>(RainTestWrap());
    RainTestSet(&FakeDraw, &Clock);
    RainTestInstalled(true);
    RainSetMode(RainMode::Auto);
    memset(g_rain, 0, sizeof(g_rain));
    memset(g_snow, 0, sizeof(g_snow));
    const int one = 1;
    memcpy(g_snow + 0x94, &one, 4);   // Rain::init type: 0 rain, 1 snow
    struct Pair {
        View rain, snow;
    } mainV, platV, islandV;
    const double mainPos[3] = {12000, 3000, -183000}, plat[3] = {10174.5, 20.5, -183638.0}, island[3] = {10108.5, 61.0, -183593.1};
    auto place = [](Pair& p, const double* at, int mark) {
        p.rain.Set(mark, g_rain);
        p.snow.Set(mark, g_snow);
        const float rp = 0.48f, sp = 0.32f;   // DCS's split at your camera: rain share 0.6
        memcpy(p.rain.rend + 0x18, &rp, 4);
        memcpy(p.snow.rend + 0x18, &sp, 4);
        p.rain.Place(at);
        p.snow.Place(at);
    };
    place(mainV, mainPos, 1);
    place(platV, plat, 0);
    place(islandV, island, 0);
    auto frame = [&](const char* order) {
        g_time += 1.0 / 60;
        NightSkyNoteView(platV.rain.cam, true);
        NightSkyNoteView(islandV.rain.cam, false);
        NightSkyBuildEnd();
        g_uploads.clear();
        for (const char* c = order; *c; ++c) {
            Pair& p = *c == 'm' ? mainV : *c == 'p' ? platV : islandV;
            wrap(p.rain.rend, p.rain.ctx, nullptr, nullptr);
            wrap(p.snow.rend, p.snow.ctx, nullptr, nullptr);
        }
    };
    auto drew = [](const View& v) {
        for (const Upload& u : g_uploads)
            if (u.self == v.rend) return true;
        return false;
    };
    // no temperature from the hook: DCS's split everywhere (both objects in every view)
    RainSetCarrierTemperature(false, 0);
    frame("mpi");
    CHECK(drew(mainV.rain) && drew(mainV.snow) && drew(platV.rain) && drew(platV.snow) && drew(islandV.rain) && drew(islandV.snow));
    // a 20 C mission: no snow in ILARTS's views, your view as DCS draws it (in both orders)
    RainSetCarrierTemperature(true, 20);
    CHECK(Logged("RAIN carrier temperature 20.0 C at sea level"));
    for (const char* order : {"mpi", "pim"}) {
        frame(order);
        CHECK(drew(mainV.rain) && drew(mainV.snow));
        CHECK(drew(platV.rain) && !drew(platV.snow) && drew(islandV.rain) && !drew(islandV.snow));
    }
    // a freezing day: no rain in ILARTS's views
    RainSetCarrierTemperature(true, -5);
    frame("mpi");
    CHECK(drew(mainV.rain) && drew(mainV.snow) && !drew(platV.rain) && drew(platV.snow) && !drew(islandV.rain) && drew(islandV.snow));
    // in DCS's mixed band at the carrier: both, as DCS draws them
    RainSetCarrierTemperature(true, 0.5);
    frame("mpi");
    CHECK(drew(platV.rain) && drew(platV.snow) && drew(islandV.rain) && drew(islandV.snow));
    // the camera's height counts (6.5 C per km): +2.1 C at sea level is +1.97 C at the PLAT (20.5 m) and +1.70 C at the
    // island camera (61 m), both in the mixed band; a camera at sea level gets rain only
    RainSetCarrierTemperature(true, 2.1);
    frame("mpi");
    CHECK(drew(islandV.snow) && drew(platV.snow));
    {
        Pair low;
        const double atSea[3] = {plat[0] - 30, 0.0, plat[2]};
        place(low, atSea, 0);
        NightSkyNoteView(low.rain.cam, false);
        NightSkyBuildEnd();
        g_uploads.clear();
        wrap(low.rain.rend, low.rain.ctx, nullptr, nullptr);
        wrap(low.snow.rend, low.snow.ctx, nullptr, nullptr);
        CHECK(drew(low.rain) && !drew(low.snow));
    }
    // stock: DCS's draw (snow too); a view with DCS's update mark: drawn as DCS does
    RainSetCarrierTemperature(true, 20);
    RainSetMode(RainMode::Stock);
    frame("mpi");
    CHECK(drew(platV.snow) && drew(islandV.snow));
    RainSetMode(RainMode::Auto);
    platV.snow.rend[0x24] = 1;
    frame("mpi");
    CHECK(drew(platV.snow) && !drew(islandV.snow));
    platV.snow.rend[0x24] = 0;
    // sample and stats lines
    RainTestResetStatsClock();
    RainStats();
    wrap(mainV.rain.rend, mainV.rain.ctx, nullptr, nullptr);
    wrap(mainV.snow.rend, mainV.snow.ctx, nullptr, nullptr);
    wrap(platV.snow.rend, platV.snow.ctx, nullptr, nullptr);   // the PLAT's sample line from its snow draw
    frame("mpi");
    CHECK(Logged("drawn not (snow: too warm at this view)"));
    CHECK(Logged("snow object, 19.9 C at the camera (rain share 1.00; DCS's at your camera 0.60)"));
    CHECK(Logged("rain object, 19.6 C at the camera (rain share 1.00; DCS's at your camera 0.60)"));   // the island
    RainTestResetStatsClock();
    const std::string st = RainStats();
    printf("  stats: %s\n", st.c_str());
    CHECK(st.find("type: carrier 20.0 C at sea level, rain share used 1.00, DCS's at your camera 0.60") != std::string::npos);
    CHECK(st.find("snow not drawn=") != std::string::npos && st.find("snow not drawn=0") == std::string::npos);
    RainSetCarrierTemperature(false, 0);
    RainTestInstalled(false);
    for (int b = 0; b < 4; ++b) NightSkyBuildEnd();
}

// ---- your camera in pure snow: DCS draws no rain; ILARTS's views draw DCS's rain object for the carrier ----
uintptr_t FakeCollect(void*, uintptr_t, void*, void*, void*) { return 7; }

void TestBorrow() {
    printf("carrier's rain while DCS draws none\n");
    WrapFn wrap = reinterpret_cast<WrapFn>(RainTestWrap());
    using CollectFn = uintptr_t (*)(void*, uintptr_t, void*, void*, void*);
    CollectFn collect = reinterpret_cast<CollectFn>(RainTestCollectWrap());
    RainTestSet(&FakeDraw, &Clock);
    RainTestInstalled(true);
    RainTestSetBorrow(&FakeCollect, true);
    RainSetMode(RainMode::Auto);
    RainSetCarrierTemperature(true, 20);
    memset(g_rain, 0, sizeof(g_rain));
    memset(g_snow, 0, sizeof(g_snow));
    const int one = 1;
    memcpy(g_snow + 0x94, &one, 4);
    void* prov[3] = {nullptr, g_rain, g_snow};   // the provider: +0x08 rain, +0x10 snow
    g_time = 500;
    *EffTime() = g_time;
    // you 5.4 km up (pure snow: DCS gives every view only its snow renderable), the PLAT and the island on the ship
    View mainSnow, mainRain, platSnow, platRain, islandSnow;
    double mainPos[3] = {5800, 5400, 1400}, ship[3] = {15600, 0, -183700};
    const double mainVel[3] = {-230, 20, -80}, shipVel[3] = {13.9, 0, 0};
    const double platOff[3] = {-14, 20.5, -15}, islandOff[3] = {-80, 61, 30};
    const float snowPower = 0.8f;
    auto setSnow = [&](View& v, int mark) {
        v.Set(mark, g_snow);
        memcpy(v.rend + 0x18, &snowPower, 4);
    };
    setSnow(mainSnow, 1);
    setSnow(platSnow, 0);
    setSnow(islandSnow, 0);
    mainRain.Set(1, g_rain);
    platRain.Set(0, g_rain);
    bool provider = true;    // collectRenderables runs (a live provider)
    bool dcsRain = false;    // DCS gives the views rain renderables too (you below the snow)
    auto frame = [&](const char* order) {
        g_time += 1.0 / 60;
        for (int i = 0; i < 3; ++i) {
            mainPos[i] += mainVel[i] / 60;
            ship[i] += shipVel[i] / 60;
        }
        double p[3], is[3];
        for (int i = 0; i < 3; ++i) {
            p[i] = ship[i] + platOff[i];
            is[i] = ship[i] + islandOff[i];
        }
        mainSnow.Place(mainPos);
        mainRain.Place(mainPos);
        platSnow.Place(p);
        platRain.Place(p);
        islandSnow.Place(is);
        NightSkyNoteView(platSnow.cam, true);
        NightSkyNoteView(islandSnow.cam, false);
        NightSkyBuildEnd();
        if (provider) CHECK(collect(prov, 3, nullptr, nullptr, nullptr) == 7);
        g_uploads.clear();
        for (const char* c = order; *c; ++c) {
            if (*c == 'm') {
                if (dcsRain) wrap(mainRain.rend, mainRain.ctx, nullptr, nullptr);
                wrap(mainSnow.rend, mainSnow.ctx, nullptr, nullptr);
            } else if (*c == 'p') {
                if (dcsRain) wrap(platRain.rend, platRain.ctx, nullptr, nullptr);
                wrap(platSnow.rend, platSnow.ctx, nullptr, nullptr);
            } else {
                wrap(islandSnow.rend, islandSnow.ctx, nullptr, nullptr);
            }
        }
    };
    auto find = [](const View& v, const void* rain) -> const Upload* {
        for (const Upload& u : g_uploads)
            if (u.self == v.rend && u.rain == rain) return &u;
        return nullptr;
    };
    unsigned char platBefore[0x28], islandBefore[0x28], mainBefore[0x28];
    memcpy(platBefore, platSnow.rend, 0x28);
    memcpy(islandBefore, islandSnow.rend, 0x28);
    memcpy(mainBefore, mainSnow.rend, 0x28);

    // auto: the PLAT and the island draw DCS's rain object (power 0.8 x their rain share 1), the PLAT (first to draw)
    // runs the simulation; no snow in them (20 C); your view draws its snow as DCS does; the renderables put back
    RainTestResetStatsClock();
    RainStats();
    int marks = 0;
    double worstPlat = 0, worstIsland = 0;
    for (int f = 0; f < 90; ++f) {
        frame("mpi");
        const Upload* p = find(platSnow, g_rain);
        const Upload* i = find(islandSnow, g_rain);
        CHECK(p && i);
        if (!p || !i) break;
        CHECK(std::fabs(p->power - 0.8f) < 1e-6f && std::fabs(i->power - 0.8f) < 1e-6f);
        CHECK(p->mark == 1 && i->mark == 0);
        marks += p->mark + i->mark;
        CHECK(!find(platSnow, g_snow) && !find(islandSnow, g_snow));
        CHECK(find(mainSnow, g_snow) && !find(mainSnow, g_rain));
        if (f >= 10) {
            worstPlat = std::fmax(worstPlat, std::fabs(Speed(p->vel) - 13.9));
            worstIsland = std::fmax(worstIsland, std::fabs(Speed(i->vel) - 13.9));
        }
        if (f == 0) CHECK(Speed(p->vel) < 0.5);   // the new marker's first update: no streak from your camera
    }
    printf("  PLAT (runs the simulation) within %.3f m/s of the ship's 13.9, island within %.3f\n", worstPlat, worstIsland);
    CHECK(worstPlat < 0.05 && worstIsland < 0.05);
    CHECK(marks == 90);
    CHECK(memcmp(platBefore, platSnow.rend, 0x28) == 0 && memcmp(islandBefore, islandSnow.rend, 0x28) == 0);
    CHECK(memcmp(mainBefore, mainSnow.rend, 0x28) == 0);
    CHECK(Logged("RAIN DCS draws no rain (your camera in snow, DCS's rain share 0)"));
    CHECK(Logged("drawn with DCS's rain object (DCS draws no rain: your camera in snow), running its simulation"));
    CHECK(Logged("drawn with DCS's rain object (DCS draws no rain: your camera in snow);"));
    {
        RainTestResetStatsClock();
        const std::string st = RainStats();
        printf("  stats: %s\n", st.substr(st.find("| type:")).c_str());
        CHECK(st.find("DCS's rain drawn while DCS drew none=180 (simulated=90, no rain object=0)") != std::string::npos);
    }
    // the island drawn first: the PLAT keeps the simulation (the marker stays while it draws)
    for (int f = 0; f < 5; ++f) {
        frame("ipm");
        const Upload* p = find(platSnow, g_rain);
        const Upload* i = find(islandSnow, g_rain);
        CHECK(p && i && p->mark == 1 && i->mark == 0);
    }
    // the PLAT closed: the island takes over the simulation after 0.3 s, without a streak from the PLAT's position
    {
        int first = -1;
        double maxIsland = 0;
        for (int f = 0; f < 60; ++f) {
            frame("mi");
            const Upload* i = find(islandSnow, g_rain);
            CHECK(i != nullptr);
            if (!i) break;
            if (i->mark && first < 0) first = f;
            maxIsland = std::fmax(maxIsland, Speed(i->vel));
        }
        printf("  PLAT closed: the island runs the simulation from frame %d, highest speed %.2f m/s\n", first, maxIsland);
        CHECK(first >= 17 && first <= 20);
        CHECK(maxIsland < 14.5);
    }
    // you descend into rain: DCS gives the views rain renderables again; no more borrowing
    dcsRain = true;
    frame("mpi");   // your rain draw comes first: in this frame already
    CHECK(!find(platSnow, g_rain) && find(platRain, g_rain) && !find(islandSnow, g_rain));
    dcsRain = false;
    frame("mpi");   // within 0.25 s of DCS's last rain draw: not yet
    CHECK(!find(platSnow, g_rain));
    for (int f = 0; f < 16; ++f) frame("mpi");
    CHECK(find(platSnow, g_rain) != nullptr);
    // no live provider (collectRenderables not seen for 0.5 s): nothing borrowed, counted
    provider = false;
    for (int f = 0; f < 40; ++f) frame("mpi");
    CHECK(!find(platSnow, g_rain) && !find(islandSnow, g_rain));
    {
        RainTestResetStatsClock();
        const std::string st = RainStats();
        CHECK(st.find("no rain object=0)") == std::string::npos);
    }
    provider = true;
    // still: DCS's rain object with cameraVel 0 (the marker's stored camera set to its own: about 0 too)
    RainSetMode(RainMode::Still);
    for (int f = 0; f < 30; ++f) frame("mpi");
    {
        const Upload* p = find(platSnow, g_rain);
        const Upload* i = find(islandSnow, g_rain);
        CHECK(p && i && Speed(p->vel) < 0.01 && Speed(i->vel) < 0.01);
    }
    // stock and hide: DCS's draw / nothing, no borrowing
    RainSetMode(RainMode::Stock);
    frame("mpi");
    CHECK(!find(platSnow, g_rain) && find(platSnow, g_snow));
    RainSetMode(RainMode::Hide);
    frame("mpi");
    CHECK(!find(platSnow, g_rain) && !find(platSnow, g_snow));
    RainSetMode(RainMode::Auto);
    // DCS's mixed band at the carrier (+0.5 C): the rain object at power 0.8 x the view's share, and its snow
    RainSetCarrierTemperature(true, 0.5);
    for (int f = 0; f < 3; ++f) frame("mpi");
    {
        const Upload* p = find(platSnow, g_rain);
        const double share = (0.5 - 0.0065 * 20.5 + 1.0) / 3.0;
        CHECK(p && std::fabs(p->power - 0.8 * share) < 1e-4 && find(platSnow, g_snow));
    }
    // freezing at the carrier: snow only, nothing borrowed
    RainSetCarrierTemperature(true, -5);
    frame("mpi");
    CHECK(!find(platSnow, g_rain) && find(platSnow, g_snow));
    // no temperature from the hook: DCS's split, nothing borrowed
    RainSetCarrierTemperature(false, 0);
    frame("mpi");
    CHECK(!find(platSnow, g_rain) && find(platSnow, g_snow));
    // not installed: nothing borrowed
    RainSetCarrierTemperature(true, 20);
    RainTestSetBorrow(&FakeCollect, false);
    for (int f = 0; f < 3; ++f) frame("mpi");
    CHECK(!find(platSnow, g_rain) && !find(platSnow, g_snow));
    {
        RainTestResetStatsClock();
        CHECK(RainStats().find("no rain object=0, not installed)") != std::string::npos);
    }
    RainSetCarrierTemperature(false, 0);
    RainTestInstalled(false);
    for (int b = 0; b < 4; ++b) NightSkyBuildEnd();
}

// ---- far views: the cloud volume lookup ----
// Stand-ins: GraphicsCore's block stack (index, array of 0xBD8-byte blocks: scale +0x918, offset +0x928), enlight's
// cloud implementation (origin +0x1740, offset +0x1770, scale +0x1780), a ContextBase (camera +0x08, origin +0x2C).
uint32_t g_stackIndex = 1;
alignas(16) unsigned char g_blocks[3][0xBD8];
unsigned char* g_stackArr = &g_blocks[0][0];
alignas(16) unsigned char g_impl[0x2000];
void* g_implPtr = g_impl;
alignas(16) unsigned char g_context[0xD00];

// The cloud volume: 400 km x 20 km, its corner 200 km west / south of your snapped camera.
double g_corner[3] = {}, g_size[3] = {400000, 20000, 400000};
float g_frameO[3] = {};   // the frame origin (your camera snapped to 64 m)

void SetEnlight(const float* frameOrigin) {
    float off[3], sc[3];
    for (int i = 0; i < 3; ++i) {
        sc[i] = static_cast<float>(1.0 / g_size[i]);
        off[i] = static_cast<float>((double(frameOrigin[i]) - g_corner[i]) / g_size[i]);
    }
    memcpy(g_impl + 0x1740, frameOrigin, 12);
    memcpy(g_impl + 0x1770, off, 12);
    memcpy(g_impl + 0x1780, sc, 12);
}

struct GpuView {   // what one cPerView upload carried
    double cam[3];
    float origin[3], off[3], scale[3];
};
std::vector<GpuView> g_gpu;
bool g_upMeddle = false;

unsigned char* Block() { return g_stackArr + g_stackIndex * 0xBD8; }

void FakeUpload(void* context, int slot) {
    CHECK(slot == 7);
    auto c = static_cast<unsigned char*>(context);
    GpuView g;
    memcpy(g.cam, c + 0x08, 24);
    memcpy(g.origin, c + 0x2C, 12);
    memcpy(g.scale, Block() + 0x918, 12);
    memcpy(g.off, Block() + 0x928, 12);
    g_gpu.push_back(g);
    if (g_upMeddle) {
        const float other[3] = {0.25f, 0.5f, 0.75f};
        memcpy(Block() + 0x928, other, 12);
    }
}

// Where the shader looks up the clouds for the camera: uvw = (camera - gOrigin) x scale + offset, as a world position.
void LookedUp(const GpuView& g, double out[3]) {
    for (int i = 0; i < 3; ++i) {
        const double uvw = (g.cam[i] - double(g.origin[i])) * double(g.scale[i]) + double(g.off[i]);
        out[i] = uvw / double(g.scale[i]) + g_corner[i];
    }
}
double Miss(const GpuView& g) {
    double w[3];
    LookedUp(g, w);
    const double d[3] = {w[0] - g.cam[0], w[1] - g.cam[1], w[2] - g.cam[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

void TestFar() {
    printf("far views: cloud lookup\n");
    using UpFn = void (*)(void*, int);
    UpFn up = reinterpret_cast<UpFn>(RainTestUploadWrap());
    RainTestSetFar(&FakeUpload, &g_stackIndex, &g_stackArr, &g_implPtr, true);
    RainTestInstalled(true);
    RainSetMode(RainMode::Auto);
    const double you[3] = {90123.4, 4000.0, -100456.7};      // 115 km from the carrier
    const double plat[3] = {10174.5, 20.5, -183638.0}, island[3] = {10108.5, 61.0, -183593.1};
    const double shadowCam[3] = {10300.0, 3000.0, -183900.0};   // a pass of those views with its own camera
    float carrierO[3];
    for (int i = 0; i < 3; ++i) {
        g_frameO[i] = static_cast<float>(std::floor(you[i] / 64) * 64);
        carrierO[i] = static_cast<float>(std::floor(plat[i] / 64) * 64);
        g_corner[i] = i == 1 ? 0.0 : std::floor(you[i] / 1562.5) * 1562.5 - 200000;
    }
    SetEnlight(g_frameO);
    // one build: ILARTS's views on the carrier's own origin (plat_origin.cpp notes it)
    auto build = [&](bool own) {
        NightSkyNoteView(plat, true);
        NightSkyNoteView(island, false);
        if (own) NightSkyNoteOrigin(carrierO);
        NightSkyBuildEnd();
    };
    // the pass binder: DCS's volume values into the block (setCloudVolumeTransform), the view into the context, upload
    auto bind = [&](const double* cam, const float* origin) {
        memcpy(Block() + 0x918, g_impl + 0x1780, 12);
        memcpy(Block() + 0x928, g_impl + 0x1770, 12);
        memcpy(g_context + 0x08, cam, 24);
        memcpy(g_context + 0x2C, origin, 12);
        up(g_context, 7);
    };
    auto pass = [&](const char* order) {
        g_gpu.clear();
        for (const char* c = order; *c; ++c) {
            if (*c == 'm') bind(you, g_frameO);
            if (*c == 'p') bind(plat, carrierO);
            if (*c == 'i') bind(island, carrierO);
            if (*c == 's') bind(shadowCam, carrierO);
        }
    };
    float dcsOff[3];
    memcpy(dcsOff, g_impl + 0x1770, 12);
    // stock: the bug - the PLAT reads the clouds over you (~115 km away)
    build(true);
    RainSetMode(RainMode::Stock);
    pass("mpis");
    CHECK(g_gpu.size() == 4);
    if (g_gpu.size() == 4) {
        printf("  stock: your view's lookup %.2f m off, the PLAT's %.1f km off\n", Miss(g_gpu[0]), Miss(g_gpu[1]) / 1000);
        CHECK(Miss(g_gpu[0]) < 1.0 && Miss(g_gpu[1]) > 100000);
        for (const GpuView& g : g_gpu) CHECK(memcmp(g.off, dcsOff, 12) == 0);
    }
    // auto / still / hide: ILARTS's views and their passes read the clouds where they are; your view unchanged
    for (RainMode m : {RainMode::Auto, RainMode::Still, RainMode::Hide}) {
        RainSetMode(m);
        for (const char* order : {"mpis", "psim", "pmis"}) {
            pass(order);
            CHECK(g_gpu.size() == 4);
            double worst = 0;
            for (const GpuView& g : g_gpu) {
                if (memcmp(g.cam, you, 24) == 0) {
                    CHECK(memcmp(g.off, dcsOff, 12) == 0);   // DCS's view: exactly DCS's value
                } else {
                    worst = std::fmax(worst, Miss(g));
                }
                CHECK(Miss(g) < 1.0);
            }
            CHECK(memcmp(Block() + 0x928, dcsOff, 12) == 0);   // put back after each upload
            if (m == RainMode::Auto && order[0] == 'm') printf("  auto: ILARTS's views' lookups within %.3f m\n", worst);
        }
    }
    RainSetMode(RainMode::Auto);
    // DCS's values not enlight's of this moment (enlight updated between bind and upload): left alone
    {
        g_gpu.clear();
        memcpy(Block() + 0x918, g_impl + 0x1780, 12);
        memcpy(Block() + 0x928, g_impl + 0x1770, 12);
        float moved[3] = {g_frameO[0] + 64, g_frameO[1], g_frameO[2]};
        SetEnlight(moved);
        memcpy(g_context + 0x08, plat, 24);
        memcpy(g_context + 0x2C, carrierO, 12);
        up(g_context, 7);
        CHECK(g_gpu.size() == 1 && memcmp(g_gpu[0].off, dcsOff, 12) == 0);
        SetEnlight(g_frameO);
    }
    // enlight not readable: left alone
    g_implPtr = nullptr;
    pass("p");
    CHECK(g_gpu.size() == 1 && memcmp(g_gpu[0].off, dcsOff, 12) == 0);
    g_implPtr = g_impl;
    // the offset changed during the upload (another thread): left as found
    g_upMeddle = true;
    pass("p");
    {
        const float other[3] = {0.25f, 0.5f, 0.75f};
        CHECK(memcmp(Block() + 0x928, other, 12) == 0);
    }
    g_upMeddle = false;
    CHECK(Logged("RAIN anomaly: the cloud volume offset changed during a per-view upload"));
    // the whole frame on the carrier's origin (PLAT_ORIGIN 'frame'): nothing to shift
    SetEnlight(carrierO);
    memcpy(dcsOff, g_impl + 0x1770, 12);
    pass("p");
    CHECK(g_gpu.size() == 1 && memcmp(g_gpu[0].off, dcsOff, 12) == 0 && Miss(g_gpu[0]) < 1.0);
    SetEnlight(g_frameO);
    memcpy(dcsOff, g_impl + 0x1770, 12);
    // within 70 km (no own origin for 4 builds): ILARTS's views on the frame origin, untouched and not counted
    for (int b = 0; b < 4; ++b) build(false);
    RainTestResetStatsClock();
    RainStats();
    g_gpu.clear();
    bind(plat, g_frameO);
    CHECK(g_gpu.size() == 1 && memcmp(g_gpu[0].off, dcsOff, 12) == 0 && Miss(g_gpu[0]) < 1.0);
    RainTestResetStatsClock();
    std::string st = RainStats();
    CHECK(st.find("uploads on the carrier's origin=0") != std::string::npos);
    // the lines: a far view sample after each stats line; the rain sample says what the draw's constants carry
    build(true);
    RainTestResetStatsClock();
    RainStats();
    pass("mpis");
    CHECK(Logged("RAIN far view upload (PLAT):"));
    CHECK(Logged("corrected to this view's origin"));
    {
        // a rain draw in the PLAT right after its pass bind
        RainTestSet(&FakeDraw, &Clock);
        View pv;
        pv.Set(0, g_rain);
        pv.Place(plat);
        memcpy(pv.ctx + 0x32C, carrierO, 12);   // the view's own origin (SceneRenderContext +0x32C)
        bind(plat, carrierO);
        WrapFn wrap = reinterpret_cast<WrapFn>(RainTestWrap());
        wrap(pv.rend, pv.ctx, nullptr, nullptr);
        CHECK(Logged("its per-view constants: corrected to this view's origin, own origin "));
        CHECK(Logged("cloud density read 0 m from the camera (DCS's offset: "));
    }
    RainTestResetStatsClock();
    st = RainStats();
    printf("  stats: %s\n", st.c_str());
    CHECK(st.find("far views: uploads on the carrier's origin=") != std::string::npos);
    CHECK(st.find("corrected=0 ") == std::string::npos && st.find("PLAT 0,") == std::string::npos);
    CHECK(st.find("other 0)") == std::string::npos);
    CHECK(st.find("max shift 11") != std::string::npos);   // 80 km east, 83 km north, 4 km down: 115 km
    RainTestSetFar(nullptr, nullptr, nullptr, nullptr, false);
    RainTestInstalled(false);
    for (int b = 0; b < 4; ++b) NightSkyBuildEnd();
}

// ---- 2. the real DLLs ----
int CountRip(const unsigned char* base, size_t size, const unsigned char* op, size_t opLen, uintptr_t target,
             std::vector<uintptr_t>* at) {
    int n = 0;
    for (size_t o = 0; o + opLen + 4 <= size; ++o) {
        if (memcmp(base + o, op, opLen) != 0) continue;
        int32_t rel = 0;
        memcpy(&rel, base + o + opLen, 4);
        if (o + opLen + 4 + rel == target) {
            ++n;
            if (at) at->push_back(o);
        }
    }
    return n;
}

void TestRealDlls(const std::wstring& dcs) {
    printf("Visualizer.dll + Effects2.dll\n");
    const std::wstring vp = dcs + L"\\bin\\Visualizer.dll", ep = dcs + L"\\bin\\Effects2.dll";
    HMODULE vis = LoadLibraryExW(vp.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    HMODULE eff = LoadLibraryExW(ep.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!vis || !eff) {
        printf("  skipped: cannot map them (%lu)\n", GetLastError());
        if (vis) FreeLibrary(vis);
        if (eff) FreeLibrary(eff);
        return;
    }
    auto vb = reinterpret_cast<unsigned char*>(vis);
    auto eb = reinterpret_cast<unsigned char*>(eff);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(vb + reinterpret_cast<const IMAGE_DOS_HEADER*>(vb)->e_lfanew);
    const size_t visSize = nt->OptionalHeader.SizeOfImage;
    // Who uses the renderable's vtable (+0x23BBD0): the two `lea rdx` in the provider's collectRenderables only.
    {
        const unsigned char lea[] = {0x48, 0x8D, 0x15};
        std::vector<uintptr_t> at;
        const int n = CountRip(vb, visSize, lea, sizeof(lea), 0x23BBD0, &at);
        printf("  renderable vtable referenced %d times:", n);
        for (uintptr_t a : at) printf(" +0x%llX", static_cast<unsigned long long>(a));
        printf("\n");
        CHECK(n == 2 && at.size() == 2 && at[0] == 0xC9B11 && at[1] == 0xC9B7F);
        // and its slot 1 is the only pointer to the per-view call in the image
        int ptrs = 0;
        const uint64_t want = reinterpret_cast<uint64_t>(vb + 0xC9C60);
        for (size_t o = 0; o + 8 <= visSize; o += 8) ptrs += memcmp(vb + o, &want, 8) == 0 ? 1 : 0;
        CHECK(ptrs == 1);
        // the provider's vtable (+0x23BB88): slot 1 is the only pointer to collectRenderables (+0xC9900)
        const uint64_t coll = reinterpret_cast<uint64_t>(vb + 0xC9900);
        int collPtrs = 0;
        size_t collAt = 0;
        for (size_t o = 0; o + 8 <= visSize; o += 8)
            if (memcmp(vb + o, &coll, 8) == 0) {
                ++collPtrs;
                collAt = o;
            }
        printf("  collectRenderables pointed to %d time(s), at +0x%llX\n", collPtrs, static_cast<unsigned long long>(collAt));
        CHECK(collPtrs == 1 && collAt == 0x23BB90);
    }
    // Every call through the Rain::draw import (+0x22E0B8) is in the wrapped per-view call.
    {
        const unsigned char call[] = {0xFF, 0x15};
        std::vector<uintptr_t> at;
        const int n = CountRip(vb, visSize, call, sizeof(call), 0x22E0B8, &at);
        const unsigned char mov[] = {0x48, 0x8B, 0x05};
        const int loads = CountRip(vb, visSize, mov, sizeof(mov), 0x22E0B8, nullptr);
        printf("  Rain::draw import called %d time(s)%s\n", n, n == 1 && at[0] == 0xC9D39 ? " (+0xC9D39, the wrapped call)" : "");
        CHECK(n == 1 && at.size() == 1 && at[0] == 0xC9D39 && loads == 0);
    }
    // Every write to cameraVel (+0x150 .. +0x15B, rbx-based disp32) in Rain::update (+0x75A0 .. +0x7CAB) and the
    // private draw (+0x50E0 .. +0x52D5) is one of the two stores behind the update mark (+0x79E5, +0x79F1); the
    // constant buffer upload (+0x7AAD) comes after the mark's skip target (+0x7A1E).
    {
        std::vector<uintptr_t> refs;
        const uintptr_t ranges[2][2] = {{0x50E0, 0x52D5}, {0x75A0, 0x7CAB}};
        for (const auto& r : ranges)
            for (uintptr_t o = r[0] + 1; o + 4 <= r[1]; ++o) {
                int32_t d = 0;
                memcpy(&d, eb + o, 4);
                if (d >= 0x150 && d < 0x15C && (eb[o - 1] & 0xC7) == 0x83) refs.push_back(o - 1);
            }
        printf("  cameraVel referenced at:");
        for (uintptr_t a : refs) printf(" +0x%llX", static_cast<unsigned long long>(a));
        printf("\n");
        // the ModRM bytes of movsd [rbx+150h],xmm4 (+0x79E5) and movss [rbx+158h],xmm2 (+0x79F1); +0x7B4D is not an
        // operand but the rel32 (0x150) of the jae at +0x7B4C (its 0F 83 opcode reads like an rbx ModRM)
        CHECK(refs.size() == 3 && refs[0] == 0x79E8 && refs[1] == 0x79F4 && refs[2] == 0x7B4D);
        CHECK(eb[0x7B4C] == 0x0F && eb[0x7B4D] == 0x83);
    }
    // install: refused while the import is unresolved (as mapped), then with Effects2's export in the slot
    std::string why = RainInstall();
    printf("  install with the import unresolved: %s\n", why.c_str());
    CHECK(why.find("import of Rain::draw") != std::string::npos);
    void* exp = reinterpret_cast<void*>(GetProcAddress(eff, "?draw@Rain@Effects@@QEAAXMMMAEBVVec3d@osg@@AEBVVec3f@4@AEBVMatrixf@4@2_N@Z"));
    CHECK(exp == eb + 0x52E0);
    DWORD old = 0;
    VirtualProtect(vb + 0x22E0B8, 8, PAGE_READWRITE, &old);
    void* saved = nullptr;
    memcpy(&saved, vb + 0x22E0B8, 8);
    memcpy(vb + 0x22E0B8, &exp, 8);
    unsigned char slotBefore[8], provBefore[8];
    memcpy(slotBefore, vb + 0x23BBD8, 8);
    memcpy(provBefore, vb + 0x23BB90, 8);
    for (int mission = 0; mission < 2; ++mission) {
        why = RainInstall();
        printf("  install: %s\n", why.empty() ? "ok" : why.c_str());
        CHECK(why.empty());
        if (!why.empty()) break;
        void* slot = nullptr;
        memcpy(&slot, vb + 0x23BBD8, 8);
        CHECK(slot == RainTestWrap());
        void* prov = nullptr;
        memcpy(&prov, vb + 0x23BB90, 8);
        CHECK(prov == RainTestCollectWrap() && RainTestBorrowInstalled());
        CHECK(Logged("RAIN while you are in snow: the precipitation provider's collectRenderables"));
        CHECK(RainInstall().empty());   // a second install is a no-op
        RainRemove();
        CHECK(memcmp(vb + 0x23BBD8, slotBefore, 8) == 0 && memcmp(vb + 0x23BB90, provBefore, 8) == 0);
        CHECK(!RainTestBorrowInstalled());
    }
    // changed collectRenderables (the rain object's offset): the rest installs, this part is left out
    {
        DWORD o3 = 0;
        VirtualProtect(vb + 0xC9AA8, 1, PAGE_EXECUTE_READWRITE, &o3);
        const unsigned char b = vb[0xC9AA8];
        vb[0xC9AA8] = 0x18;
        why = RainInstall();
        printf("  install on changed collectRenderables: %s, borrow %s\n", why.empty() ? "ok" : why.c_str(),
               RainTestBorrowInstalled() ? "installed" : "not installed");
        CHECK(why.empty() && !RainTestBorrowInstalled() && memcmp(vb + 0x23BB90, provBefore, 8) == 0);
        CHECK(Logged("RAIN while you are in snow: NOT installed (collectRenderables is not the expected code)"));
        RainRemove();
        vb[0xC9AA8] = b;
    }
    // refused on changed code (the update mark test), and when the slot holds something else
    {
        VirtualProtect(eb + 0x75F4, 1, PAGE_EXECUTE_READWRITE, &old);
        const unsigned char b = eb[0x75F4];
        eb[0x75F4] = 0x90;
        why = RainInstall();
        printf("  install on changed Effects2 code: %s\n", why.c_str());
        CHECK(!why.empty() && memcmp(vb + 0x23BBD8, slotBefore, 8) == 0);
        eb[0x75F4] = b;
        DWORD o2 = 0;
        VirtualProtect(vb + 0x23BBD8, 8, PAGE_READWRITE, &o2);
        const uint64_t other = reinterpret_cast<uint64_t>(vb + 0x1000);
        memcpy(vb + 0x23BBD8, &other, 8);
        why = RainInstall();
        printf("  install with another slot value: %s\n", why.c_str());
        CHECK(!why.empty());
        memcpy(vb + 0x23BBD8, slotBefore, 8);
        VirtualProtect(vb + 0xC9C80, 1, PAGE_EXECUTE_READWRITE, &o2);
        const unsigned char c = vb[0xC9C80];
        vb[0xC9C80] = 0x90;
        why = RainInstall();
        printf("  install on changed Visualizer code: %s\n", why.c_str());
        CHECK(!why.empty() && memcmp(vb + 0x23BBD8, slotBefore, 8) == 0);
        vb[0xC9C80] = c;
    }
    memcpy(vb + 0x22E0B8, &saved, 8);
    FreeLibrary(vis);
    FreeLibrary(eff);
}

// Every import of the DLLs and executables in <DCS>\bin whose name contains one of the needles: "file: dll!name".
std::vector<std::string> ImportsIn(const std::wstring& bin, const std::vector<std::string>& needles, int* files) {
    std::vector<std::string> out;
    *files = 0;
    for (const wchar_t* pattern : {L"\\*.dll", L"\\*.exe"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((bin + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring path = bin + L"\\" + fd.cFileName;
            HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            if (!m) continue;
            ++*files;
            auto b = reinterpret_cast<const unsigned char*>(reinterpret_cast<uintptr_t>(m) & ~static_cast<uintptr_t>(3));
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
            if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
                const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
                for (auto d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(b + dir.VirtualAddress);
                     dir.VirtualAddress && d->Name; ++d) {
                    auto t = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
                        b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
                    for (; t->u1.AddressOfData; ++t) {
                        if (IMAGE_SNAP_BY_ORDINAL64(t->u1.Ordinal)) continue;
                        const char* name = reinterpret_cast<const char*>(b + t->u1.AddressOfData + 2);
                        for (const std::string& n : needles)
                            if (strstr(name, n.c_str())) {
                                char line[400];
                                _snprintf_s(line, _countof(line), _TRUNCATE, "%ls: %s!%s", fd.cFileName,
                                            reinterpret_cast<const char*>(b + d->Name), name);
                                out.push_back(line);
                            }
                    }
                }
            }
            FreeLibrary(m);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

size_t ImageSize(const unsigned char* b) {
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew)
        ->OptionalHeader.SizeOfImage;
}

void** IatSlot(HMODULE mod, const char* func) {
    auto b = reinterpret_cast<unsigned char*>(mod);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(b + dir.VirtualAddress); d->Name; ++d) {
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(b + d->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(b + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
            if (!IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) &&
                strcmp(reinterpret_cast<const char*>(b + names->u1.AddressOfData + 2), func) == 0)
                return reinterpret_cast<void**>(&slots->u1.Function);
    }
    return nullptr;
}

void Poke(void* at, const void* v, size_t n) {
    DWORD old = 0;
    VirtualProtect(at, n, PAGE_READWRITE, &old);
    memcpy(at, v, n);
    VirtualProtect(at, n, old, &old);
}

// The far views' cloud lookup on the real DLLs: what it relies on, and install / remove.
void TestRealFar(const std::wstring& dcs) {
    printf("GraphicsCore / GraphicsVista / enlight / SceneRenderer (far views)\n");
    const char* const kUpd = "?updateBuffers@ContextBase@GraphicsCore@@QEAAXH@Z";
    const char* const kSet = "?setCloudVolumeTransform@ContextBase@GraphicsCore@@QEAAXAEBVVec3f@osg@@0@Z";
    // nothing else in DCS uploads the per-view constants or sets their cloud volume fields
    {
        int files = 0;
        const auto imp = ImportsIn(dcs + L"\\bin",
                                   {"updateBuffers@ContextBase", "setCloudVolumeTransform@ContextBase",
                                    "setShadowMatrix@ContextBase", "getPerViewCBuffer@ContextBase"},
                                   &files);
        printf("  %d files in bin; imports of the cPerView uploaders / setters:\n", files);
        for (const auto& l : imp) printf("    %s\n", l.c_str());
        const std::vector<std::string> want = {
            "enlight.dll: GraphicsCore.dll!?updateBuffers@ContextBase@GraphicsCore@@QEAAXH@Z",
            "GraphicsVista.dll: GraphicsCore.dll!?updateBuffers@ContextBase@GraphicsCore@@QEAAXH@Z",
            "ModelViewer2.exe: GraphicsCore.dll!?setShadowMatrix@ContextBase@GraphicsCore@@QEAAXAEBVMatrixf@osg@@@Z",
            "SceneRenderer.dll: GraphicsCore.dll!?setCloudVolumeTransform@ContextBase@GraphicsCore@@QEAAXAEBVVec3f@osg@@0@Z"};
        CHECK(files > 100);
        CHECK(imp.size() == want.size());
        for (const auto& w : want) {
            bool found = false;
            for (const auto& l : imp) found = found || _stricmp(l.c_str(), w.c_str()) == 0;
            CHECK(found);
        }
    }
    const wchar_t* names[] = {L"Visualizer.dll", L"Effects2.dll", L"GraphicsCore.dll", L"GraphicsVista.dll",
                              L"enlight.dll", L"SceneRenderer.dll"};
    HMODULE mod[6] = {};
    for (int i = 0; i < 6; ++i)
        mod[i] = LoadLibraryExW((dcs + L"\\bin\\" + names[i]).c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    for (HMODULE m : mod)
        if (!m) {
            printf("  skipped: cannot map them (%lu)\n", GetLastError());
            for (HMODULE x : mod)
                if (x) FreeLibrary(x);
            return;
        }
    auto vb = reinterpret_cast<unsigned char*>(mod[0]);
    auto eb2 = reinterpret_cast<unsigned char*>(mod[1]);
    auto gb = reinterpret_cast<unsigned char*>(mod[2]);
    auto eb = reinterpret_cast<unsigned char*>(mod[4]);
    // the calls through those imports: GraphicsVista two (the viewport bind, Context::setCamera), enlight one (a
    // context restore), SceneRenderer's setter one (the pass binder); no other reads of the slots
    {
        const unsigned char call[] = {0xFF, 0x15};
        struct Site {
            int m;
            const char* fn;
            std::vector<uintptr_t> want;
        } sites[] = {{3, kUpd, {0x1F45F, 0x205A8}}, {4, kUpd, {0x52809}}, {5, kSet, {0x1EE46}}};
        for (const Site& st : sites) {
            void** slot = IatSlot(mod[st.m], st.fn);
            CHECK(slot != nullptr);
            if (!slot) continue;
            auto b = reinterpret_cast<unsigned char*>(mod[st.m]);
            const uintptr_t rva = reinterpret_cast<unsigned char*>(slot) - b;
            std::vector<uintptr_t> at;
            CountRip(b, ImageSize(b), call, sizeof(call), rva, &at);
            int loads = 0;
            for (int r : {0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D})
                for (int rex : {0x48, 0x4C}) {
                    const unsigned char mov[] = {static_cast<unsigned char>(rex), 0x8B, static_cast<unsigned char>(r)};
                    loads += CountRip(b, ImageSize(b), mov, sizeof(mov), rva, nullptr);
                }
            printf("  %ls: %s called at", names[st.m], st.fn == kUpd ? "updateBuffers" : "setCloudVolumeTransform");
            for (uintptr_t a : at) printf(" +0x%llX", static_cast<unsigned long long>(a));
            printf("\n");
            CHECK(at == st.want && loads == 0);
        }
    }
    // GraphicsCore: no internal call of the uploaders or the setter; the only 0x970-byte cPerView uploads (mov r8d,970h
    // then the buffer's update, call [rax+10h]) are in updateBuffers (+0x19533) and setShadowMatrix (+0x18A9A, which
    // only ModelViewer2 imports); the others create the buffers (init, call [rax+78h])
    {
        const size_t n = ImageSize(gb);
        for (uintptr_t fn : {uintptr_t(0x18BD0), uintptr_t(0x18A20), uintptr_t(0x189D0)})
            for (int op : {0xE8, 0xE9}) {
                const unsigned char o8 = static_cast<unsigned char>(op);
                CHECK(CountRip(gb, n, &o8, 1, fn, nullptr) == 0);
            }
        const unsigned char mov970[] = {0x41, 0xB8, 0x70, 0x09, 0x00, 0x00};
        std::vector<size_t> at;
        const unsigned char update[] = {0xFF, 0x50, 0x10};
        for (size_t o = 0; o + sizeof(mov970) + 12 <= n; ++o) {
            if (memcmp(gb + o, mov970, sizeof(mov970)) != 0) continue;
            for (size_t k = o + sizeof(mov970); k < o + sizeof(mov970) + 9; ++k)
                if (memcmp(gb + k, update, sizeof(update)) == 0) {
                    at.push_back(o);
                    break;
                }
        }
        printf("  GraphicsCore: 0x970-byte uploads at");
        for (size_t a : at) printf(" +0x%zX", a);
        printf("\n");
        CHECK(at.size() == 2 && at[0] == 0x18A9A && at[1] == 0x19533);
        // updateBuffers writes nothing into the cloud volume fields (cPerView +0x6B0 .. +0x6CB) but +0x6BC (gMipLevelBias)
        std::vector<uint32_t> disps;
        for (uintptr_t o = 0x18BD0 + 1; o + 4 <= 0x195BF; ++o) {
            uint32_t d = 0;
            memcpy(&d, gb + o, 4);
            if (d >= 0x6B0 && d < 0x6CC && (gb[o - 1] & 0xC7) == 0x87) disps.push_back(d);
        }
        CHECK(disps.size() == 1 && disps[0] == 0x6BC);
    }
    // enlight: its cloud origin, offset and scale (+0x1740, +0x1770, +0x1780) are stored only by the volume update
    {
        std::vector<uintptr_t> at;
        const size_t n = ImageSize(eb);
        for (size_t o = 2; o + 4 <= n; ++o) {
            uint32_t d = 0;
            memcpy(&d, eb + o, 4);
            const bool field = (d >= 0x1740 && d < 0x174C) || (d >= 0x1770 && d < 0x177C) || (d >= 0x1780 && d < 0x178C);
            if (!field || (eb[o - 1] & 0xC0) != 0x80 || (eb[o - 1] & 7) == 4) continue;
            const unsigned char op = eb[o - 2];
            if (op == 0x11 || op == 0x89) at.push_back(o - 2);   // movss / movsd / movups store, mov store
        }
        printf("  enlight: stores to the volume origin / offset / scale at");
        for (uintptr_t a : at) printf(" +0x%llX", static_cast<unsigned long long>(a));
        printf("\n");
        CHECK(!at.empty());
        for (uintptr_t a : at) CHECK(a >= 0x28490 && a < 0x28B02);
    }
    // install: the far part needs the imports to lead to GraphicsCore's updateBuffers and enlight's facade set up
    void** rainImp = reinterpret_cast<void**>(vb + 0x22E0B8);
    void* rainDraw = reinterpret_cast<void*>(
        GetProcAddress(mod[1], "?draw@Rain@Effects@@QEAAXMMMAEBVVec3d@osg@@AEBVVec3f@4@AEBVMatrixf@4@2_N@Z"));
    void* savedRainImp = *rainImp;
    Poke(rainImp, &rainDraw, 8);
    void** s0 = IatSlot(mod[3], kUpd);
    void** s1 = IatSlot(mod[4], kUpd);
    void* upd = reinterpret_cast<void*>(GetProcAddress(mod[2], kUpd));
    CHECK(upd == gb + 0x18BD0 && s0 && s1);
    if (!(upd && s0 && s1)) {
        for (HMODULE m : mod) FreeLibrary(m);
        return;
    }
    void* saved0 = *s0;
    void* saved1 = *s1;
    std::string why = RainInstall();
    printf("  install with the imports unresolved: %s, far views %s\n", why.empty() ? "ok" : why.c_str(),
           RainTestFarInstalled() ? "on" : "off");
    CHECK(why.empty() && !RainTestFarInstalled());
    CHECK(Logged("cloud lookup NOT installed (an import of updateBuffers (GraphicsVista, enlight) does not lead to GraphicsCore's)"));
    RainRemove();
    Poke(s0, &upd, 8);
    Poke(s1, &upd, 8);
    // enlight's facade object is initialised in the image (its vtable pointer is relocated data): checked as found
    unsigned char* facade = nullptr;
    memcpy(&facade, eb + 0x9A178, 8);
    CHECK(facade == eb + 0x9A1C8);
    void* savedVptr = nullptr;
    memcpy(&savedVptr, facade, 8);
    CHECK(savedVptr == eb + 0x7C850);
    {
        void* other = eb + 0x7B558;   // the base class's vtable (what its destructor leaves)
        Poke(facade, &other, 8);
        why = RainInstall();
        CHECK(why.empty() && !RainTestFarInstalled());
        CHECK(Logged("cloud lookup NOT installed (enlight's clouds facade does not use the expected vtable)"));
        RainRemove();
        Poke(facade, &savedVptr, 8);
    }
    for (int mission = 0; mission < 2; ++mission) {
        why = RainInstall();
        printf("  install: %s, far views %s\n", why.empty() ? "ok" : why.c_str(), RainTestFarInstalled() ? "on" : "off");
        CHECK(why.empty() && RainTestFarInstalled());
        CHECK(*s0 == RainTestUploadWrap() && *s1 == RainTestUploadWrap());
        RainRemove();
        CHECK(*s0 == upd && *s1 == upd && !RainTestFarInstalled());
    }
    // refused on changed code (the far part only: the rest installs) ...
    {
        const unsigned char x = 0x90;
        const unsigned char b = gb[0x189EE];   // setCloudVolumeTransform's +0x928
        Poke(gb + 0x189EE, &x, 1);
        why = RainInstall();
        printf("  install on changed GraphicsCore code: %s, far views %s\n", why.empty() ? "ok" : why.c_str(),
               RainTestFarInstalled() ? "on" : "off");
        CHECK(why.empty() && !RainTestFarInstalled() && *s0 == upd && *s1 == upd);
        CHECK(Logged("GraphicsCore: setCloudVolumeTransform is not the expected code"));
        RainRemove();
        Poke(gb + 0x189EE, &b, 1);
        const unsigned char e = eb[0x28702];   // the update's origin store at +0x1740
        Poke(eb + 0x28702, &x, 1);
        why = RainInstall();
        CHECK(why.empty() && !RainTestFarInstalled());
        CHECK(Logged("enlight: the volume update is not the expected code"));
        RainRemove();
        Poke(eb + 0x28702, &e, 1);
        // ... and nothing installs when Rain::init no longer stores the type at +0x94
        const unsigned char t = eb2[0x5742];
        Poke(eb2 + 0x5742, &x, 1);
        why = RainInstall();
        printf("  install on a changed Rain::init: %s\n", why.c_str());
        CHECK(!why.empty() && *s0 == upd);
        Poke(eb2 + 0x5742, &t, 1);
    }
    Poke(facade, &savedVptr, 8);
    Poke(s0, &saved0, 8);
    Poke(s1, &saved1, 8);
    Poke(rainImp, &savedRainImp, 8);
    for (HMODULE m : mod) FreeLibrary(m);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestWrapper();
    TestType();
    TestHangar();
    TestBorrow();
    TestFar();
    const std::wstring dcs = argc > 1 ? argv[1] : L"C:\\Program Files\\Eagle Dynamics\\DCS World";
    TestRealDlls(dcs);
    TestRealFar(dcs);
    printf(g_fail ? "rain_test: %d FAILED\n" : "rain_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
