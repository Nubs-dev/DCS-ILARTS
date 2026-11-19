// Benchmark (SETTINGS > Benchmark in the viewer): the small shared block through which the DLL, the hook and the viewer
// run one benchmark at a mission start. Separate from the share (Header) because the first stage measures DCS with the
// hook alone, before anything is shared. Plain C layout, no pointers. Shared by src/bench.cpp, viewer/bench.inc and
// tools/share_selftest.cpp.
//
//   DLL, render thread: frameMs[] gets the time of every DCS frame (EndFrame to EndFrame), frames counts them.
//   Hook (through the DLL): session, hookState, paused, info, endReason.
//   Viewer: command, stage, viewerTick; it reads the frame times and writes the report.
#pragma once

#include "share_protocol.h"

#include <cwchar>

namespace lsoshare {

constexpr uint32_t kBenchMagic = 0x434E4542;  // "BENC"
constexpr uint32_t kBenchVersion = 1;
constexpr int kBenchRing = 16384;  // frame times kept (55 s at 300 fps); the viewer reads them several times a second

// Bench::hookState
constexpr LONG kBenchIdle = 0;      // no benchmark in this mission
constexpr LONG kBenchHolding = 1;   // armed: the hook holds every ILARTS feature back (stage 1, the hook alone)
constexpr LONG kBenchReleased = 2;  // the viewer said go: the hook started ILARTS as usual
constexpr LONG kBenchEnded = 3;     // the mission ended, the player changed aircraft or slot, or no viewer came (endReason)
// Bench::command (for commandSession)
constexpr LONG kBenchCmdNone = 0;
constexpr LONG kBenchCmdRelease = 1;  // start ILARTS's features (stage 2 on)
constexpr LONG kBenchCmdDone = 2;     // the run is over (finished or aborted): the hook stops watching it

struct Bench {
    uint32_t magic, version;
    uint32_t producerPid, reserved0;
    LONG64 qpcFrequency;
    // DLL, render thread: frame i's time (ms) at frameMs[i % kBenchRing]; frames is written after the entry
    volatile LONG64 frames;
    float frameMs[kBenchRing];
    // hook
    volatile LONG session;    // + 1 at each armed mission start (a run belongs to one session)
    volatile LONG hookState;  // kBench*
    volatile LONG paused;     // 1 while the simulation is paused (the hook's last step)
    volatile LONG whoSeq;     // + 1 when the player's aircraft or slot changed (info has the new one)
    volatile LONG64 hookTick;  // GetTickCount64 at the hook's last step
    char endReason[64];        // with kBenchEnded
    char info[512];            // "key=value" lines: mission, aircraft, role, dcs (UTF-8)
    // viewer
    volatile LONG command;         // kBenchCmd*
    volatile LONG commandSession;  // the session the command is for
    volatile LONG64 viewerTick;    // GetTickCount64 at the viewer's last look
    char stage[48];                // what the viewer measures now (the hook logs changes)
};

// The block's name: the share's with "_Bench" (a test copy with its own kMappingName gets its own block too).
inline void BenchMappingName(wchar_t* out, size_t n) {
    wcscpy_s(out, n, kMappingName);
    wcscat_s(out, n, L"_Bench");
}

}  // namespace lsoshare
