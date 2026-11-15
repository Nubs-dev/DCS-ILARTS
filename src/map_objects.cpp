#include "map_objects.h"

#include "common.h"
#include "compat.h"
#include "engine.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

namespace lsodiag {
namespace {

constexpr uintptr_t kSiteRva = 0x199D2F;       // cmp qword [+0x2E1968],0 (8 bytes) ; je +0x199D77
constexpr uintptr_t kAfterSiteRva = 0x199D37;  // the je
constexpr uintptr_t kAfterCallRva = 0x199D6D;  // instruction after DCS's own call of +0x19F8C0
constexpr uintptr_t kSearchFnRva = 0x19F8C0;
constexpr uintptr_t kGatePtrRva = 0x2E1968;
constexpr uintptr_t kFlagRva = 0x2EA07C;       // cmp dword [..],-1 ; setg r9b
constexpr uintptr_t kTimeRva = 0x2E1778;       // double, 5th argument
constexpr uintptr_t kMainCamRva = 0x2E1790;    // 0x80 bytes: the frame's stored main camera matrix
// From the null test to DCS's own call (unchanged code, checked byte for byte before patching).
constexpr unsigned char kExpect[] = {
    0x48, 0x83, 0x3D, 0x31, 0x7C, 0x14, 0x00, 0x00,              // cmp qword [rip+..],0
    0x74, 0x3E,                                                  // je +0x199D77
    0x83, 0x3D, 0x3C, 0x03, 0x15, 0x00, 0xFF,                    // cmp dword [rip+..],-1
    0x41, 0x0F, 0x9F, 0xC1,                                      // setg r9b
    0xF3, 0x44, 0x0F, 0x11, 0x5C, 0x24, 0x28,                    // movss [rsp+28h],xmm11
    0xF2, 0x0F, 0x10, 0x05, 0x25, 0x7A, 0x14, 0x00,              // movsd xmm0,[rip+..]
    0xF2, 0x0F, 0x11, 0x44, 0x24, 0x20,                          // movsd [rsp+20h],xmm0
    0x4C, 0x8D, 0x44, 0x24, 0x60,                                // lea r8,[rsp+60h]
    0x48, 0x8D, 0x15, 0xEB, 0xE9, 0x13, 0x00,                    // lea rdx,[rip+..]
    0x48, 0x8B, 0xCF,                                            // mov rcx,rdi
    0xE8, 0x53, 0x5B, 0x00, 0x00};                               // call +0x19F8C0
constexpr size_t kPatchBytes = 8;

unsigned char* g_site = nullptr;
unsigned char* g_stub = nullptr;
bool g_installed = false;
volatile unsigned char g_enabled = 0;
volatile LONG64 g_calls = 0;      // times the stub ran the search
volatile LONG64 g_noCamera = 0;   // enabled, but the view-mode object was missing

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

std::string MapObjectsInstall() {
    if (g_installed) return "";
    HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
    if (!vis) return "Visualizer.dll not loaded";
    if (!CompatOk()) return CompatWhy();
    unsigned char* base = reinterpret_cast<unsigned char*>(vis);
    unsigned char cur[sizeof(kExpect)];
    if (!SafeRead(base + kSiteRva, cur, sizeof(cur)) || memcmp(cur, kExpect, sizeof(cur)) != 0)
        return "Visualizer+0x199D2F is not the expected code";
    g_site = base + kSiteRva;
    if (!g_stub) {
        unsigned char* mem = AllocNear(g_site, 256);
        if (!mem) return "no memory near Visualizer.dll";
        unsigned char code[256];
        size_t n = 0;
        auto put = [&](std::initializer_list<unsigned char> bytes) {
            for (unsigned char c : bytes) code[n++] = c;
        };
        auto movRax = [&](uint64_t v) {
            put({0x48, 0xB8});
            memcpy(code + n, &v, 8);
            n += 8;
        };
        auto abs = [&](uintptr_t rva) { return reinterpret_cast<uint64_t>(base + rva); };
        // Layout (all short jumps within range):
        //   gate test -> back (DCS's own flow) | cont
        //   back / nocam / skip blocks, then cont: the enabled and view-object tests and the call.
        movRax(abs(kGatePtrRva));
        put({0x48, 0x83, 0x38, 0x00});                    // cmp qword [rax],0
        put({0x74, 0x00});                                // je cont
        const size_t jeCont = n;
        const size_t back = n;                            // flags decide the je at the return address
        movRax(abs(kAfterSiteRva));                       // (mov and jmp do not change the flags)
        put({0xFF, 0xE0});                                // jmp rax
        const size_t noCam = n;
        movRax(reinterpret_cast<uint64_t>(&g_noCamera));
        put({0x48, 0xFF, 0x00});                          // inc qword [rax]
        const size_t skip = n;
        put({0x31, 0xC0});                                // xor eax,eax (ZF=1: the je skips the call)
        put({0xEB, 0x00});                                // jmp back
        const size_t jmpBack = n;
        const size_t cont = n;
        movRax(reinterpret_cast<uint64_t>(&g_enabled));
        put({0x80, 0x38, 0x00});                          // cmp byte [rax],0
        put({0x74, 0x00});                                // je skip
        const size_t jeOff = n;
        put({0x48, 0x8B, 0x87, 0x40, 0x03, 0x00, 0x00});  // mov rax,[rdi+340h]  (view-mode object, mode 5)
        put({0x48, 0x85, 0xC0});                          // test rax,rax
        put({0x74, 0x00});                                // je nocam
        const size_t jeNoCam = n;
        movRax(reinterpret_cast<uint64_t>(&g_calls));
        put({0x48, 0xFF, 0x00});                          // inc qword [rax]
        movRax(abs(kFlagRva));
        put({0x83, 0x38, 0xFF});                          // cmp dword [rax],-1
        put({0x41, 0x0F, 0x9F, 0xC1});                    // setg r9b
        put({0xF3, 0x44, 0x0F, 0x11, 0x5C, 0x24, 0x28});  // movss [rsp+28h],xmm11
        movRax(abs(kTimeRva));
        put({0xF2, 0x0F, 0x10, 0x00});                    // movsd xmm0,[rax]
        put({0xF2, 0x0F, 0x11, 0x44, 0x24, 0x20});        // movsd [rsp+20h],xmm0
        put({0x4C, 0x8D, 0x44, 0x24, 0x60});              // lea r8,[rsp+60h]
        put({0x48, 0xBA});                                // mov rdx,imm64 (stored main camera)
        const uint64_t cam = abs(kMainCamRva);
        memcpy(code + n, &cam, 8);
        n += 8;
        put({0x48, 0x8B, 0xCF});                          // mov rcx,rdi
        movRax(abs(kSearchFnRva));
        put({0xFF, 0xD0});                                // call rax
        movRax(abs(kAfterCallRva));
        put({0xFF, 0xE0});                                // jmp rax
        auto rel8 = [&](size_t from, size_t to) {
            code[from - 1] = static_cast<unsigned char>(static_cast<int>(to) - static_cast<int>(from));
        };
        rel8(jeCont, cont);
        rel8(jmpBack, back);
        rel8(jeOff, skip);
        rel8(jeNoCam, noCam);
        if (n > sizeof(code)) return "stub layout";
        memcpy(mem, code, n);
        DWORD old = 0;
        if (!VirtualProtect(mem, 256, PAGE_EXECUTE_READ, &old)) return "VirtualProtect (stub) failed";
        FlushInstructionCache(GetCurrentProcess(), mem, 256);
        g_stub = mem;
    }
    const long long rel = reinterpret_cast<intptr_t>(g_stub) - reinterpret_cast<intptr_t>(g_site + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return "stub out of rel32 range";
    unsigned char nb[kPatchBytes] = {0xE9, 0, 0, 0, 0, 0x0F, 0x1F, 0x00};  // jmp stub ; nop (3 bytes)
    const int32_t r32 = static_cast<int32_t>(rel);
    memcpy(nb + 1, &r32, 4);
    if (!WriteSite(nb)) return "could not patch Visualizer+0x199D2F";
    g_installed = true;
    return "";
}

void MapObjectsRemove() {
    g_enabled = 0;
    if (!g_installed) return;
    if (WriteSite(kExpect)) g_installed = false;
}

void MapObjectsEnable(bool on) { g_enabled = on ? 1 : 0; }

std::string MapObjectsStats() {
    char b[160];
    _snprintf_s(b, _countof(b), _TRUNCATE, "installed=%d enabled=%d searches=%lld no_view_object=%lld", g_installed ? 1 : 0,
                g_enabled ? 1 : 0, static_cast<long long>(InterlockedExchange64(&g_calls, 0)),
                static_cast<long long>(InterlockedExchange64(&g_noCamera, 0)));
    return b;
}

}  // namespace lsodiag
