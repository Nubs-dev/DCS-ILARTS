#include "ship_trace.h"

#include "common.h"
#include "engine.h"

#include <windows.h>

#include <intrin.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace lsodiag {
namespace {

constexpr const char* kVerifiedTransport = kVerifiedBuild;
// woShip::Position(double t) -> const wPosition3<float>&. (woShip::Parse +0x13C8C0 was tapped first: it
// is never called for the carrier, run 03:19.)
constexpr uintptr_t kParseRva = 0x13C980;
// mov [rsp+8],rbx ; push rbp ; lea rbp,[rsp-57h] ; sub rsp,100h   (position independent, 18 bytes)
constexpr unsigned char kExpect[18] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x48, 0x8D, 0x6C,
                                       0x24, 0xA9, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00};

using PositionFn = const float* (*)(void* self, double t);

unsigned char* g_site = nullptr;
unsigned char* g_tramp = nullptr;
bool g_installed = false;

// edObjects.dll viObjectShape::getObjectPosition() const -> osg::Matrixd (hidden return pointer): the
// matrix the scene draws a shape object with. Rotation from the object's no-argument Position(),
// position from a separate double getter.
constexpr uintptr_t kDrawRva = 0x10340;
// mov [rsp+8],rbx ; push rdi ; sub rsp,40h ; mov rax,[rcx-48h] ; lea rbx,[rcx-78h]   (18 bytes)
constexpr unsigned char kDrawExpect[18] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC,
                                           0x40, 0x48, 0x8B, 0x41, 0xB8, 0x48, 0x8D, 0x59, 0x88};
using DrawFn = double* (*)(void* self, double* out);
unsigned char* g_drawSite = nullptr;
unsigned char* g_drawTramp = nullptr;
bool g_drawInstalled = false;

struct DrawRec {
    volatile LONG calls = 0;
    double m[16] = {};
    double first[16] = {};
    double diff = 0;
    LARGE_INTEGER firstQpc{};
    double positionTimeAtFirst = 0;  // time argument of the latest woShip::Position call before the first draw
    // every call since the previous read (up to kMaxCalls): when, which thread, who, which pose
    struct Call {
        LARGE_INTEGER qpc;
        DWORD tid;
        const void* ra;
        double poseTime, x, z, m0, m2;
        double arg;  // Visualizer tap: the time argument; -1 for the edObjects tap
        int parent;  // Visualizer tap: 1 when a parent matrix was passed
    } call[24];
} g_draw;
LARGE_INTEGER g_newTimeQpc{};  // when woShip::Position was first called with a later time than before
double g_lastT = 0;

struct Rec {
    volatile LONG calls = 0;
    void* self = nullptr;
    void* others[4] = {};
    int nOthers = 0;
    float pos[16] = {};
    float first[16] = {};
    float diff = 0;
    double poseTime = 0, stateTime = 0, tMin = 0, tMax = 0;
    float yawRate = 0;
    double refX = 0, refZ = 0;
    bool haveRef = false;
} g_rec;

const float* HookParse(void* self, double t) {
    const float* pos = reinterpret_cast<PositionFn>(g_tramp)(self, t);
    if (pos && g_rec.self == self && t > g_lastT) {
        g_lastT = t;
        QueryPerformanceCounter(&g_newTimeQpc);
    }
    if (pos && g_rec.haveRef) {
        const double dx = double(pos[12]) - g_rec.refX, dz = double(pos[14]) - g_rec.refZ;
        if (dx * dx + dz * dz < 600.0 * 600.0) {
            if (g_rec.calls == 0) {
                memcpy(g_rec.first, pos, sizeof(g_rec.first));
                g_rec.diff = 0;
                g_rec.tMin = g_rec.tMax = t;
            } else {
                if (t < g_rec.tMin) g_rec.tMin = t;
                if (t > g_rec.tMax) g_rec.tMax = t;
                for (int i = 0; i < 15; ++i) {
                    const float d = fabsf(pos[i] - g_rec.first[i]);
                    if (d > g_rec.diff) g_rec.diff = d;
                }
            }
            memcpy(g_rec.pos, pos, sizeof(g_rec.pos));
            g_rec.self = self;
            g_rec.poseTime = t;
            SafeRead(static_cast<const unsigned char*>(self) + 0x260, &g_rec.stateTime, sizeof(double));
            SafeRead(static_cast<const unsigned char*>(self) + 0x1E4, &g_rec.yawRate, sizeof(float));
            InterlockedIncrement(&g_rec.calls);
        } else {
            bool known = false;
            for (int i = 0; i < g_rec.nOthers; ++i) known = known || g_rec.others[i] == self;
            if (!known && g_rec.nOthers < 4) g_rec.others[g_rec.nOthers++] = self;
        }
    }
    return pos;
}

