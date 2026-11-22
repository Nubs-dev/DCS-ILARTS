#include "plat_ui.h"

#include "common.h"
#include "compat.h"
#include "engine.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace lsodiag {
namespace {

struct lua_State;
using lua_CFunction = int (*)(lua_State*);
constexpr int kGlobals = -10002;  // LUA_GLOBALSINDEX (Lua 5.1)
constexpr int kTFunction = 6;     // LUA_TFUNCTION
constexpr unsigned char kTThread = 8;

struct LuaApi {
    void* (*touserdata)(lua_State*, int) = nullptr;
    int (*status)(lua_State*) = nullptr;
    int (*cpcall)(lua_State*, lua_CFunction, void*) = nullptr;
    void (*getfield)(lua_State*, int, const char*) = nullptr;
    int (*type)(lua_State*, int) = nullptr;
    int (*gettop)(lua_State*) = nullptr;
    void (*settop)(lua_State*, int) = nullptr;
    void (*call)(lua_State*, int, int) = nullptr;
    void (*pushnumber)(lua_State*, double) = nullptr;
    void (*pushstring)(lua_State*, const char*) = nullptr;
    void (*pushboolean)(lua_State*, int) = nullptr;
    void (*pushnil)(lua_State*) = nullptr;
    const char* (*tolstring)(lua_State*, int, size_t*) = nullptr;
    int (*loadstring)(lua_State*, const char*) = nullptr;
    bool ok = false;
} api;

// lua.dll's lua_State / global_State layout (stock Lua 5.1, x64): tt +0x08, status +0x0A, l_G +0x20;
// global_State::mainthread +0xB0. Used only when lua.dll's own lua_status and lua_pushthread are exactly the
// code that reads those offsets (DCS 2.9.31's lua.dll, 2026-11-19); otherwise each state is kept as seen.
constexpr size_t kOffTt = 0x08, kOffStatus = 0x0A, kOffG = 0x20, kOffMainThread = 0xB0;
const unsigned char kLuaStatusCode[] = {0x0F, 0xB6, 0x41, 0x0A, 0xC3};  // movzx eax,byte [rcx+0Ah] ; ret
const unsigned char kLuaPushThreadCode[] = {
    0x48, 0x8B, 0x41, 0x10,                    // mov rax,[rcx+10h]       (L->top)
    0x48, 0x89, 0x08,                          // mov [rax],rcx
    0xC7, 0x40, 0x08, 0x08, 0x00, 0x00, 0x00,  // mov dword [rax+8],8     (LUA_TTHREAD)
    0x33, 0xC0,                                // xor eax,eax
    0x48, 0x8B, 0x51, 0x20,                    // mov rdx,[rcx+20h]       (L->l_G)
    0x48, 0x83, 0x41, 0x10, 0x10,              // add qword [rcx+10h],10h
    0x48, 0x39, 0x8A, 0xB0, 0x00, 0x00, 0x00,  // cmp [rdx+0B0h],rcx      (G->mainthread == L)
    0x0F, 0x94, 0xC0,                          // sete al
    0xC3};
bool g_layoutOk = false;

bool ResolveApi() {
    if (api.ok) return true;
    HMODULE m = GetModuleHandleW(L"lua.dll");
    if (!m) return false;
    auto get = [m](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
        return fn != nullptr;
    };
    void (*pushthread)(lua_State*) = nullptr;
    api.ok = get(api.touserdata, "lua_touserdata") && get(api.status, "lua_status") && get(api.cpcall, "lua_cpcall") &&
             get(api.getfield, "lua_getfield") && get(api.type, "lua_type") && get(api.gettop, "lua_gettop") &&
             get(api.settop, "lua_settop") && get(api.call, "lua_call") && get(api.pushnumber, "lua_pushnumber") &&
             get(api.pushstring, "lua_pushstring") && get(api.pushboolean, "lua_pushboolean") &&
             get(api.pushnil, "lua_pushnil") && get(api.tolstring, "lua_tolstring") &&
             get(api.loadstring, "luaL_loadstring") && get(pushthread, "lua_pushthread");
    if (api.ok) {
        unsigned char a[sizeof(kLuaStatusCode)], b[sizeof(kLuaPushThreadCode)];
        g_layoutOk = SafeRead(reinterpret_cast<const void*>(api.status), a, sizeof(a)) &&
                     memcmp(a, kLuaStatusCode, sizeof(a)) == 0 &&
                     SafeRead(reinterpret_cast<const void*>(pushthread), b, sizeof(b)) &&
                     memcmp(b, kLuaPushThreadCode, sizeof(b)) == 0;
    }
    return api.ok;
}

#ifdef PLAT_UI_TEST
ULONGLONG (*g_testNow)() = nullptr;
ULONGLONG Now() { return g_testNow ? g_testNow() : GetTickCount64(); }
#else
ULONGLONG Now() { return GetTickCount64(); }
#endif

// ---- recorded states (written by the lua-dxgui import hook) ----
// Each entry is a state's MAIN thread (coroutines are folded into it: they share its globals, and a main
// thread lives until its whole state is closed, while a coroutine can be collected at any time). `last` is
// the latest widget call; a state that comes back after kGapMs without one gets a new `epoch` (it may be a
// new state at the address of a closed one), which makes FindState look at it afresh.
constexpr int kMaxSeen = 32;
constexpr ULONGLONG kGapMs = 5000;    // longer without widget calls: the next call starts a new sighting
constexpr ULONGLONG kFreshMs = 2000;  // a state is probed only within this time of a widget call
struct Seen {
    lua_State* volatile L;
    volatile DWORD tid;
    volatile ULONGLONG last;
    volatile LONG epoch;
};
Seen g_seen[kMaxSeen];
volatile LONG g_seenCount = 0;
volatile LONG g_epochGen = 0;
SRWLOCK g_seenLock = SRWLOCK_INIT;
// FindState's own bookkeeping (the hook's thread only): per entry, the sighting it probed and which probes
// (kProbePlat, kProbeFlols) came back negative in it. A state is probed at most once per sighting.
constexpr unsigned kProbePlat = 1, kProbeFlols = 2;
LONG g_probedEpoch[kMaxSeen];
unsigned g_probedMask[kMaxSeen];
long long g_probes = 0;  // lua_cpcall probes run (test hook)

using TouserdataFn = void* (*)(lua_State*, int);
TouserdataFn g_origTouserdata = nullptr;
void** g_iatSlot = nullptr;

// The main thread of L's state (L is the live, calling state here).
lua_State* MainThreadOf(lua_State* L) {
    if (!g_layoutOk || !L) return L;
    const char* G = *reinterpret_cast<char* const*>(reinterpret_cast<const char*>(L) + kOffG);
    lua_State* M = G ? *reinterpret_cast<lua_State* const*>(G + kOffMainThread) : nullptr;
    return M ? M : L;
}

void Record(lua_State* L) {
    lua_State* M = MainThreadOf(L);
    const ULONGLONG now = Now();
    LONG n = g_seenCount;
    for (LONG i = 0; i < n; ++i)
        if (g_seen[i].L == M) {
            Seen& s = g_seen[i];
            if (now - s.last > kGapMs) s.epoch = InterlockedIncrement(&g_epochGen);
            s.last = now;
            return;
        }
    AcquireSRWLockExclusive(&g_seenLock);
    n = g_seenCount;
    bool again = false;
    for (LONG i = 0; i < n; ++i)
        if (g_seen[i].L == M) {
            again = true;
            break;
        }
    if (!again && n < kMaxSeen) {
        g_seen[n].L = M;
        g_seen[n].tid = GetCurrentThreadId();
        g_seen[n].last = now;
        g_seen[n].epoch = InterlockedIncrement(&g_epochGen);
        InterlockedExchange(&g_seenCount, n + 1);
    }
    ReleaseSRWLockExclusive(&g_seenLock);
}

void* HookTouserdata(lua_State* L, int idx) {
    Record(L);
    return g_origTouserdata(L, idx);
}

bool LooksLikeThread(lua_State* L) {
    unsigned char tt = 0;
    return L && SafeRead(reinterpret_cast<const char*>(L) + kOffTt, &tt, 1) && tt == kTThread;
}

// Whether a recorded state can be called into: still reads as a Lua thread, not suspended (status 0) and,
// with the layout known, still its state's main thread. (A closed state cannot be told apart for certain;
// these catch freed or reused memory in most cases, and FindState only probes states used moments ago.)
bool Callable(lua_State* L) {
    if (!LooksLikeThread(L)) return false;
    if (!g_layoutOk) return api.status(L) == 0;
    unsigned char status = 1;
    const char* G = nullptr;
    lua_State* M = nullptr;
    return SafeRead(reinterpret_cast<const char*>(L) + kOffStatus, &status, 1) && status == 0 &&
           SafeRead(reinterpret_cast<const char*>(L) + kOffG, &G, sizeof(G)) && G &&
           SafeRead(G + kOffMainThread, &M, sizeof(M)) && M == L;
}

// ---- protected work inside a foreign state (lua_cpcall) ----
struct Job {
    enum Kind { Probe, Call, Chunk } kind = Probe;
    const char* name = nullptr;
    const std::vector<PlatUiArg>* args = nullptr;
    const char* chunk = nullptr;
    const char* probeA = "getPLATCamera";   // a state is identified by two global functions
    const char* probeB = "setTime";
    bool found = false;
    std::string result;
};

int JobBody(lua_State* L) {
    Job* job = static_cast<Job*>(api.touserdata(L, 1));
    api.settop(L, 0);
    switch (job->kind) {
    case Job::Probe:
        api.getfield(L, kGlobals, job->probeA);
        job->found = api.type(L, -1) == kTFunction;
        api.getfield(L, kGlobals, job->probeB);
        job->found = job->found && api.type(L, -1) == kTFunction;
        break;
    case Job::Call:
        api.getfield(L, kGlobals, job->name);
        if (api.type(L, -1) != kTFunction) {
            job->result = std::string(job->name) + " is not a function in the PLAT UI state";
            break;
        }
        for (const auto& a : *job->args) {
            switch (a.kind) {
            case PlatUiArg::Number: api.pushnumber(L, a.number); break;
            case PlatUiArg::String: api.pushstring(L, a.string.c_str()); break;
            case PlatUiArg::Boolean: api.pushboolean(L, a.boolean ? 1 : 0); break;
            default: api.pushnil(L); break;
            }
        }
        api.call(L, static_cast<int>(job->args->size()), 0);
        break;
    case Job::Chunk:
        if (api.loadstring(L, job->chunk) != 0) {
            const char* e = api.tolstring(L, -1, nullptr);
            job->result = std::string("chunk load failed: ") + (e ? e : "?");
            break;
        }
        api.call(L, 0, 1);
        if (const char* s = api.tolstring(L, -1, nullptr)) job->result = s;
        break;
    }
    api.settop(L, 0);
    return 0;
}

// Runs job in L under lua_cpcall; Lua errors come back as text, the state's stack is left as found.
std::string RunJob(lua_State* L, Job& job) {
    int top = api.gettop(L);
    int rc = api.cpcall(L, &JobBody, &job);
    std::string err;
    if (rc != 0) {
        const char* e = api.tolstring(L, -1, nullptr);
        err = std::string("lua error: ") + (e ? e : "?");
    }
    api.settop(L, top);
    return err;
}

// A found UI state and the sighting it was found in.
struct Found {
    lua_State* L = nullptr;
    LONG epoch = 0;
};
Found g_plat, g_flols;

void ForgetStates() {
    AcquireSRWLockExclusive(&g_seenLock);
    InterlockedExchange(&g_seenCount, 0);
    for (int i = 0; i < kMaxSeen; ++i) {
        g_seen[i].L = nullptr;
        g_probedEpoch[i] = 0;
        g_probedMask[i] = 0;
    }
    ReleaseSRWLockExclusive(&g_seenLock);
    g_plat = Found{};
    g_flols = Found{};
}

// Re-finds a UI state among the recorded ones (only states used on this thread) by two globals.
// The found state is kept for as long as its recorded sighting lasts (the PLAT UI state is idle while you are
// away from the station, and that is when the feed calls it). An unknown state is probed with lua_cpcall at
// most once per sighting, and only within kFreshMs of one of its widget calls: never one that has gone quiet
// (it may have been closed), never a suspended one.
lua_State* FindState(Found& found, unsigned probe, const char* probeA, const char* probeB, const char* label, std::string* why) {
    if (!ResolveApi()) return *why = "lua.dll API unavailable", nullptr;
    const DWORD me = GetCurrentThreadId();
    const ULONGLONG now = Now();
    const LONG n = g_seenCount;
    if (found.L) {
        for (LONG i = 0; i < n; ++i)
            if (g_seen[i].L == found.L && g_seen[i].tid == me && g_seen[i].epoch == found.epoch && Callable(found.L)) return found.L;
        found = Found{};
    }
    int other = 0;
    for (LONG i = 0; i < n; ++i) {
        const Seen& s = g_seen[i];
        lua_State* L = s.L;
        if (s.tid != me) {
            ++other;
            continue;
        }
        const LONG epoch = s.epoch;
        if (g_probedEpoch[i] != epoch) {  // a new sighting: nothing known about it yet
            g_probedEpoch[i] = epoch;
            g_probedMask[i] = 0;
        }
        if ((g_probedMask[i] & probe) || now - s.last > kFreshMs || !Callable(L)) continue;
        g_probedMask[i] |= probe;
        ++g_probes;
        Job job;
        job.kind = Job::Probe;
        job.probeA = probeA;
        job.probeB = probeB;
        if (RunJob(L, job).empty() && job.found) {
            found.L = L;
            found.epoch = epoch;
            Log("PLATUI found %s state %p (of %ld recorded dxgui states, %d on other threads)", label, L, n, other);
            return L;
        }
    }
    char b[160];
    _snprintf_s(b, _countof(b), _TRUNCATE, "%s state not seen yet (%ld dxgui states recorded, %d on other threads)", label,
                n, other);
    *why = b;
    return nullptr;
}

lua_State* FindPlatState(std::string* why) {
    std::string w;
    lua_State* L = FindState(g_plat, kProbePlat, "getPLATCamera", "setTime", "PLATCameraUI", &w);
    if (!L) *why = w.find("PLATCameraUI state") == 0 ? "PLAT UI" + w.substr(12) : w;
    return L;
}

// FLOLS.lua (2D meatball overlay pilot aid): update(meatball, datum, cutlights) every frame while shown.
lua_State* FindFlolsState(std::string* why) { return FindState(g_flols, kProbeFlols, "unit_test_FLOLS", "update", "FLOLS", why); }

const char kFlolsRecorderChunk[] = R"LUA(
local rec = rawget(_G, '__lsodiag_flols')
if not rec then
  rec = { count = 0, last = {} }
  rawset(_G, '__lsodiag_flols', rec)
  local f = rawget(_G, 'update')
  if type(f) == 'function' then
    rawset(_G, 'update', function(...)
      rec.count = rec.count + 1
      rec.last = { ... }
      return f(...)
    end)
  end
end
local a = rec.last
return string.format('update=%d(%s,%s,%s)', rec.count, tostring(a[1]), tostring(a[2]), tostring(a[3]))
)LUA";

