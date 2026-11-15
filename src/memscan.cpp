#include "memscan.h"

#include "common.h"

#include <windows.h>

#include <psapi.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lsodiag {
namespace {

std::atomic<bool> g_running{false};

struct Target {
    const char* what;
    bool isDouble;
    uint64_t lo, hi;   // bit-pattern range (positive IEEE values are ordered like their bits)
};

uint64_t Bits(double v) {
    uint64_t b;
    memcpy(&b, &v, 8);
    return b;
}
uint32_t Bits(float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    return b;
}

void AddTargets(std::vector<Target>& t, const char* name, double v) {
    // float: within 2 ulp; double: within 1e-9 relative (the radian value may be computed differently)
    uint32_t fb = Bits(static_cast<float>(v));
    t.push_back({name, false, fb - 2, fb + 2});
    t.push_back({name, true, Bits(v * (1 - 1e-9)), Bits(v * (1 + 1e-9))});
}

struct Job {
    double deg;
    char label[64];
};

std::string ModuleOf(const void* p) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(p), &m) ||
        !m)
        return "";
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    char out[300];
    _snprintf_s(out, _countof(out), _TRUNCATE, "%s+0x%llX", base ? base + 1 : path,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(m)));
    return out;
}

DWORD WINAPI ScanThread(void* arg) {
    Job job = *static_cast<Job*>(arg);
    delete static_cast<Job*>(arg);
    std::vector<Target> targets;
    AddTargets(targets, "deg", job.deg);
    AddTargets(targets, "rad", job.deg * 3.14159265358979323846 / 180.0);
    AddTargets(targets, "tan", std::tan(job.deg * 3.14159265358979323846 / 180.0));
    Log("MEMSCAN [%s] searching for %.6g deg (float/double, also radians and tangent)", job.label, job.deg);
    const DWORD t0 = GetTickCount();
    constexpr int kMaxLogged = 300;
    int found = 0;
    unsigned long long scanned = 0;
    std::vector<unsigned char> buf;
    unsigned char* addr = nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        unsigned char* next = static_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool writable = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && writable && !(mbi.Protect & PAGE_GUARD) && mbi.RegionSize < (1ull << 31)) {
            buf.resize(mbi.RegionSize);
            if (SafeRead(mbi.BaseAddress, buf.data(), mbi.RegionSize)) {
                scanned += mbi.RegionSize;
                const size_t n = mbi.RegionSize;
                for (size_t off = 0; off + 4 <= n; off += 4) {
                    uint32_t w;
                    memcpy(&w, &buf[off], 4);
                    for (const Target& t : targets) {
                        bool hit = false;
                        if (!t.isDouble) {
                            hit = w >= t.lo && w <= t.hi;
                        } else if ((off & 7) == 0 && off + 8 <= n) {
                            uint64_t q;
                            memcpy(&q, &buf[off], 8);
                            hit = q >= t.lo && q <= t.hi;
                        }
                        if (!hit) continue;
                        ++found;
                        if (found <= kMaxLogged) {
                            unsigned char* a = static_cast<unsigned char*>(mbi.BaseAddress) + off;
                            char hex[3 * 48 + 1] = {};
                            size_t from = off >= 16 ? off - 16 : 0;
                            int h = 0;
                            for (size_t k = from; k < from + 48 && k < n; ++k)
                                h += _snprintf_s(hex + h, sizeof(hex) - h, _TRUNCATE, "%02X%s", buf[k],
                                                 k + 1 == off ? "|" : (k + 1 == off + (t.isDouble ? 8 : 4) ? "|" : " "));
                            std::string mod = ModuleOf(a);
                            Log("MEMSCAN [%s] %s %s at %p (region %p size 0x%llX %s%s%s) -16: %s", job.label, t.what,
                                t.isDouble ? "double" : "float", a, mbi.BaseAddress,
                                static_cast<unsigned long long>(mbi.RegionSize),
                                mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : "private",
                                mod.empty() ? "" : " ", mod.c_str(), hex);
                        }
                    }
                }
            }
        }
        if (next <= addr) break;
        addr = next;
    }
    Log("MEMSCAN [%s] done: %d hits (%d logged), %.0f MB scanned in %.1f s", job.label, found,
        found < kMaxLogged ? found : kMaxLogged, scanned / 1048576.0, (GetTickCount() - t0) / 1000.0);
    g_running = false;
    return 0;
}

}  // namespace

std::string MemScanStart(double degrees, const char* label) {
    if (!(degrees > 0.1 && degrees < 89)) return "angle out of range";
    if (g_running.exchange(true)) return "a scan is already running";
    auto* job = new Job{degrees, {}};
    strncpy_s(job->label, label ? label : "", _TRUNCATE);
    HANDLE th = CreateThread(nullptr, 0, ScanThread, job, 0, nullptr);
    if (!th) {
        delete job;
        g_running = false;
        return "CreateThread failed";
    }
    SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(th);
    return "";
}

}  // namespace lsodiag
