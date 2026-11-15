// Opt-in engine probe + Lua 5.1 module entry point (luaopen_ILARTS).
//
// This is the only file that calls DCS engine code. It calls exactly three DX11Renderer
// methods whose behaviour was verified by disassembly for DCS 2.9.25 and again for 2.9.31 (docs/FINDINGS.md):
//
//   bool        isRenderTargetExists(const char* name)  - name lookup; releases its own reference
//   const char* getResourcesList()                      - text listing of the texture manager;
//                                                         side effect: writes shader_dump.txt
//   void        freeStringMem(const char*)              - frees the getResourcesList string
//
// Guards applied before every call:
//   * RenderAPI::pRenderApi must be non-null and its vptr must equal the vftable exported by
//     dx11backend.dll (so the object really is a DX11Renderer, primary base).
//   * Each method's exported address must occupy a slot in that vtable (so the export is the
//     implementation this object dispatches to).
//   * bin-mt is accepted: in 2.9.31.29434 (as in 2.9.25) its modules are byte-identical to bin.
//   * Calls run inside SEH; any exception disables all further engine calls for the session.
//
// Nothing here runs at load time. The Lua functions run on whatever thread DCS calls the
// hook on (logged), which for the GUI hooks environment is the main simulation thread.

#include "capture.h"
#include "common.h"
#include "compat.h"
#include "engine.h"
#include "lso_camera.h"
#include "plat_ui.h"
#include "clouds_probe.h"
#include "map_objects.h"
#include "ship_trace.h"
#include "plat_origin.h"
#include "memscan.h"
#include "hullscan.h"
#include "hull_refresh.h"
#include "plat_cam.h"
#include "extra_cams.h"
#include "mfd.h"
#include "render_callback.h"
#include "share.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace lsodiag {
namespace {

// ------------------------------------------------------------ minimal Lua 5.1 ABI (DCS lua.dll)

struct lua_State;
using lua_CFunction = int (*)(lua_State*);
using lua_Integer = ptrdiff_t;  // Lua 5.1 default (LUA_INTEGER)

struct LuaApi {
    int (*gettop)(lua_State*);
    int (*toboolean)(lua_State*, int);
    int (*type)(lua_State*, int);
    double (*tonumber)(lua_State*, int);
    const char* (*tolstring)(lua_State*, int, size_t*);
    void (*pushstring)(lua_State*, const char*);
    void (*pushlstring)(lua_State*, const char*, size_t);
    void (*pushboolean)(lua_State*, int);
    void (*pushinteger)(lua_State*, lua_Integer);
    void (*pushnil)(lua_State*);
    void (*createtable)(lua_State*, int, int);
    void (*setfield)(lua_State*, int, const char*);
    void (*pushcclosure)(lua_State*, lua_CFunction, int);
    bool ok = false;
} lua;

bool ResolveLua() {
    if (lua.ok) return true;
    HMODULE m = GetModuleHandleW(L"lua.dll");
    if (!m) return false;
    auto get = [m](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
        return fn != nullptr;
    };
    lua.ok = get(lua.gettop, "lua_gettop") && get(lua.toboolean, "lua_toboolean") && get(lua.type, "lua_type") && get(lua.tonumber, "lua_tonumber") && get(lua.tolstring, "lua_tolstring") &&
             get(lua.pushstring, "lua_pushstring") && get(lua.pushlstring, "lua_pushlstring") &&
             get(lua.pushboolean, "lua_pushboolean") && get(lua.pushinteger, "lua_pushinteger") &&
             get(lua.pushnil, "lua_pushnil") && get(lua.createtable, "lua_createtable") &&
             get(lua.setfield, "lua_setfield") && get(lua.pushcclosure, "lua_pushcclosure");
    return lua.ok;
}

std::wstring LogDir() {
    const std::wstring& p = LogPath();
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

bool WriteWholeFile(const std::wstring& path, const char* data, size_t size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(f);
    return ok && written == size;
}

// Scoped log open (ref-counted in common.cpp).
struct LogScope {
    bool ok;
    LogScope() : ok(OpenLog()) {}
    ~LogScope() {
        if (ok) CloseLog();
    }
};

// ------------------------------------------------------------ Lua functions

// ILARTS.run() -> rc, logpath     : read-only pass (no engine calls)
int L_run(lua_State* L) {
    DWORD rc = RunGuardedPass();
    lua.pushinteger(L, static_cast<lua_Integer>(rc));
    lua.pushstring(L, Utf8(LogPath()).c_str());
    return 2;
}

// ILARTS.probe(name, ...) -> table {name=true|false} | nil, reason
int L_probe(lua_State* L) {
    LogScope log;
    Renderer r;
    std::string why = ResolveRenderer(r);
    if (!why.empty()) {
        Log("PROBE refused: %s", why.c_str());
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    int n = lua.gettop(L);
    lua.createtable(L, 0, n);
    for (int i = 1; i <= n; ++i) {
        const char* name = lua.tolstring(L, i, nullptr);
        if (!name) continue;
        DWORD code = 0;
        int res = SehIsRtExists(r, name, &code);
        if (res < 0) {
            DisableEngine("isRenderTargetExists", code);
            break;
        }
        Log("PROBE isRenderTargetExists('%s') = %s (thread %lu)", name, res ? "true" : "false", GetCurrentThreadId());
        lua.pushboolean(L, res);
        lua.setfield(L, -2, name);
    }
    return 1;
}

// ILARTS.resources([filter, ...]) -> path, matched_lines | nil, reason
// Dumps getResourcesList() to resources-<time>.txt next to the log; logs and returns lines that
// contain any filter substring (default: LSO, mfd, TACTICAL, SCREEN).
int L_resources(lua_State* L) {
    LogScope log;
    Renderer r;
    std::string why = ResolveRenderer(r);
    if (!why.empty()) {
        Log("RESOURCES refused: %s", why.c_str());
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    std::vector<std::string> filters;
    for (int i = 1, n = lua.gettop(L); i <= n; ++i) {
        if (const char* f = lua.tolstring(L, i, nullptr)) filters.emplace_back(f);
    }
    if (filters.empty()) filters = {"LSO", "mfd", "TACTICAL", "SCREEN"};

    DWORD code = 0;
    const char* list = SehGetResList(r, &code);
    if (code) {
        DisableEngine("getResourcesList", code);
        lua.pushnil(L);
        lua.pushstring(L, "exception in getResourcesList");
        return 2;
    }
    if (!list) {
        Log("RESOURCES getResourcesList returned null");
        lua.pushnil(L);
        lua.pushstring(L, "getResourcesList returned null");
        return 2;
    }
    std::string text(list);  // copy before freeing
    if (!SehFreeStr(r, list, &code)) {
        DisableEngine("freeStringMem", code);
    }

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"\\resources-%04u%02u%02u-%02u%02u%02u.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond);
    std::wstring path = LogDir() + name;
    bool saved = WriteWholeFile(path, text.data(), text.size());
    Log("RESOURCES %zu bytes -> %s (%s)", text.size(), Utf8(path).c_str(), saved ? "saved" : "SAVE FAILED");

    std::string matched;
    size_t pos = 0, lines = 0, hits = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        ++lines;
        for (const auto& f : filters) {
            if (line.find(f) != std::string::npos) {
                Log("RESOURCE %s", line.c_str());
                matched += line;
                matched += '\n';
                ++hits;
                break;
            }
        }
        pos = end + 1;
    }
    Log("RESOURCES %zu lines, %zu matched filters", lines, hits);
    lua.pushstring(L, Utf8(path).c_str());
    lua.pushlstring(L, matched.data(), matched.size());
    return 2;
}

// ILARTS.capture(name) -> summary | nil, reason
// Copies one frame of the named render target to captures\<name>-<time>.dds/.bmp.
int L_capture(lua_State* L) {
    LogScope log;
    const char* name = lua.tolstring(L, 1, nullptr);
    if (!name) {
        lua.pushnil(L);
        lua.pushstring(L, "capture(name): name must be a string");
        return 2;
    }
    // The immediate context belongs to the render thread: queue the capture for the next
    // EndFrame callback instead of copying here (the Lua thread is NOT the render thread).
    std::string why = QueueCapture(name);
    if (!why.empty()) {
        Log("CAPTURE %s not queued: %s", name, why.c_str());
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    Log("CAPTURE %s queued for the render thread", name);
    lua.pushstring(L, "queued");
    return 1;
}

// ILARTS.share_start(name, ...) -> true | nil, reason
int L_share_start(lua_State* L) {
    LogScope log;
    std::vector<std::string> names;
    for (int i = 1, n = lua.gettop(L); i <= n; ++i) {
        if (const char* s = lua.tolstring(L, i, nullptr)) names.emplace_back(s);
    }
    std::string why = ShareStart(names);
    if (!why.empty()) {
        Log("SHARE start refused: %s", why.c_str());
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    lua.pushboolean(L, 1);
    return 1;
}

// ILARTS.compat() -> state ('pending' | 'verified' | 'matched' | 'incompatible'), DCS version, detail
// Runs the DCS compatibility check (compat.h) once every module it covers is loaded.
int L_compat(lua_State* L) {
    LogScope log;
    const CompatResult r = CompatCheck();
    lua.pushstring(L, CompatStateName(r.state));
    lua.pushstring(L, r.dcsVersion.c_str());
    lua.pushstring(L, r.detail.c_str());
    return 3;
}

// ILARTS.share_announce() -> true | nil, reason. Only the shared header with the compat block (no
// feeds): lets the viewer say why nothing is shown when DCS is not compatible.
int L_share_announce(lua_State* L) {
    LogScope log;
    std::string why = ShareAnnounce();
    if (!why.empty()) {
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    lua.pushboolean(L, 1);
    return 1;
}

// ILARTS.verbose(on) : development diagnostics in the log (large dumps, statistics)
int L_verbose(lua_State* L) {
    SetVerbose(lua.toboolean(L, 1) != 0);
    return 0;
}

// ILARTS.share_frame() : call every frame (cheap; no-op unless started)
int L_share_frame(lua_State*) {
    ShareFrame();
    return 0;
}

int L_share_status(lua_State* L) {
    lua.pushstring(L, ShareStatus().c_str());
    return 1;
}

int L_share_stop(lua_State*) {
    LogScope log;
    ShareStop();
    return 0;
}

// ILARTS.share_carriers(list, selected)   list = "name|type|x|z" lines, selected = 0-based or -1
int L_share_carriers(lua_State* L) {
    const char* list = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    const int sel = lua.gettop(L) >= 2 ? static_cast<int>(lua.tonumber(L, 2)) : -1;
    ShareSetCarriers(list ? list : "", sel);
    return 0;
}

// ILARTS.share_symbology(text)   "key=value" lines (lsoshare::Header::Symbology)
int L_share_symbology(lua_State* L) {
    const char* t = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    if (t) ShareSetSymbology(t);
    return 0;
}

// ILARTS.share_cue(text)   cue panel rows (lsoshare::Header::Cue)
int L_share_cue(lua_State* L) {
    const char* t = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    if (t) ShareSetCue(t);
    return 0;
}

// ILARTS.share_control() -> "seq", name   (the viewer's carrier request)
int L_share_control(lua_State* L) {
    std::string name;
    const long seq = ShareReadControl(&name);
    char b[24];
    _snprintf_s(b, _countof(b), _TRUNCATE, "%ld", seq);
    lua.pushstring(L, b);   // string: lua_pushnumber is not imported
    lua.pushstring(L, name.c_str());
    return 2;
}

// ILARTS.share_cameras(text)   "name|ldr|hdr|available|enabled|aspect" lines (lsoshare::Header::cameras)
int L_share_cameras(lua_State* L) {
    const char* t = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    if (t) ShareSetCameras(t);
    return 0;
}

// ILARTS.share_camera_control() -> "seq", "mask"   (the viewer's camera request)
int L_share_camera_control(lua_State* L) {
    unsigned mask = 0;
    const long seq = ShareReadCameraControl(&mask);
    char a[24], b[24];
    _snprintf_s(a, _countof(a), _TRUNCATE, "%ld", seq);
    _snprintf_s(b, _countof(b), _TRUNCATE, "%u", mask);
    lua.pushstring(L, a);
    lua.pushstring(L, b);
    return 2;
}

// ILARTS.share_camera_view(i) -> "seq", "index", "trackMode"   (the view the viewer asks camera i to show, and what
// it should follow automatically: share_protocol.h PtzControl::trackMode)
int L_share_camera_view(lua_State* L) {
    int request = -1, mode = 0;
    const long seq = ShareReadCameraView(lua.gettop(L) >= 1 ? static_cast<int>(lua.tonumber(L, 1)) : -1, &request, &mode);
    char a[24], b[24], c[24];
    _snprintf_s(a, _countof(a), _TRUNCATE, "%ld", seq);
    _snprintf_s(b, _countof(b), _TRUNCATE, "%d", request);
    _snprintf_s(c, _countof(c), _TRUNCATE, "%d", mode);
    lua.pushstring(L, a);
    lua.pushstring(L, b);
    lua.pushstring(L, c);
    return 3;
}

// ILARTS.share_camera_views(i, current, "NAME|NAME|...")   camera i's views and the one shown (v6)
int L_share_camera_views(lua_State* L) {
    const char* t = lua.gettop(L) >= 3 ? lua.tolstring(L, 3, nullptr) : nullptr;
    if (t) ShareSetCameraViews(static_cast<int>(lua.tonumber(L, 1)), static_cast<int>(lua.tonumber(L, 2)), t);
    return 0;
}

// ILARTS.share_mishap(text)   a mishap near the carrier (the viewer's black box keeps its footage)
int L_share_mishap(lua_State* L) {
    const char* t = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    if (t) ShareSetMishap(t);
    return 0;
}

// ILARTS.share_retarget(slot, name) -> true | nil, reason   (0-based slot of share_start's list)
int L_share_retarget(lua_State* L) {
    LogScope log;
    const char* name = lua.gettop(L) >= 2 ? lua.tolstring(L, 2, nullptr) : nullptr;
    const std::string why = name ? ShareRetarget(static_cast<int>(lua.tonumber(L, 1)), name) : "share_retarget(slot, name)";
    if (why.empty()) {
        lua.pushboolean(L, 1);
        return 1;
    }
    lua.pushnil(L);
    lua.pushstring(L, why.c_str());
    return 2;
}

// ILARTS.frame_count() -> "n"   DCS frames seen by the render callback
int L_frame_count(lua_State* L) {
    char b[32];
    _snprintf_s(b, _countof(b), _TRUNCATE, "%lld", ShareFrameCount());
    lua.pushstring(L, b);
    return 1;
}

// ILARTS.rt_exists(name) -> true / false / nil (cannot tell)
int L_rt_exists(lua_State* L) {
    const char* name = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    const int r = name ? ShareTargetExists(name) : -1;
    if (r < 0) lua.pushnil(L);
    else lua.pushboolean(L, r);
    return 1;
}

// ILARTS.extra_cam(action, ...)   extra cameras on the carrier (extra_cams.h)
//   'available' -> true | nil, reason
//   'set', index, px, py, pz, lx, ly, lz, fovDeg, aspect, source, type
//   'enable', mask -> true | nil, reason       'mask' -> "mask"      'stats' -> summary      'remove'
//   'track', index, x, y, z, modelTime  (aim at an aircraft, carrier's axes)   'track', index  (stop)
int L_extra_cam(lua_State* L) {
    LogScope log;
    const char* a = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    const std::string act = a ? a : "stats";
    auto result = [&](const std::string& why) {
        if (why.empty()) {
            lua.pushboolean(L, 1);
            return 1;
        }
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    };
    if (act == "available") return result(ExtraCamsAvailable());
    if (act == "set") {
        if (lua.gettop(L) < 12) return result("extra_cam('set', index, px, py, pz, lx, ly, lz, fov, aspect, source, type)");
        const double pos[3] = {lua.tonumber(L, 3), lua.tonumber(L, 4), lua.tonumber(L, 5)};
        const double look[3] = {lua.tonumber(L, 6), lua.tonumber(L, 7), lua.tonumber(L, 8)};
        ExtraCamsConfigure(static_cast<int>(lua.tonumber(L, 2)), pos, look, lua.tonumber(L, 9), lua.tonumber(L, 10),
                           static_cast<int>(lua.tonumber(L, 11)), static_cast<int>(lua.tonumber(L, 12)));
        return 0;
    }
    if (act == "enable") return result(ExtraCamsEnable(lua.gettop(L) >= 2 ? static_cast<unsigned>(lua.tonumber(L, 2)) : 0));
    if (act == "ptz") {  // 'ptz', index, rate deg/s, ramp s, deadband deg
        if (lua.gettop(L) >= 5)
            ExtraCamsPtz(static_cast<int>(lua.tonumber(L, 2)), lua.tonumber(L, 3), lua.tonumber(L, 4), lua.tonumber(L, 5),
                         lua.gettop(L) >= 6 ? static_cast<int>(lua.tonumber(L, 6)) : 0);
        return 0;
    }
    if (act == "track") {  // 'track', index, x, y, z, modelTime   |   'track', index  (stop)
        const int i = lua.gettop(L) >= 2 ? static_cast<int>(lua.tonumber(L, 2)) : -1;
        if (lua.gettop(L) >= 6) {
            const double p[3] = {lua.tonumber(L, 3), lua.tonumber(L, 4), lua.tonumber(L, 5)};
            ExtraCamsTrack(i, p, lua.tonumber(L, 6));
        } else {
            ExtraCamsTrack(i, nullptr, 0);
        }
        return 0;
    }
    if (act == "mask") {
        char b[16];
        _snprintf_s(b, _countof(b), _TRUNCATE, "%u", ExtraCamsMask());
        lua.pushstring(L, b);
        return 1;
    }
    if (act == "remove") {
        ExtraCamsRemove();
        return 0;
    }
    lua.pushstring(L, ExtraCamsStats().c_str());
    return 1;
}

// ILARTS.mark_lsoframe(frames) -> true | nil, reason   (frame-order experiment)
int L_mark_lsoframe(lua_State* L) {
    LogScope log;
    const char* a = lua.tolstring(L, 1, nullptr);
    std::string why = QueueMarker(a ? atoi(a) : 0);
    if (!why.empty()) {
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    lua.pushboolean(L, 1);
    return 1;
}

// ILARTS.renderer_callbacks()   (read-only; writes the callback registry to the log)
int L_renderer_callbacks(lua_State*) {
    LogScope log;
    LogRendererCallbackRegistry();
    return 0;
}

// ILARTS.lso_camera() -> summary string   (Q4 observation; details go to the log on change)
int L_lso_camera(lua_State* L) {
    LogScope log;
    const char* a = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    if (a && std::string(a) == "trace") {
        lua.pushstring(L, LsoPoseTraceStats().c_str());
        return 1;
    }
    if (a && std::string(a) == "ship") {  // ship axes for this frame: 9 numbers in one string
        const char* axes = lua.gettop(L) >= 2 ? lua.tolstring(L, 2, nullptr) : nullptr;
        HullScanShip(axes);
        lua.pushboolean(L, LsoSmoothShip(axes) ? 1 : 0);
        return 1;
    }
    if (a && (std::string(a) == "smooth_on" || std::string(a) == "smooth_off")) {
        lua.pushstring(L, LsoSmoothEnable(std::string(a) == "smooth_on").c_str());
        return 1;
    }
    if (a && std::string(a) == "smooth_stats") {
        lua.pushstring(L, LsoSmoothStats().c_str());
        return 1;
    }
    if (a && std::string(a) == "sample") {  // per-frame pose sample for the trace -> "qpc yaw x y z"
        LsoCameraProbe();
        const std::string pose = LsoPoseLast();
        double x = 0, z = 0;
        if (sscanf_s(pose.c_str(), "%*lf %*lf %lf %*lf %lf", &x, &z) == 2) ShipTraceSetRef(x, z);
        lua.pushstring(L, pose.c_str());
        return 1;
    }
    if (a && std::string(a) == "ship_draw_on") {  // trace: the pose ships are drawn with (ship_trace.h)
        std::string why = ShipTraceInstall();
        if (why.empty()) why = ShipDrawInstall();
        if (why.empty()) why = ShipMatInstall();
        lua.pushstring(L, why.c_str());
        return 1;
    }
    if (a && std::string(a) == "ship_draw_off") {
        ShipTraceRemove();
        return 0;
    }
    if (a && std::string(a) == "ship_draw") {
        lua.pushstring(L, (ShipTraceLast() + " | " + ShipDrawLast()).c_str());
        return 1;
    }
    lua.pushstring(L, LsoCameraProbe().c_str());
    return 1;
}

// ILARTS.keep_plat(true|false) -> true | nil, reason   (Q4 override; see lso_camera.h)
int L_keep_plat(lua_State* L) {
    LogScope log;
    bool on = lua.gettop(L) < 1 || lua.toboolean(L, 1) != 0;
    std::string r = on ? LsoKeepPlatStart() : LsoKeepPlatStop();
    if (on && !r.empty() && r != "already installed") {
        lua.pushnil(L);
        lua.pushstring(L, r.c_str());
        return 2;
    }
    lua.pushboolean(L, 1);
    lua.pushstring(L, r.empty() ? "installed" : r.c_str());
    return 2;
}

// ILARTS.sc_override(true|false)
int L_sc_override(lua_State* L) {
    LogScope log;
    LsoSupercarrierOverride(lua.gettop(L) < 1 || lua.toboolean(L, 1) != 0);
    return 0;
}

// ILARTS.view_calls() -> string   (globalView slot call counts since last call; observation)
// ILARTS.clouds(action)   (cloud jitter investigation; see clouds_probe.h)
//   clouds('install') -> true | nil, reason      clouds('remove')
//   clouds('stats')   -> per-renderer summary    clouds('dump') -> same, and dump each renderer's context
int L_clouds(lua_State* L) {
    LogScope log;
    const char* a = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    std::string act = a ? a : "stats";
    if (act == "install") {
        std::string why = CloudsProbeInstall();
        if (why.empty()) {
            lua.pushboolean(L, 1);
            return 1;
        }
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    if (act == "remove") {
        CloudsProbeRemove();
        return 0;
    }
    if (act == "rebase_on" || act == "rebase_off" || act == "rebase_full") {
        CloudsRebase(act == "rebase_on" ? 2 : act == "rebase_full" ? 1 : 0);   // on = ray-direction fix
        return 0;
    }
    if (act == "trace_on" || act == "trace_off") {
        CloudsTrace(act == "trace_on");
        return 0;
    }
    if (act == "trace_dump") {
        const char* lbl = lua.gettop(L) >= 2 ? lua.tolstring(L, 2, nullptr) : nullptr;
        lua.pushinteger(L, CloudsTraceDump(lbl));
        return 1;
    }
    if (act == "qpc") {
        char q[32];
        _snprintf_s(q, _countof(q), _TRUNCATE, "%.6f", CloudsQpc());
        lua.pushstring(L, q);   // string: lua_pushnumber is not imported
        return 1;
    }
    if (act == "rebase_stats") {
        lua.pushstring(L, CloudsRebaseStats().c_str());
        return 1;
    }
    lua.pushstring(L, CloudsProbeStats(act == "dump").c_str());
    return 1;
}
// ILARTS.memscan(degrees [, label]) -> true | nil, reason   (background search; MEMSCAN log lines)
int L_memscan(lua_State* L) {
    LogScope log;
    double deg = lua.gettop(L) >= 1 ? lua.tonumber(L, 1) : 0;
    const char* label = lua.gettop(L) >= 2 ? lua.tolstring(L, 2, nullptr) : nullptr;
    std::string why = MemScanStart(deg, label ? label : "");
    if (why.empty()) {
        lua.pushboolean(L, 1);
        return 1;
    }
    lua.pushnil(L);
    lua.pushstring(L, why.c_str());
    return 2;
}
// ILARTS.hull_scan(action [, label])   (deck jitter: find the pose the carrier is drawn with)
//   'start' -> true | nil, reason   'stats' -> summary (details to hullscan.log)
int L_hull_scan(lua_State* L) {
    LogScope log;
    const char* a = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    const std::string act = a ? a : "";
    if (act == "start") {
        const char* label = lua.gettop(L) >= 2 ? lua.tolstring(L, 2, nullptr) : nullptr;
        std::string why = HullScanStart(label);
        if (why.empty()) {
            lua.pushboolean(L, 1);
            return 1;
        }
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    lua.pushstring(L, HullScanStats().c_str());
    return 1;
}
// ILARTS.hull_refresh(action)   (deck jitter fix; see hull_refresh.h)
//   'install' -> true | nil, reason    'on' / 'off'    'stats' -> summary    'remove'
int L_hull_refresh(lua_State* L) {
    LogScope log;
    const char* a = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    const std::string act = a ? a : "";
    if (act == "install") {
        std::string why = HullRefreshInstall();
        if (why.empty()) {
            lua.pushboolean(L, 1);
            return 1;
        }
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    if (act == "on" || act == "off") {
        HullRefreshEnable(act == "on");
        return 0;
    }
    if (act == "remove") {
        HullRefreshRemove();
        return 0;
    }
    lua.pushstring(L, HullRefreshStats().c_str());
    return 1;
}
// ILARTS.plat_cam(action, ...)   (synthetic PLAT camera; see plat_cam.h)
//   'enable' -> true | nil, reason    'disable'    'select', x, y, z, type    'ship', "12 numbers"
//   'stats' -> summary
int L_plat_cam(lua_State* L) {
    LogScope log;
    const char* a = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    const std::string act = a ? a : "stats";
    if (act == "enable") {
        const std::string why = PlatCamEnable(true);
        if (why.empty()) {
            lua.pushboolean(L, 1);
            return 1;
        }
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    if (act == "disable") {
        PlatCamEnable(false);
        return 0;
    }
    if (act == "select") {
        if (lua.gettop(L) < 4) return 0;
        const char* type = lua.gettop(L) >= 5 ? lua.tolstring(L, 5, nullptr) : "";
        PlatCamSelect(lua.tonumber(L, 2), lua.tonumber(L, 3), lua.tonumber(L, 4), type ? type : "");
        return 0;
    }
    if (act == "ship") {
        const char* s = lua.gettop(L) >= 2 ? lua.tolstring(L, 2, nullptr) : nullptr;
        double v[12];
        const bool ok = s && sscanf_s(s, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3], &v[4],
                                      &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]) == 12;
        if (ok) PlatCamShip(v);
        lua.pushboolean(L, ok ? 1 : 0);
        return 1;
    }
    if (act == "viewport") {  // size of the PLAT's render-to-texture viewport (32): "w h" | nil, reason
        int w = 0, h = 0;
        const std::string why = MfdViewportSize(32, &w, &h);
        if (!why.empty()) {
            lua.pushnil(L);
            lua.pushstring(L, why.c_str());
            return 2;
        }
        char b[32];
        _snprintf_s(b, _countof(b), _TRUNCATE, "%d %d", w, h);
        lua.pushstring(L, b);
        return 1;
    }
    lua.pushstring(L, PlatCamStats().c_str());
    return 1;
}

// ILARTS.plat_origin(action)   (PLAT render origin fix; see plat_origin.h)
//   'install' -> true | nil, reason    'on' (= 'view') / 'frame' / 'off'    'stats' -> summary    'remove'
int L_plat_origin(lua_State* L) {
    LogScope log;
    const char* a = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    std::string act = a ? a : "stats";
    if (act == "install") {
        std::string why = PlatOriginInstall();
        if (why.empty()) {
            lua.pushboolean(L, 1);
            return 1;
        }
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    if (act == "on" || act == "view" || act == "frame" || act == "off") {
        PlatOriginEnable(act == "frame" ? 1 : act == "off" ? 0 : 2);
        return 0;
    }
    if (act == "remove") {
        PlatOriginRemove();
        return 0;
    }
    lua.pushstring(L, PlatOriginStats().c_str());
    return 1;
}
// ILARTS.map_objects(action)   (world object search on the F10 map; see map_objects.h)
//   'install' -> true | nil, reason    'on' / 'off'    'stats' -> summary    'remove'
int L_map_objects(lua_State* L) {
    LogScope log;
    const char* a = lua.gettop(L) >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    std::string act = a ? a : "stats";
    if (act == "install") {
        std::string why = MapObjectsInstall();
        if (why.empty()) {
            lua.pushboolean(L, 1);
            return 1;
        }
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    if (act == "on" || act == "off") {
        MapObjectsEnable(act == "on");
        return 0;
    }
    if (act == "remove") {
        MapObjectsRemove();
        return 0;
    }
    lua.pushstring(L, MapObjectsStats().c_str());
    return 1;
}
int L_view_calls(lua_State* L) {
    LogScope log;
    lua.pushstring(L, LsoViewCallStats().c_str());
    return 1;
}

// ILARTS.plat_ui(action, ...)   (Q4 overlay bridge; see plat_ui.h)
//   plat_ui('install')            -> true | nil, reason
//   plat_ui('status')             -> status string
//   plat_ui('log')                -> what DCS has fed the PLAT UI setters (installs recorders once)
//   plat_ui('call', name, ...)    -> true | nil, error   (name from a fixed whitelist)
//   plat_ui('flols')              -> FLOLS overlay update() recorder summary
//   plat_ui('reset')
int L_plat_ui(lua_State* L) {
    LogScope log;
    int n = lua.gettop(L);
    const char* action = n >= 1 ? lua.tolstring(L, 1, nullptr) : nullptr;
    std::string a = action ? action : "";
    std::string r;
    if (a == "install") {
        r = PlatUiInstall();
    } else if (a == "status") {
        lua.pushstring(L, PlatUiStatus().c_str());
        return 1;
    } else if (a == "log") {
        lua.pushstring(L, PlatUiCallLog().c_str());
        return 1;
    } else if (a == "flols") {
        lua.pushstring(L, PlatUiFlolsLog().c_str());
        return 1;
    } else if (a == "reset") {
        PlatUiReset();
    } else if (a == "call" && n >= 2 && lua.tolstring(L, 2, nullptr)) {
        std::string name = lua.tolstring(L, 2, nullptr);
        std::vector<PlatUiArg> args;
        for (int i = 3; i <= n; ++i) {
            PlatUiArg v;
            switch (lua.type(L, i)) {
            case 3: v.kind = PlatUiArg::Number; v.number = lua.tonumber(L, i); break;   // LUA_TNUMBER
            case 4: v.kind = PlatUiArg::String; v.string = lua.tolstring(L, i, nullptr); break;  // LUA_TSTRING
            case 1: v.kind = PlatUiArg::Boolean; v.boolean = lua.toboolean(L, i) != 0; break;  // LUA_TBOOLEAN
            default: break;
            }
            args.push_back(v);
        }
        r = PlatUiCall(name.c_str(), args);
    } else {
        r = "usage: plat_ui('install'|'status'|'log'|'flols'|'reset'|'call', ...)";
    }
    if (!r.empty()) {
        lua.pushnil(L);
        lua.pushstring(L, r.c_str());
        return 2;
    }
    lua.pushboolean(L, 1);
    return 1;
}

// ILARTS.mfd_viewports() -> true | nil, reason   (read-only; writes the table to the log)
int L_mfd_viewports(lua_State* L) {
    LogScope log;
    std::string why = MfdLogViewports();
    if (!why.empty()) {
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    lua.pushboolean(L, 1);
    return 1;
}

// ILARTS.set_mfd_resolution(index, w, h) -> true | nil, reason
// Applied on the render thread (requires share_start, which installs the render callback).
int L_set_mfd_resolution(lua_State* L) {
    LogScope log;
    const char* a = lua.tolstring(L, 1, nullptr);
    const char* b = lua.tolstring(L, 2, nullptr);
    const char* c = lua.tolstring(L, 3, nullptr);
    if (!a || !b || !c) {
        lua.pushnil(L);
        lua.pushstring(L, "set_mfd_resolution(index, w, h)");
        return 2;
    }
    // DCS's own enforceResolutionOfMFD first (size map + queued resize), then keep it applied from
    // the render thread in case DCS resets the viewport.
    std::string why = MfdEnforceResolution(atoi(a), atoi(b), atoi(c));
    if (!why.empty()) Log("MFD enforceResolutionOfMFD not called: %s", why.c_str());
    why = MfdRequestResolution(atoi(a), atoi(b), atoi(c));
    if (!why.empty()) {
        lua.pushnil(L);
        lua.pushstring(L, why.c_str());
        return 2;
    }
    lua.pushboolean(L, 1);
    return 1;
}

// ILARTS.log(text) : writes a line to the diagnostic log (for hook script breadcrumbs)
int L_log(lua_State* L) {
    LogScope log;
    const char* s = lua.tolstring(L, 1, nullptr);
    Log("LUA %s", s ? s : "<non-string>");
    return 0;
}

// ILARTS.launch_viewer() -> text. Starts ILARTS.exe unless one is already running (its main
// window class is "ILARTSViewer"). The path comes from viewer_path.txt beside this DLL (written by
// deploy\install.ps1; UTF-8, one line), else ILARTS.exe beside this DLL. The window is shown without
// being activated so DCS keeps the keyboard focus. Not when the user switched it off ([viewer] auto_start=0).
int L_launch_viewer(lua_State* L) {
    LogScope log;
    std::string out;
    if (FindWindowW(L"ILARTSViewer", nullptr)) {
        out = "already running";
    } else {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&L_launch_viewer), &self);
        std::wstring dir = ModulePath(self);
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        std::wstring exe = dir + L"ILARTS.exe";
        FILE* f = nullptr;
        if (_wfopen_s(&f, (dir + L"viewer_path.txt").c_str(), L"rb") == 0 && f) {
            char buf[1024] = {};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            std::string s(buf, n);
            if (s.rfind("\xEF\xBB\xBF", 0) == 0) s.erase(0, 3);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
            wchar_t w[1024] = {};
            if (!s.empty() && MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w, 1024) > 0) exe = w;
        }
        // SETTINGS > Window > Start with DCS: [viewer] auto_start in the ILARTS.ini beside ILARTS.exe (default on)
        const std::wstring ini = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"ILARTS.ini";
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            out = "not found: " + Utf8(exe);
        } else if (GetPrivateProfileIntW(L"viewer", L"auto_start", 1, ini.c_str()) == 0) {
            out = "not started: auto start is off ([viewer] auto_start=0)";
        } else {
            STARTUPINFOW si{sizeof(si)};
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWNOACTIVATE;
            PROCESS_INFORMATION pi{};
            std::wstring cmd = L"\"" + exe + L"\"";
            std::wstring cwd = exe.substr(0, exe.find_last_of(L"\\/"));
            if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                out = "started " + Utf8(exe);
            } else {
                out = "CreateProcess failed (" + std::to_string(GetLastError()) + "): " + Utf8(exe);
            }
        }
    }
    Log("VIEWER %s", out.c_str());
    lua.pushstring(L, out.c_str());
    return 1;
}

int L_logpath(lua_State* L) {
    LogScope log;  // resolves the path
    lua.pushstring(L, Utf8(LogPath()).c_str());
    return 1;
}

}  // namespace
}  // namespace lsodiag

// require('ILARTS') entry point. Builds and returns a module table; sets no globals.
extern "C" __declspec(dllexport) int luaopen_ILARTS(lsodiag::lua_State* L) {
    using namespace lsodiag;
    if (!ResolveLua()) return 0;  // lua.dll not present: nothing we can safely do
    {
        LogScope log;
        Log("LUA module loaded into PID=%lu thread=%lu exe=%s", GetCurrentProcessId(), GetCurrentThreadId(),
            Utf8(ModulePath(GetModuleHandleW(nullptr))).c_str());
    }
    lua.createtable(L, 0, 24);
    struct {
        const char* name;
        lua_CFunction fn;
    } fns[] = {{"run", L_run}, {"probe", L_probe}, {"resources", L_resources}, {"capture", L_capture},
               {"share_start", L_share_start}, {"share_frame", L_share_frame}, {"compat", L_compat}, {"share_announce", L_share_announce}, {"verbose", L_verbose},
               {"share_status", L_share_status}, {"share_stop", L_share_stop}, {"share_carriers", L_share_carriers}, {"share_symbology", L_share_symbology}, {"share_control", L_share_control}, {"share_cue", L_share_cue},
               {"share_cameras", L_share_cameras}, {"share_camera_control", L_share_camera_control},
               {"share_camera_view", L_share_camera_view}, {"share_camera_views", L_share_camera_views}, {"share_mishap", L_share_mishap},
               {"share_retarget", L_share_retarget}, {"frame_count", L_frame_count}, {"rt_exists", L_rt_exists}, {"extra_cam", L_extra_cam},
               {"mfd_viewports", L_mfd_viewports}, {"mark_lsoframe", L_mark_lsoframe},
               {"renderer_callbacks", L_renderer_callbacks}, {"lso_camera", L_lso_camera}, {"keep_plat", L_keep_plat}, {"plat_ui", L_plat_ui}, {"view_calls", L_view_calls}, {"clouds", L_clouds}, {"plat_origin", L_plat_origin}, {"map_objects", L_map_objects},{"memscan", L_memscan}, {"hull_scan", L_hull_scan}, {"hull_refresh", L_hull_refresh}, {"plat_cam", L_plat_cam}, {"sc_override", L_sc_override}, {"set_mfd_resolution", L_set_mfd_resolution}, {"log", L_log}, {"logpath", L_logpath}, {"launch_viewer", L_launch_viewer}};
    for (const auto& f : fns) {
        lua.pushcclosure(L, f.fn, 0);
        lua.setfield(L, -2, f.name);
    }
    lua.pushstring(L, "ILARTS " __DATE__ " " __TIME__ " (verified against DCS 2.9.31.29434)");
    lua.setfield(L, -2, "version");
    return 1;
}