const char* const kWhitelist[] = {"setTime", "setDate", "setAircraftData", "setAircraftReference", "setHook",
                                  "setHookToRampMarker", "setHookTouchdownMarker", "setHookToRampDesired",
                                  "setHookTouchdownDesired", "setDeckWind", "setShipYawPitchRoll", "adjustGate",
                                  "setFoulDeck", "setNightMode", "setDesiredRope", "setCarrierID"};

// Fixed chunk: wraps the setters (call-through) so we learn what DCS feeds them, and reports.
const char kRecorderChunk[] = R"LUA(
local names = {'setTime','setDate','setAircraftData','setAircraftReference','setHook','setHookToRampMarker',
  'setHookTouchdownMarker','setHookToRampDesired','setHookTouchdownDesired','setDeckWind','setShipYawPitchRoll',
  'adjustGate','setFoulDeck','setNightMode','setDesiredRope','setCuePanelInfo','setCaseIIIStack','setCarrierID',
  'show','hide','cleanCuePanel'}
local rec = rawget(_G, '__lsodiag_rec')
if not rec then
  rec = { count = {}, last = {}, foul = {} }
  rawset(_G, '__lsodiag_rec', rec)
  for _, n in ipairs(names) do
    local f = rawget(_G, n)
    if type(f) == 'function' then
      rawset(_G, n, function(...)
        rec.count[n] = (rec.count[n] or 0) + 1
        rec.last[n] = { n = select('#', ...), ... }
        if n == 'setFoulDeck' then
          -- value changes, stamped with Supercarrier's per-frame setTime call count (flash pattern)
          local v = (...)
          if v ~= rec.foulLast then
            rec.foulLast = v
            local h = rec.foul
            h[#h + 1] = tostring(v) .. '@' .. tostring(rec.count.setTime or 0)
            if #h > 40 then table.remove(h, 1) end
          end
        end
        if n == 'setAircraftData' then
          -- PLATCameraUI.lua prints these with %05d (ft, floored to 100), %03d (kt) and %02d (sink, ft/s);
          -- longer numbers run past the field and get cut mid-digit. The real PLAT shows 5 / 3 / 2 digits,
          -- so hold them at the largest value that fits (rec.last above keeps the true values).
          local d, s, vy = ...
          if type(d) == 'number' and d > 30479 then d = 30479 end             -- 99,900 ft
          if type(s) == 'number' and s > 514.2 then s = 514.2 end             -- 999 kt
          if type(vy) == 'number' and vy < -30.479 then vy = -30.479 end      -- 99 ft/s sink
          return f(d, s, vy, select(4, ...))
        end
        return f(...)
      end)
    end
  end
end
local out = {}
for _, n in ipairs(names) do
  local c = rec.count[n]
  if c then
    local a, s = rec.last[n] or {}, {}
    for i = 1, math.min(a.n or #a, 16) do
      local v = a[i]
      s[#s + 1] = type(v) == 'number' and string.format('%.4g', v) or tostring(v)
    end
    out[#out + 1] = n .. '=' .. c .. '(' .. table.concat(s, ',') .. ')'
  end
end
if #rec.foul > 0 then out[#out + 1] = 'foulChanges=' .. table.concat(rec.foul, ' ') end
return #out > 0 and table.concat(out, ' ') or 'no setter calls recorded yet'
)LUA";

}  // namespace

std::string PlatUiInstall() {
    if (EngineDisabled()) return "engine calls disabled";
    // Once per mission: states recorded before now may belong to an earlier mission (the patch stays in place
    // when the hook could not reset it); live ones are recorded again at their next widget call.
    ForgetStates();
    if (g_iatSlot) return "";
    if (!CompatOk()) return CompatWhy();
    if (!ResolveApi()) return "lua.dll API unavailable";
    HMODULE dx = GetModuleHandleW(L"lua-dxgui.dll");
    if (!dx) return "lua-dxgui.dll not loaded";
    void** slot = FindIatSlot(dx, "lua.dll", "lua_touserdata");
    if (!slot) return "lua-dxgui.dll does not import lua_touserdata";
    if (*slot != reinterpret_cast<void*>(api.touserdata)) return "lua-dxgui import of lua_touserdata already redirected";
    g_origTouserdata = reinterpret_cast<TouserdataFn>(*slot);
    if (!PatchSlot(slot, reinterpret_cast<void*>(&HookTouserdata))) return "VirtualProtect failed";
    g_iatSlot = slot;
    Log("PLATUI lua-dxgui.dll import lua_touserdata %p -> hook (slot %p)%s", g_origTouserdata, slot,
        g_layoutOk ? "" : "; lua.dll layout not as expected: states kept as seen");
    return "";
}

void PlatUiReset() {
    if (g_iatSlot) {
        PatchSlot(g_iatSlot, reinterpret_cast<void*>(g_origTouserdata));
        Log("PLATUI lua-dxgui import restored");
        g_iatSlot = nullptr;
    }
    ForgetStates();
}

std::string PlatUiStatus() {
    std::string why;
    lua_State* L = FindPlatState(&why);
    if (!L) return why;
    char b[96];
    _snprintf_s(b, _countof(b), _TRUNCATE, "PLAT UI state %p", L);
    return b;
}

std::string PlatUiCall(const char* name, const std::vector<PlatUiArg>& args) {
    bool allowed = false;
    for (const char* w : kWhitelist)
        if (strcmp(w, name) == 0) allowed = true;
    if (!allowed) return std::string(name) + " is not a permitted PLAT UI function";
    std::string why;
    lua_State* L = FindPlatState(&why);
    if (!L) return why;
    Job job;
    job.kind = Job::Call;
    job.name = name;
    job.args = &args;
    std::string err = RunJob(L, job);
    return err.empty() ? job.result : err;
}

std::string PlatUiCallLog() {
    std::string why;
    lua_State* L = FindPlatState(&why);
    if (!L) return why;
    Job job;
    job.kind = Job::Chunk;
    job.chunk = kRecorderChunk;
    std::string err = RunJob(L, job);
    return err.empty() ? job.result : err;
}

std::string PlatUiFlolsLog() {
    std::string why;
    lua_State* L = FindFlolsState(&why);
    if (!L) return why;
    Job job;
    job.kind = Job::Chunk;
    job.chunk = kFlolsRecorderChunk;
    std::string err = RunJob(L, job);
    return err.empty() ? job.result : err;
}

#ifdef PLAT_UI_TEST
// tools/plat_ui_test.cpp: drives the recording and the search with real Lua states (lua.dll loaded by the test).
bool PlatUiTestResolve() { return ResolveApi(); }
bool PlatUiTestLayoutOk() { return g_layoutOk; }
void PlatUiTestRecord(void* L) { Record(static_cast<lua_State*>(L)); }
void PlatUiTestSetClock(unsigned long long (*now)()) { g_testNow = now; }
long long PlatUiTestProbes() { return g_probes; }
void* PlatUiTestRecorded(int i) { return i >= 0 && i < g_seenCount ? g_seen[i].L : nullptr; }
#endif

}  // namespace lsodiag
