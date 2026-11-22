// Offline test of src/plat_ui.cpp's state recording and search (no DCS running):
// DCS's lua.dll (plain Lua 5.1) is loaded from the install, read only, and real Lua states stand in for the
// dxgui states; PlatUiTestRecord does what the lua-dxgui import hook does for a widget call. Checks that an
// unknown state is probed once per sighting and only while it is in use, that coroutines are folded into
// their main thread (a suspended coroutine is never called into), that a found state is kept while idle and
// that closed / quiet / other-thread states are left alone.
//
//   plat_ui_test.exe ["C:\Program Files\Eagle Dynamics\DCS World"]
#include "../src/plat_ui.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lsodiag {
void Log(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    printf("  log: ");
    vprintf(format, ap);
    printf("\n");
    va_end(ap);
}
bool SafeRead(const void* address, void* out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &got) && got == size;
}
void** FindIatSlot(HMODULE, const char*, const char*) { return nullptr; }
bool PatchSlot(void**, void*) { return false; }
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

struct lua_State;
struct Lua {
    lua_State* (*newstate)() = nullptr;
    void (*openlibs)(lua_State*) = nullptr;
    int (*loadstring)(lua_State*, const char*) = nullptr;
    int (*pcall)(lua_State*, int, int, int) = nullptr;
    void (*close)(lua_State*) = nullptr;
    lua_State* (*newthread)(lua_State*) = nullptr;
    int (*resume)(lua_State*, int) = nullptr;
    void (*settop)(lua_State*, int) = nullptr;
    int (*gettop)(lua_State*) = nullptr;
    const char* (*tolstring)(lua_State*, int, size_t*) = nullptr;
    double (*tonumber)(lua_State*, int) = nullptr;
    void (*pushnumber)(lua_State*, double) = nullptr;
} lua;

unsigned long long g_clock = 100000;
unsigned long long Clock() { return g_clock; }

// Runs a chunk in L, returns its first result as text (or the error).
std::string Run(lua_State* L, const char* code) {
    const int top = lua.gettop(L);
    std::string r;
    if (lua.loadstring(L, code) != 0 || lua.pcall(L, 0, 1, 0) != 0) {
        const char* e = lua.tolstring(L, -1, nullptr);
        r = std::string("error: ") + (e ? e : "?");
    } else {
        const char* s = lua.tolstring(L, -1, nullptr);
        r = s ? s : "nil";
    }
    lua.settop(L, top);
    return r;
}

lua_State* NewState(const char* code) {
    lua_State* L = lua.newstate();
    lua.openlibs(L);
    if (code) Run(L, code);
    return L;
}

// A state whose global lookups of getPLATCamera are counted (what a probe does first).
const char kCounted[] =
    "probes = 0\n"
    "setmetatable(_G, { __index = function(t, k) if k == 'getPLATCamera' then rawset(t, 'probes', rawget(t, 'probes') + 1) end end })";
const char kPlat[] =
    "function getPLATCamera() end\n"
    "function setTime(h, m, s) lastTime = h * 3600 + m * 60 + s end";