void NoteCall(const void* ra, const double* r, double arg, int parent) {
    const LONG k = InterlockedIncrement(&g_draw.calls) - 1;
    if (k >= 0 && k < 24) {
        DrawRec::Call& c = g_draw.call[k];
        QueryPerformanceCounter(&c.qpc);
        c.tid = GetCurrentThreadId();
        c.ra = ra;
        c.poseTime = g_rec.poseTime;
        c.x = r[12];
        c.z = r[14];
        c.m0 = r[0];
        c.m2 = r[2];
        c.arg = arg;
        c.parent = parent;
    }
}

// Visualizer.dll +0x125B50 (out matrix, object, parent matrix or null, time): the object matrix Visualizer
// builds when it hands objects to the scene (8 call sites).
constexpr uintptr_t kMatRva = 0x125B50;
// mov rax,rsp ; mov [rax+8],rbx ; mov [rax+10h],rsi ; mov [rax+18h],rdi   (15 bytes)
constexpr unsigned char kMatExpect[15] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18};
using MatFn = double* (*)(double* out, void* obj, const double* parent, double time);
unsigned char* g_matSite = nullptr;
unsigned char* g_matTramp = nullptr;
bool g_matInstalled = false;

double* HookMat(double* out, void* obj, const double* parent, double time) {
    const void* ra = _ReturnAddress();
    double* r = reinterpret_cast<MatFn>(g_matTramp)(out, obj, parent, time);
    if (out && g_rec.self && fabs(out[12] - double(g_rec.pos[12])) < 1.0 && fabs(out[14] - double(g_rec.pos[14])) < 1.0 &&
        fabs(out[13] - double(g_rec.pos[13])) < 1.0)
        NoteCall(ra, out, time, parent ? 1 : 0);
    return r;
}

double* HookDraw(void* self, double* out) {
    const void* ra = _ReturnAddress();
    double* r = reinterpret_cast<DrawFn>(g_drawTramp)(self, out);
    // the hull: the shape whose origin is the ship's own position
    if (r && g_rec.self && fabs(r[12] - double(g_rec.pos[12])) < 1.0 && fabs(r[14] - double(g_rec.pos[14])) < 1.0 &&
        fabs(r[13] - double(g_rec.pos[13])) < 1.0) {
        if (g_draw.calls == 0) {
            memcpy(g_draw.first, r, sizeof(g_draw.first));
            g_draw.diff = 0;
            QueryPerformanceCounter(&g_draw.firstQpc);
            g_draw.positionTimeAtFirst = g_rec.poseTime;
        } else {
            for (int i = 0; i < 15; ++i) {
                const double d = fabs(r[i] - g_draw.first[i]);
                if (d > g_draw.diff) g_draw.diff = d;
            }
        }
        memcpy(g_draw.m, r, sizeof(g_draw.m));
        NoteCall(ra, r, -1.0, 0);
    }
    return r;
}

bool WriteAt(unsigned char* site, const unsigned char* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(site, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site, bytes, n);
    DWORD tmp = 0;
    VirtualProtect(site, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), site, n);
    return true;
}
bool WriteSite(const unsigned char* bytes, size_t n) { return WriteAt(g_site, bytes, n); }

