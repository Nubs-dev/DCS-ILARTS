// Q4 overlay: reach the Lua state that runs Supercarrier's PLATCameraUI.lua (PLAT symbology).
//
// That script runs in a private Lua state of the (protected) Supercarrier module, not in any of
// DCS's named states. Its widgets are dxgui widgets, whose Lua bindings live in the unprotected
// lua-dxgui.dll; every widget method calls lua_touserdata(L, 1) with the caller's state. We patch
// lua-dxgui.dll's import of lua_touserdata (IAT entry only) to record the states that make widget
// calls, then pick the one whose globals contain getPLATCamera.
//
// All calls into that state are made on the thread that was seen using it (DCS's main thread), from
// the GUI hook, i.e. never while that state is executing.
//
// Safety of the search (2026-11-19 audit): the recorded states are main threads (a coroutine is folded
// into its state's main thread), each with the time of its latest widget call. An unknown state is
// probed with lua_cpcall at most once per sighting and only within 2 s of a widget call, and never when
// suspended. Before, every recorded state was probed once a second for as long as the PLAT UI state had
// not been found, i.e. for the whole mission when you never opened the LSO view - including states closed
// since and coroutines collected since, whose memory had been freed.
#pragma once

#include <string>
#include <vector>

namespace lsodiag {

struct PlatUiArg {
    enum Kind { Nil, Number, String, Boolean } kind = Nil;
    double number = 0;
    std::string string;
    bool boolean = false;
};

// Installs the lua-dxgui import patch (idempotent). Returns "" on success or why not.
std::string PlatUiInstall();
// Removes the import patch and forgets recorded states (call at mission stop).
void PlatUiReset();
// Finds the PLAT UI state among recorded ones; one-line status.
std::string PlatUiStatus();
// Calls a whitelisted PLATCameraUI.lua global function in that state. Returns "" or the error.
std::string PlatUiCall(const char* name, const std::vector<PlatUiArg>& args);
// Installs (once) call-through recorders on the PLATCameraUI setters inside that state and returns
// a summary of how often each was called by DCS and with which arguments.
std::string PlatUiCallLog();
// FLOLS.lua (2D meatball pilot aid, its own private state): installs (once) a call-through recorder on
// update(meatball, datum, cutlights) and returns "update=<calls>(meatball,datum,cutlights)" (last call).
std::string PlatUiFlolsLog();

#ifdef PLAT_UI_TEST
bool PlatUiTestResolve();                              // lua.dll's API (lua.dll must be loaded)
bool PlatUiTestLayoutOk();                             // lua.dll's code matches the layout plat_ui reads
void PlatUiTestRecord(void* L);                        // what the import hook records for a widget call from L
void PlatUiTestSetClock(unsigned long long (*now)());  // replaces GetTickCount64 (null: back to it)
long long PlatUiTestProbes();                          // lua_cpcall probes run so far
void* PlatUiTestRecorded(int i);                       // the i-th recorded state, or null
#endif

}  // namespace lsodiag
