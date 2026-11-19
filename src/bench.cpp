#include "bench.h"

#include "bench_protocol.h"
#include "common.h"
#include "compat.h"
#include "render_callback.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace lsodiag {
namespace {

HANDLE g_map = nullptr;
std::atomic<lsoshare::Bench*> g_block{nullptr};
std::atomic<bool> g_timing{false};  // frame times are written (holding / released)
LONG64 g_lastQpc = 0;                // render thread only
double g_msPerTick = 0;
std::string g_dcs;  // DCS.exe's version (info's dcs= line)

void Copy(char* dst, size_t n, const char* src) { strncpy_s(dst, n, src ? src : "", _TRUNCATE); }

void SetInfo(lsoshare::Bench* b, const char* info) {
    std::string lines = info ? info : "";
    if (!g_dcs.empty()) lines += (lines.empty() || lines.back() == '\n' ? "" : "\n") + std::string("dcs=") + g_dcs + "\n";
    Copy(b->info, sizeof(b->info), lines.c_str());
}

}  // namespace

std::wstring ViewerExePath() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ViewerExePath), &self);
    std::wstring dir = ModulePath(self);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    std::wstring exe = dir + L"ILARTS.exe";
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"viewer_path.txt").c_str(), L"rb") == 0 && f) {  // development: install.ps1 writes it
        char buf[1024] = {};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        std::string s(buf, n);
        if (s.rfind("\xEF\xBB\xBF", 0) == 0) s.erase(0, 3);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        wchar_t w[1024] = {};
        if (!s.empty() && MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w, 1024) > 0) exe = w;
    }
    return exe;
}

std::wstring ViewerIniPath() {
    const std::wstring exe = ViewerExePath();
    return exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"ILARTS.ini";
}

bool BenchArmed() { return GetPrivateProfileIntW(L"benchmark", L"next_mission", 0, ViewerIniPath().c_str()) != 0; }

void BenchDisarm() { WritePrivateProfileStringW(L"benchmark", L"next_mission", L"0", ViewerIniPath().c_str()); }

std::string BenchBegin(const char* info) {
    lsoshare::Bench* b = g_block.load();
    if (!b) {
        wchar_t name[96];
        lsoshare::BenchMappingName(name, _countof(name));
        g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(lsoshare::Bench), name);
        if (!g_map) return "CreateFileMapping failed (" + std::to_string(GetLastError()) + ")";
        b = static_cast<lsoshare::Bench*>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(lsoshare::Bench)));
        if (!b) return "MapViewOfFile failed (" + std::to_string(GetLastError()) + ")";
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_msPerTick = 1000.0 / static_cast<double>(f.QuadPart);
        b->qpcFrequency = f.QuadPart;
        b->producerPid = GetCurrentProcessId();
        b->version = lsoshare::kBenchVersion;
        MemoryBarrier();
        b->magic = lsoshare::kBenchMagic;
        g_block.store(b, std::memory_order_release);  // never unmapped: the render thread may hold it
    }
    g_dcs = CompatCheck().dcsVersion;
    SetInfo(b, info);
    Copy(b->endReason, sizeof(b->endReason), "");
    b->paused = 0;
    b->hookTick = static_cast<LONG64>(GetTickCount64());
    InterlockedExchange(&b->hookState, lsoshare::kBenchHolding);
    InterlockedIncrement(&b->session);
    // The timer: our EndFrame callback (registering it changes nothing in DCS's rendering; fn stays whatever it is).
    const std::string why = EnsureRenderCallback(nullptr);
    if (!why.empty()) {
        Copy(b->endReason, sizeof(b->endReason), "no frame timer");
        InterlockedExchange(&b->hookState, lsoshare::kBenchEnded);
        return "render-thread callback unavailable: " + why;
    }
    g_timing.store(true, std::memory_order_release);
    Log("BENCH session %ld: holding ILARTS back (stage 1: the hook alone); block %p", b->session, static_cast<void*>(b));
    return {};
}

void BenchSetState(LONG state, const char* reason) {
    lsoshare::Bench* b = g_block.load();
    if (!b) return;
    if (reason) Copy(b->endReason, sizeof(b->endReason), reason);
    MemoryBarrier();
    InterlockedExchange(&b->hookState, state);
    if (state == lsoshare::kBenchEnded || state == lsoshare::kBenchIdle) g_timing.store(false, std::memory_order_release);
    Log("BENCH session %ld: %s%s%s", b->session, state == lsoshare::kBenchReleased ? "released (ILARTS starts)" : "ended",
        reason && *reason ? ": " : "", reason ? reason : "");
}

void BenchTick(bool paused) {
    lsoshare::Bench* b = g_block.load();
    if (!b) return;
    b->paused = paused ? 1 : 0;
    InterlockedExchange64(&b->hookTick, static_cast<LONG64>(GetTickCount64()));
}

void BenchWho(const char* info) {
    lsoshare::Bench* b = g_block.load();
    if (!b) return;
    SetInfo(b, info);
    MemoryBarrier();
    InterlockedIncrement(&b->whoSeq);
}

LONG BenchPoll(long long* viewerAgeMs, std::string* stage) {
    lsoshare::Bench* b = g_block.load();
    if (viewerAgeMs) *viewerAgeMs = -1;
    if (!b) return lsoshare::kBenchCmdNone;
    const LONG64 vt = b->viewerTick;
    if (viewerAgeMs && vt) *viewerAgeMs = static_cast<long long>(GetTickCount64()) - vt;
    if (stage) stage->assign(b->stage, strnlen(b->stage, sizeof(b->stage)));
    return b->commandSession == b->session ? b->command : lsoshare::kBenchCmdNone;
}

void BenchFrame() {
    if (!g_timing.load(std::memory_order_acquire)) {
        g_lastQpc = 0;
        return;
    }
    lsoshare::Bench* b = g_block.load(std::memory_order_acquire);
    if (!b) return;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (g_lastQpc) {
        const LONG64 n = b->frames;
        b->frameMs[n % lsoshare::kBenchRing] = static_cast<float>(static_cast<double>(q.QuadPart - g_lastQpc) * g_msPerTick);
        InterlockedExchange64(&b->frames, n + 1);  // after the entry (a full barrier)
    }
    g_lastQpc = q.QuadPart;
}

}  // namespace lsodiag