void PutAbsJmp(unsigned char* at, const void* target) {  // jmp [rip+0] ; target (14 bytes)
    const unsigned char op[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, op, 6);
    memcpy(at + 6, &target, 8);
}

}  // namespace

std::string ShipTraceInstall() {
    if (g_installed) return "";
    HMODULE tr = GetModuleHandleW(L"Transport.dll");
    if (!tr) return "Transport.dll not loaded";
    const std::string ver = FileVersion(ModulePath(tr));
    if (ver != kVerifiedTransport) return "Transport.dll is " + ver + ", only verified for " + kVerifiedTransport;
    unsigned char* site = reinterpret_cast<unsigned char*>(tr) + kParseRva;
    unsigned char cur[sizeof(kExpect)];
    if (!SafeRead(site, cur, sizeof(cur)) || memcmp(cur, kExpect, sizeof(cur)) != 0) return "Transport+0x13C980 is not the expected code";
    g_site = site;
    if (!g_tramp) {
        unsigned char* mem = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!mem) return "no memory for the trampoline";
        memcpy(mem, kExpect, sizeof(kExpect));
        PutAbsJmp(mem + sizeof(kExpect), site + sizeof(kExpect));
        DWORD old = 0;
        if (!VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &old)) return "VirtualProtect (trampoline) failed";
        FlushInstructionCache(GetCurrentProcess(), mem, 64);
        g_tramp = mem;
    }
    unsigned char nb[sizeof(kExpect)];
    PutAbsJmp(nb, reinterpret_cast<const void*>(&HookParse));
    for (size_t i = 14; i < sizeof(nb); ++i) nb[i] = 0x90;
    if (!WriteSite(nb, sizeof(nb))) return "could not patch Transport+0x13C980";
    g_installed = true;
    return "";
}

std::string ShipDrawInstall() {
    if (g_drawInstalled) return "";
    HMODULE ed = GetModuleHandleW(L"edObjects.dll");
    if (!ed) return "edObjects.dll not loaded";
    const std::string ver = FileVersion(ModulePath(ed));
    if (ver != kVerifiedTransport) return "edObjects.dll is " + ver + ", only verified for " + kVerifiedTransport;
    unsigned char* site = reinterpret_cast<unsigned char*>(ed) + kDrawRva;
    unsigned char cur[sizeof(kDrawExpect)];
    if (!SafeRead(site, cur, sizeof(cur)) || memcmp(cur, kDrawExpect, sizeof(cur)) != 0) return "edObjects+0x10340 is not the expected code";
    g_drawSite = site;
    if (!g_drawTramp) {
        unsigned char* mem = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!mem) return "no memory for the trampoline";
        memcpy(mem, kDrawExpect, sizeof(kDrawExpect));
        PutAbsJmp(mem + sizeof(kDrawExpect), site + sizeof(kDrawExpect));
        DWORD old = 0;
        if (!VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &old)) return "VirtualProtect (trampoline) failed";
        FlushInstructionCache(GetCurrentProcess(), mem, 64);
        g_drawTramp = mem;
    }
    unsigned char nb[sizeof(kDrawExpect)];
    PutAbsJmp(nb, reinterpret_cast<const void*>(&HookDraw));
    for (size_t i = 14; i < sizeof(nb); ++i) nb[i] = 0x90;
    if (!WriteAt(g_drawSite, nb, sizeof(nb))) return "could not patch edObjects+0x10340";
    g_drawInstalled = true;
    return "";
}

