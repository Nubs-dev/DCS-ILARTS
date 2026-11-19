// Benchmark support in the DLL (src/bench_protocol.h): the frame timer on DCS's render thread and the hook's side of
// the shared block. The viewer runs the stages and writes the report (viewer/bench.inc).
#pragma once

#include <windows.h>

#include <string>

namespace lsodiag {

// ILARTS.exe as the DLL starts it (bin\viewer_path.txt, else beside the DLL) and the ILARTS.ini beside it.
std::wstring ViewerExePath();
std::wstring ViewerIniPath();

bool BenchArmed();   // SETTINGS > Benchmark: [benchmark] next_mission=1 in the viewer's ini
void BenchDisarm();  // writes next_mission=0 (no viewer took the run)

// A mission with the benchmark armed: makes the block (once per process; it is never unmapped, so the render thread
// can always write to it), starts a session in state "holding" with the hook's info lines, and starts the frame
// timer (the renderer callback). "" on success, else the reason.
std::string BenchBegin(const char* info);
void BenchSetState(LONG state, const char* reason);  // kBenchReleased / kBenchEnded (the timer stops at Ended)
void BenchTick(bool paused);                          // once a second from the hook: paused flag, hookTick
void BenchWho(const char* info);                      // the player's aircraft or slot changed: new info, whoSeq + 1
// The viewer's command for this session (kBenchCmd*); *viewerAgeMs = ms since the viewer last looked (-1 never);
// *stage = what it measures now.
LONG BenchPoll(long long* viewerAgeMs, std::string* stage);

void BenchFrame();  // render thread, EndFrame (render_callback.cpp): one frame time

}  // namespace lsodiag