bool Starts(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

DWORD WINAPI RecordOnOtherThread(void* L) {
    PlatUiTestRecord(L);
    return 0;
}

void TestProbedOnce() {
    printf("unknown state: probed once per sighting, only while in use\n");
    PlatUiReset();
    lua_State* A = NewState(kCounted);
    const long long p0 = PlatUiTestProbes();
    for (int i = 0; i < 5; ++i) {  // in use (a widget call every 100 ms), the hook asks once a second
        PlatUiTestRecord(A);
        const std::string st = PlatUiStatus();
        CHECK(Starts(st, "PLAT UI state not seen yet (1 dxgui states recorded, 0 on other threads)"));
        g_clock += 100;
    }
    CHECK(PlatUiTestProbes() - p0 == 1);
    CHECK(Run(A, "return probes") == "1");
    // quiet for longer than the gap, then used again: a new sighting, probed once more
    g_clock += 6000;
    PlatUiTestRecord(A);
    PlatUiStatus();
    PlatUiStatus();
    CHECK(PlatUiTestProbes() - p0 == 2);
    CHECK(Run(A, "return probes") == "2");
    PlatUiReset();
    lua.close(A);
}

void TestQuietNotProbed() {
    printf("a state that has gone quiet is not probed\n");
    PlatUiReset();
    lua_State* Q = NewState(kCounted);
    const long long p0 = PlatUiTestProbes();
    PlatUiTestRecord(Q);
    g_clock += 3000;  // older than the 2 s window: may have been closed
    PlatUiStatus();
    CHECK(PlatUiTestProbes() == p0);
    PlatUiTestRecord(Q);  // in use again (3 s gap < 5 s: same sighting)
    PlatUiStatus();
    CHECK(PlatUiTestProbes() - p0 == 1);
    CHECK(Run(Q, "return probes") == "1");
    PlatUiReset();
    lua.close(Q);
}

void TestClosedStateLeftAlone() {
    printf("a state probed once and then closed is never called again\n");
    PlatUiReset();
    lua_State* Z = NewState(nullptr);
    const long long p0 = PlatUiTestProbes();
    PlatUiTestRecord(Z);
    PlatUiStatus();
    CHECK(PlatUiTestProbes() - p0 == 1);
    lua.close(Z);  // its memory is freed; the entry stays until the next reset
    for (int i = 0; i < 10; ++i) {
        PlatUiStatus();
        g_clock += 1000;
    }
    CHECK(PlatUiTestProbes() - p0 == 1);
    PlatUiReset();
}

void TestFoundAndKept() {
    printf("PLAT UI state: found, called, kept while idle, re-found after a new sighting\n");
    PlatUiReset();
    lua_State* other = NewState(nullptr);
    lua_State* P = NewState(kPlat);
    const long long p0 = PlatUiTestProbes();
    PlatUiTestRecord(other);
    PlatUiTestRecord(P);
    std::string st = PlatUiStatus();
    char want[64];
    _snprintf_s(want, _countof(want), _TRUNCATE, "PLAT UI state %p", static_cast<void*>(P));
    CHECK(st == want);
    CHECK(PlatUiTestProbes() - p0 == 2);
    std::vector<PlatUiArg> args(3);
    for (int i = 0; i < 3; ++i) {
        args[i].kind = PlatUiArg::Number;
        args[i].number = i + 1;
    }
    CHECK(PlatUiCall("setTime", args).empty());
    CHECK(Run(P, "return lastTime") == "3723");
    CHECK(Starts(PlatUiCall("hide", args), "hide is not a permitted"));
    // away from the station the PLAT UI state makes no widget calls; the feed keeps calling it
    g_clock += 60000;
    args[2].number = 30;
    CHECK(PlatUiCall("setTime", args).empty());
    CHECK(Run(P, "return lastTime") == "3750");
    CHECK(PlatUiTestProbes() - p0 == 2);
    // back at the station after the gap: a new sighting, the state is checked again once
    PlatUiTestRecord(P);
    CHECK(PlatUiStatus() == want);
    CHECK(PlatUiStatus() == want);
    CHECK(PlatUiTestProbes() - p0 == 3);
    PlatUiReset();
    CHECK(PlatUiTestRecorded(0) == nullptr);
    CHECK(Starts(PlatUiStatus(), "PLAT UI state not seen yet (0 dxgui states recorded"));
    lua.close(P);
    lua.close(other);
}

void TestCoroutines() {
    printf("coroutines: folded into the main thread; a suspended one is never called into\n");
    if (!PlatUiTestLayoutOk()) {
        printf("  skipped: lua.dll's layout is not the one plat_ui reads (states kept as seen)\n");
        return;
    }
    PlatUiReset();
    lua_State* Y = NewState(kCounted);
    lua_State* co = lua.newthread(Y);  // stays on Y's stack: referenced
    CHECK(lua.loadstring(co, "local a = ... local b = coroutine.yield(a + 1) return a * 100 + b") == 0);
    lua.pushnumber(co, 5);
    CHECK(lua.resume(co, 1) == 1);  // LUA_YIELD
    CHECK(lua.tonumber(co, -1) == 6);
    const long long p0 = PlatUiTestProbes();
    PlatUiTestRecord(co);  // a widget call from inside the coroutine
    CHECK(PlatUiTestRecorded(0) == static_cast<void*>(Y));
    CHECK(PlatUiTestRecorded(1) == nullptr);
    PlatUiStatus();
    CHECK(PlatUiTestProbes() - p0 == 1);
    CHECK(Run(Y, "return probes") == "1");
    // the coroutine resumes with its stack as it left it
    lua.settop(co, 0);
    lua.pushnumber(co, 7);
    CHECK(lua.resume(co, 1) == 0);
    CHECK(lua.tonumber(co, -1) == 507);
    // the PLAT UI functions called from a coroutine of the PLAT state: the state is found as its main thread
    lua_State* P = NewState(kPlat);
    lua_State* pco = lua.newthread(P);
    PlatUiTestRecord(pco);
    char want[64];
    _snprintf_s(want, _countof(want), _TRUNCATE, "PLAT UI state %p", static_cast<void*>(P));
    CHECK(PlatUiStatus() == want);
    PlatUiReset();
    lua.close(P);
    lua.close(Y);
}

void TestOtherThread() {
    printf("a state used on another thread is not called from this one\n");
    PlatUiReset();
    lua_State* T = NewState(kCounted);
    const long long p0 = PlatUiTestProbes();
    HANDLE h = CreateThread(nullptr, 0, &RecordOnOtherThread, T, 0, nullptr);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    CHECK(Starts(PlatUiStatus(), "PLAT UI state not seen yet (1 dxgui states recorded, 1 on other threads)"));
    CHECK(PlatUiTestProbes() == p0);
    CHECK(Run(T, "return probes") == "0");
    PlatUiReset();
    lua.close(T);
}

}  // namespace

