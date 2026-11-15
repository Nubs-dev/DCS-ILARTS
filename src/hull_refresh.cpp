#include "hull_refresh.h"

#include "common.h"
#include "compat.h"
#include "engine.h"
#include "lso_camera.h"
#include "plat_cam.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

namespace lsodiag {
namespace {

constexpr uintptr_t kSiteRva = 0xCEE14;     // test al,40h ; je +0xCEF23   (8 bytes), inside +0xCEDE0
constexpr uintptr_t kContRva = 0xCEE1C;     // the call-count throttle for flag 0x40 objects
constexpr uintptr_t kRefreshRva = 0xCEF23;  // the refresh: mov rax,[rsi+10h] ... call +0x125B50 ...
constexpr uintptr_t kMatrixFnRva = 0x125B50;   // object matrix (out, object, parent or null, time)
constexpr uintptr_t kFrameTimeOffset = 0x1D0;  // double in the Visualizer object ctx[2]: this frame's time
constexpr unsigned char kExpect[] = {0xA8, 0x40,                          // test al,40h
                                     0x0F, 0x84, 0x07, 0x01, 0x00, 0x00,  // je +0xCEF23
                                     0xF2, 0x0F, 0x10, 0x47, 0x38};       // movsd xmm0,[rdi+38h]
constexpr size_t kPatchBytes = 8;
constexpr double kNearMetres = 3000.0;      // objects this close to the PLAT camera are refreshed every frame

using MatrixFn = double* (*)(double* out, void* obj, const double* parent, double time);

unsigned char* g_site = nullptr;
unsigned char* g_stub = nullptr;
MatrixFn g_matrix = nullptr;
bool g_installed = false;
volatile unsigned char g_enabled = 0;

// Per scene entry: is its object near the PLAT camera? Re-evaluated every 250 ms (near) / 1 s (far).
struct Slot {
    void* entry = nullptr;
    ULONGLONG tick = 0;
    double forcedTime = -1;   // frame time of the last forced refresh (one per frame)
    unsigned char isNear = 0;
};
Slot g_cache[1024];

// main thread only (the search / hand-off and the Lua hook both run there)
long long g_calls = 0;     // hand-off calls for existing entries
long long g_near = 0;      // of those: object near the PLAT camera
long long g_forced = 0;    // of those: refresh forced (enabled, first call of the frame)
long long g_checks = 0;    // position checks (cache misses)
long long g_noCam = 0;     // no fresh PLAT camera position

// Called by the stub for every existing entry handed to the scene. Non-zero = refresh now (the force
// byte has been set); zero = DCS's own decision.
// ctx = { search descriptor, &object, Visualizer object, &force byte, ... } as built by +0x128950.
unsigned char HullRefreshWanted(void** ctx, void* entry, unsigned /*flags*/) {
    ++g_calls;
    double cam[3], age = 0, synth[16];
    if (PlatCamPose(synth)) {
        cam[0] = synth[12], cam[1] = synth[13], cam[2] = synth[14];
    } else if (!LsoLastCameraPosition(cam, &age) || age > 1.0) {
        ++g_noCam;
        return 0;
    }
    const double frameTime =
        *reinterpret_cast<const double*>(static_cast<const unsigned char*>(ctx[2]) + kFrameTimeOffset);
    Slot& s = g_cache[(reinterpret_cast<uintptr_t>(entry) >> 4) & 1023];
    const ULONGLONG now = GetTickCount64();
    if (s.entry != entry || now - s.tick > (s.isNear ? 250u : 1000u)) {
        double m[16];
        g_matrix(m, *static_cast<void**>(ctx[1]), nullptr, frameTime);   // what DCS calls first on a refresh
        PlatCamNoteObject(*static_cast<void**>(ctx[1]), m, ctx[2]);       // the synthetic camera's carrier
        const double dx = m[12] - cam[0], dy = m[13] - cam[1], dz = m[14] - cam[2];
        if (s.entry != entry) s.forcedTime = -1;
        s.entry = entry;
        s.tick = now;
        s.isNear = dx * dx + dy * dy + dz * dz < kNearMetres * kNearMetres ? 1 : 0;
        ++g_checks;
    }
    if (!s.isNear) return 0;
    ++g_near;
    if (!g_enabled || s.forcedTime == frameTime) return 0;
    s.forcedTime = frameTime;
    *static_cast<unsigned char*>(ctx[3]) = 1;   // DCS's own "force": full refresh without the time throttle
    ++g_forced;
    return 1;
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

bool WriteSite(const unsigned char* bytes) {
    DWORD old = 0;
    if (!VirtualProtect(g_site, kPatchBytes, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(g_site, bytes, kPatchBytes);
    DWORD tmp = 0;
    VirtualProtect(g_site, kPatchBytes, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), g_site, kPatchBytes);
    return true;
}

}  // namespace

std::string HullRefreshInstall() {
    if (g_installed) return "";
    HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
    if (!vis) return "Visualizer.dll not loaded";
    if (!CompatOk()) return CompatWhy();
    unsigned char* base = reinterpret_cast<unsigned char*>(vis);
    unsigned char cur[sizeof(kExpect)];
    if (!SafeRead(base + kSiteRva, cur, sizeof(cur)) || memcmp(cur, kExpect, sizeof(cur)) != 0)
        return "Visualizer+0xCEE14 is not the expected code";
    // +0x125B50 must still start the way it did when its signature was read (ship_trace.cpp checks the
    // same bytes): mov rax,rsp ; mov [rax+8],rbx ; mov [rax+10h],rsi ; mov [rax+18h],rdi - or a jump if
    // the trace tap is installed.
    unsigned char head[3];
    if (!SafeRead(base + kMatrixFnRva, head, sizeof(head))) return "Visualizer+0x125B50 not readable";
    const bool plain = head[0] == 0x48 && head[1] == 0x8B && head[2] == 0xC4;
    const bool tapped = head[0] == 0xFF || head[0] == 0xE9;
    if (!plain && !tapped) return "Visualizer+0x125B50 is not the expected code";
    g_matrix = reinterpret_cast<MatrixFn>(base + kMatrixFnRva);
    g_site = base + kSiteRva;
    if (!g_stub) {
        unsigned char* mem = AllocNear(g_site, 128);
        if (!mem) return "no memory near Visualizer.dll";
        unsigned char code[128];
        size_t n = 0;
        auto put = [&](std::initializer_list<unsigned char> bytes) {
            for (unsigned char c : bytes) code[n++] = c;
        };
        auto movRax = [&](uint64_t v) {
            put({0x48, 0xB8});
            memcpy(code + n, &v, 8);
            n += 8;
        };
        // On entry al = the object's flags (just returned by a virtual call), rsi = ctx, rdi = entry; no
        // other volatile register holds anything that is used later (both successors reload), and rsp is
        // 16-byte aligned as for any call the function makes.
        put({0x50});                          // push rax            (keep the flags value)
        put({0x48, 0x83, 0xEC, 0x28});        // sub rsp,28h         (shadow space, realign)
        put({0x48, 0x8B, 0xCE});              // mov rcx,rsi         (ctx)
        put({0x48, 0x8B, 0xD7});              // mov rdx,rdi         (entry)
        put({0x44, 0x0F, 0xB6, 0xC0});        // movzx r8d,al        (the object's flags)
        movRax(reinterpret_cast<uint64_t>(&HullRefreshWanted));
        put({0xFF, 0xD0});                    // call rax
        put({0x48, 0x83, 0xC4, 0x28});        // add rsp,28h
        put({0x84, 0xC0});                    // test al,al
        put({0x58});                          // pop rax             (does not change the flags)
        put({0x75, 0x00});                    // jnz refresh         (forced)
        const size_t jnzRefresh = n;
        put({0xA8, 0x40});                    // test al,40h         (DCS's own test)
        put({0x74, 0x00});                    // jz refresh
        const size_t jzRefresh = n;
        movRax(reinterpret_cast<uint64_t>(base + kContRva));
        put({0xFF, 0xE0});                    // jmp rax             (call-count throttle)
        const size_t refresh = n;
        movRax(reinterpret_cast<uint64_t>(base + kRefreshRva));
        put({0xFF, 0xE0});                    // jmp rax
        code[jnzRefresh - 1] = static_cast<unsigned char>(refresh - jnzRefresh);
        code[jzRefresh - 1] = static_cast<unsigned char>(refresh - jzRefresh);
        if (n > sizeof(code)) return "stub layout";
        memcpy(mem, code, n);
        DWORD old = 0;
        if (!VirtualProtect(mem, 128, PAGE_EXECUTE_READ, &old)) return "VirtualProtect (stub) failed";
        FlushInstructionCache(GetCurrentProcess(), mem, 128);
        g_stub = mem;
    }
    const long long rel = reinterpret_cast<intptr_t>(g_stub) - reinterpret_cast<intptr_t>(g_site + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return "stub out of rel32 range";
    unsigned char nb[kPatchBytes] = {0xE9, 0, 0, 0, 0, 0x0F, 0x1F, 0x00};  // jmp stub ; nop (3 bytes)
    const int32_t r32 = static_cast<int32_t>(rel);
    memcpy(nb + 1, &r32, 4);
    if (!WriteSite(nb)) return "could not patch Visualizer+0xCEE14";
    g_installed = true;
    Log("HULLREFRESH installed: Visualizer+0xCEE14 -> stub %p (DCS's own flow while disabled)", g_stub);
    return "";
}

void HullRefreshRemove() {
    g_enabled = 0;
    if (!g_installed) return;
    if (WriteSite(kExpect)) g_installed = false;
}

void HullRefreshEnable(bool on) { g_enabled = on ? 1 : 0; }

std::string HullRefreshStats() {
    char b[260];
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "installed=%d enabled=%d handoffs=%lld near_plat=%lld forced=%lld position_checks=%lld no_plat_camera=%lld",
                g_installed ? 1 : 0, g_enabled ? 1 : 0, g_calls, g_near, g_forced, g_checks, g_noCam);
    g_calls = g_near = g_forced = g_checks = g_noCam = 0;
    return b;
}

}  // namespace lsodiag
