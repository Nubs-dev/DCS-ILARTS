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

bool ResolveApi() {
    if (api.ok) return true;
    HMODULE m = GetModuleHandleW(L"lua.dll");
    if (!m) return false;
    auto get = [m](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
        return fn != nullptr;
    };
    api.ok = get(api.touserdata, "lua_touserdata") && get(api.cpcall, "lua_cpcall") &&
             get(api.getfield, "lua_getfield") && get(api.type, "lua_type") && get(api.gettop, "lua_gettop") &&
             get(api.settop, "lua_settop") && get(api.call, "lua_call") && get(api.pushnumber, "lua_pushnumber") &&
             get(api.pushstring, "lua_pushstring") && get(api.pushboolean, "lua_pushboolean") &&
             get(api.pushnil, "lua_pushnil") && get(api.tolstring, "lua_tolstring") &&
             get(api.loadstring, "luaL_loadstring");
    return api.ok;
}

// ---- recorded states (written by the lua-dxgui import hook) ----
constexpr int kMaxSeen = 32;
struct Seen {
    lua_State* volatile L;
    volatile DWORD tid;
};
Seen g_seen[kMaxSeen];
volatile LONG g_seenCount = 0;
SRWLOCK g_seenLock = SRWLOCK_INIT;

using TouserdataFn = void* (*)(lua_State*, int);
TouserdataFn g_origTouserdata = nullptr;
void** g_iatSlot = nullptr;

void* HookTouserdata(lua_State* L, int idx) {
    LONG n = g_seenCount;
    bool known = false;
    for (LONG i = 0; i < n; ++i)
        if (g_seen[i].L == L) { known = true; break; }
    if (!known) {
        AcquireSRWLockExclusive(&g_seenLock);
        n = g_seenCount;
        bool again = false;
        for (LONG i = 0; i < n; ++i)
            if (g_seen[i].L == L) { again = true; break; }
        if (!again && n < kMaxSeen) {
            g_seen[n].L = L;
            g_seen[n].tid = GetCurrentThreadId();
            InterlockedExchange(&g_seenCount, n + 1);
        }
        ReleaseSRWLockExclusive(&g_seenLock);
    }
    return g_origTouserdata(L, idx);
}


bool LooksLikeThread(lua_State* L) {
    unsigned char tt = 0;
    return L && SafeRead(reinterpret_cast<const char*>(L) + 8, &tt, 1) && tt == kTThread;
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

lua_State* g_platL = nullptr;
lua_State* g_flolsL = nullptr;

// Re-finds a UI state among the recorded ones (only states used on this thread) by two globals.
lua_State* FindState(lua_State*& cache, const char* probeA, const char* probeB, const char* label, std::string* why) {
    if (!ResolveApi()) return *why = "lua.dll API unavailable", nullptr;
    DWORD me = GetCurrentThreadId();
    if (cache) {
        for (LONG i = 0; i < g_seenCount; ++i)
            if (g_seen[i].L == cache && g_seen[i].tid == me && LooksLikeThread(cache)) return cache;
        cache = nullptr;
    }
    LONG n = g_seenCount;
    int other = 0;
    for (LONG i = 0; i < n; ++i) {
        lua_State* L = g_seen[i].L;
        if (g_seen[i].tid != me) { ++other; continue; }
        if (!LooksLikeThread(L)) continue;
        Job job;
        job.kind = Job::Probe;
        job.probeA = probeA;
        job.probeB = probeB;
        if (RunJob(L, job).empty() && job.found) {
            cache = L;
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
    lua_State* L = FindState(g_platL, "getPLATCamera", "setTime", "PLATCameraUI", &w);
    if (!L) *why = w.find("PLATCameraUI state") == 0 ? "PLAT UI" + w.substr(12) : w;
    return L;
}

// FLOLS.lua (2D meatball overlay pilot aid): update(meatball, datum, cutlights) every frame while shown.
lua_State* FindFlolsState(std::string* why) { return FindState(g_flolsL, "unit_test_FLOLS", "update", "FLOLS", why); }

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
    Log("PLATUI lua-dxgui.dll import lua_touserdata %p -> hook (slot %p)", g_origTouserdata, slot);
    return "";
}

void PlatUiReset() {
    if (g_iatSlot) {
        PatchSlot(g_iatSlot, reinterpret_cast<void*>(g_origTouserdata));
        Log("PLATUI lua-dxgui import restored");
        g_iatSlot = nullptr;
    }
    AcquireSRWLockExclusive(&g_seenLock);
    InterlockedExchange(&g_seenCount, 0);
    ReleaseSRWLockExclusive(&g_seenLock);
    g_platL = nullptr;
    g_flolsL = nullptr;
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

}  // namespace lsodiag