int main(int argc, char** argv) {
    std::wstring root = L"C:\\Program Files\\Eagle Dynamics\\DCS World";
    if (argc > 1) {
        wchar_t w[MAX_PATH] = {};
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, w, MAX_PATH);
        root = w;
    }
    HMODULE m = nullptr;
    for (const wchar_t* sub : {L"\\bin-mt\\lua.dll", L"\\bin\\lua.dll"}) {
        m = LoadLibraryExW((root + sub).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m) break;
    }
    if (!m) {
        printf("lua.dll not found under %ls\n", root.c_str());
        return 2;
    }
    auto get = [m](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
        if (!fn) printf("lua.dll has no %s\n", name);
        return fn != nullptr;
    };
    if (!(get(lua.newstate, "luaL_newstate") && get(lua.openlibs, "luaL_openlibs") && get(lua.loadstring, "luaL_loadstring") &&
          get(lua.pcall, "lua_pcall") && get(lua.close, "lua_close") && get(lua.newthread, "lua_newthread") &&
          get(lua.resume, "lua_resume") && get(lua.settop, "lua_settop") && get(lua.gettop, "lua_gettop") &&
          get(lua.tolstring, "lua_tolstring") && get(lua.tonumber, "lua_tonumber") && get(lua.pushnumber, "lua_pushnumber")))
        return 2;
    PlatUiTestSetClock(&Clock);
    CHECK(PlatUiTestResolve());
    printf("lua.dll layout as plat_ui expects: %s\n", PlatUiTestLayoutOk() ? "yes" : "NO (coroutines kept as seen)");
    TestProbedOnce();
    TestQuietNotProbed();
    TestClosedStateLeftAlone();
    TestFoundAndKept();
    TestCoroutines();
    TestOtherThread();
    printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