std::string ShipMatInstall() {
    if (g_matInstalled) return "";
    HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
    if (!vis) return "Visualizer.dll not loaded";
    const std::string ver = FileVersion(ModulePath(vis));
    if (ver != kVerifiedTransport) return "Visualizer.dll is " + ver + ", only verified for " + kVerifiedTransport;
    unsigned char* site = reinterpret_cast<unsigned char*>(vis) + kMatRva;
    unsigned char cur[sizeof(kMatExpect)];
    if (!SafeRead(site, cur, sizeof(cur)) || memcmp(cur, kMatExpect, sizeof(cur)) != 0) return "Visualizer+0x125B50 is not the expected code";
    g_matSite = site;
    if (!g_matTramp) {
        unsigned char* mem = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!mem) return "no memory for the trampoline";
        memcpy(mem, kMatExpect, sizeof(kMatExpect));
        PutAbsJmp(mem + sizeof(kMatExpect), site + sizeof(kMatExpect));
        DWORD old = 0;
        if (!VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &old)) return "VirtualProtect (trampoline) failed";
        FlushInstructionCache(GetCurrentProcess(), mem, 64);
        g_matTramp = mem;
    }
    unsigned char nb[sizeof(kMatExpect)];
    PutAbsJmp(nb, reinterpret_cast<const void*>(&HookMat));
    nb[14] = 0x90;
    if (!WriteAt(g_matSite, nb, sizeof(nb))) return "could not patch Visualizer+0x125B50";
    g_matInstalled = true;
    return "";
}

std::string ShipDrawLast() {
    char b[520];
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double* m = g_draw.m;
    DrawRec::Call copy[24];
    memcpy(copy, g_draw.call, sizeof(copy));
    const long calls = InterlockedExchange(&g_draw.calls, 0);
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "d=%ld dq=%.6f dpt=%.6f ddiff=%.6f simq=%.6f | %.8f %.8f %.8f %.8f %.8f %.8f %.8f %.8f %.8f %.4f %.4f %.4f |", calls,
                double(g_draw.firstQpc.QuadPart) / double(f.QuadPart), g_draw.positionTimeAtFirst, g_draw.diff,
                double(g_newTimeQpc.QuadPart) / double(f.QuadPart), m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
    std::string s = b;
    // per call: qpc thread caller pose-time x z xaxis.x xaxis.z
    for (long k = 0; k < calls && k < 24; ++k) {
        const DrawRec::Call& c = copy[k];
        _snprintf_s(b, _countof(b), _TRUNCATE, " %.6f %lu %s %.4f %.4f %.4f %.8f %.8f %.6f %d ;", double(c.qpc.QuadPart) / double(f.QuadPart),
                    static_cast<unsigned long>(c.tid), DescribeAddress(c.ra).c_str(), c.poseTime, c.x, c.z, c.m0, c.m2, c.arg, c.parent);
        s += b;
    }
    return s;
}

void ShipTraceRemove() {
    if (g_matInstalled && WriteAt(g_matSite, kMatExpect, sizeof(kMatExpect))) g_matInstalled = false;
    if (g_drawInstalled && WriteAt(g_drawSite, kDrawExpect, sizeof(kDrawExpect))) g_drawInstalled = false;
    if (!g_installed) return;
    if (WriteSite(kExpect, sizeof(kExpect))) g_installed = false;
}

void ShipTraceSetRef(double x, double z) {
    g_rec.refX = x;
    g_rec.refZ = z;
    g_rec.haveRef = true;
}

std::string ShipTraceLast() {
    char b[480];
    const float* p = g_rec.pos;
    const long calls = InterlockedExchange(&g_rec.calls, 0);
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "tid=%lu n=%ld ships=%d t=%.6f t0=%.6f rate=%.7f diff=%.6f tmin=%.6f tmax=%.6f | %.8f %.8f %.8f %.8f %.8f %.8f %.8f %.8f %.8f %.4f %.4f %.4f",
                static_cast<unsigned long>(GetCurrentThreadId()), calls,
                g_rec.nOthers + (g_rec.self ? 1 : 0), g_rec.poseTime, g_rec.stateTime, g_rec.yawRate, g_rec.diff, g_rec.tMin, g_rec.tMax, p[0], p[1], p[2], p[4],
                p[5], p[6], p[8], p[9], p[10], p[12], p[13], p[14]);
    return b;
}

}  // namespace lsodiag
