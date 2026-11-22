#include "own_canopy.h"

#include "common.h"
#include "compat.h"
#include "engine.h"

#include <windows.h>

#include <intrin.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace lsodiag {
namespace {

// NGModel.dll 2.9.31.29434 (same file in bin and bin-mt). See own_canopy.h and docs/FINDINGS.md.
constexpr uintptr_t kVtblSlotRva = 0x5A100;  // NGModelLodsMT vtable (+0x5A008), slot 31 (+0xF8) = ParseMT2
constexpr uintptr_t kParseRva = 0x3E030;     // NGModelLodsMT::ParseMT2 (exported)
constexpr uintptr_t kFetchCallRva = 0x3DBE4; // call +0x37650 (per-LOD instance of the cookie), inside +0x3DAC0
constexpr uintptr_t kFetchRva = 0x37650;     // (cookie slots, slot index, const args vector*, LOD root node, flag)
constexpr unsigned char kParseHead[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr unsigned char kFetchHead[] = {0x40, 0x56, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x40};
// The model's LOD table (constructor +0x3BA10): 16 entries of 0x50 bytes at +0xA0, filled from entry 0 upward
// and sorted by distance; +0x5A0 (qword) = number of entries loaded, +0x5B0 (dword) = first entry drawn (0, or 1
// when entry 0 has a negative distance, +0x427B1: the .lods file's collision_shell, loaded at -1e6). ParseMT2
// fetches entry i's instance from slot i - first, so a model with count entries uses slots 0 .. count - first - 1.
// (F/A-18C: 6 LOD files plus the collision shell = 7 entries from 1, slots 0..5.)
constexpr size_t kLodCountOffset = 0x5A0;
constexpr size_t kFirstLodOffset = 0x5B0;
constexpr int kLodSlots = 16;                // per-LOD instance slots in the cookie (0xF18 bytes each)
constexpr size_t kCookieSlots = 0x18;        // ... starting at cookie + 0x18 (the fetch's first argument)
constexpr size_t kSlotSize = 0xF18;          // slot: +0x00 LOD root it is bound to (set on first fetch), +0x08/+0x10
constexpr size_t kSlotArgs = 0x08;           //   its own copy of the arguments (begin, end)
constexpr size_t kCollCamera = 0x318;        // CollectionInfo: SceneRenderContext at +0x10, its camera Vec3d at +0x308
constexpr size_t kCollMainCam = 0x4E8;       // CollectionInfo: main camera Vec3d (context +0x4D8)
constexpr size_t kGraphCollBegin = 0x408, kGraphCollStride = 0x6D8;  // render graph's CollectionInfo array
constexpr size_t kArg = 114;                 // external cockpit / canopy / pilot visibility argument
constexpr size_t kCollMainRel = 0x500;      // CollectionInfo: main camera - frame origin (float3; context +0x4F0)
constexpr size_t kModelName = 0x10;          // NGModelLodsMT: name (ed::string; capacity at +0x28)
constexpr int kMaxViews = 8;
// Collections per parse call. 64 was too few: the frame has 54 with the PLAT alone and about 6 more per
// camera, so with the island and a deck camera every call was passed through untouched (run 2026-11-19).
constexpr unsigned kMaxInfos = 256;
constexpr size_t kMaxArgs = 2048;            // arguments copied for ILARTS's parse (per-thread buffer, no allocation)
constexpr double kSameCameraM2 = 0.05 * 0.05;
constexpr float kNearMainM = 15.0f;          // an object this close to your camera: your own aircraft (log)
constexpr int kMaxNearLogs = 6;              // distinct objects near your camera logged with their arguments
// Stats: a line every 15 s for the first 60 (15 minutes), then a summary every 5 minutes for the rest of the
// session (counters accumulate between lines, so nothing is lost after the first 15 minutes).
constexpr ULONGLONG kStatsMs = 15000;
constexpr int kMaxStatsLines = 60;
constexpr ULONGLONG kSummaryMs = 300000;
constexpr int kMaxSummaryLines = 288;        // 24 hours
constexpr int kOwnCookies = 4;               // graphics instances seen with argument 114 set ...
constexpr ULONGLONG kOwnCookieMs = 2000;     // ... within this long (diagnostics only, nothing is split for them)
constexpr long kSamplesPerWindow = 8;        // OWNCANOPY near lines per 15 s window ...
constexpr int kSampleWindows = 30;           // ... for this many windows
constexpr long kSamplesPerSummary = 2;       // then per summary line, for your aircraft only
constexpr int kMaxRefusalLogs = 6;
constexpr int kMaxAnomalyLogs = 8;           // per kind of anomaly
constexpr int kMaxTargetLogs = 16;           // split target changes (respawn, another object with 114 set)

using ParseFn = void (*)(void* self, void* inst, unsigned n, const void* const* infos, const bool* flags, void** outs,
                         void* sampler, const void* pos);
using FetchFn = void* (*)(void* slots, int64_t index, const void* args, const void* lodRoot, uint64_t flag);

// ed::vector<float> as NGModel reads it (begin, end; capacity unused).
struct FloatVec {
    const float* begin;
    const float* end;
    const float* cap;
};

struct Views {
    int n = 0;
    double cam[kMaxViews][3] = {};
};

bool g_installed = false;
volatile long g_enabled = 0;
ParseFn g_parse = nullptr;
FetchFn g_fetch = nullptr;
void** g_vtblSlot = nullptr;
unsigned char* g_callSite = nullptr;
unsigned char g_callOrig[5] = {};
unsigned char* g_stub = nullptr;

SRWLOCK g_viewLock = SRWLOCK_INIT;
Views g_pending, g_active;
volatile long g_loggedFirst = 0;
// This build's render graph CollectionInfo array (for the collection numbers in the near lines).
volatile uintptr_t g_collBegin = 0, g_collEnd = 0;

// Per thread: set while the ILARTS views' parse runs (the slot shift = number of LODs, the arguments, and
// the slots of the object being parsed: only its fetches are redirected).
thread_local int t_shift = 0;
thread_local void* t_slots = nullptr;
thread_local FloatVec t_vec = {};
thread_local float t_buf[kMaxArgs];
// Per thread, for a sampled near line: which slots the object's fetches used.
thread_local void* t_diagSlots = nullptr;
thread_local unsigned t_diagMask = 0;

volatile long long g_ownViews = 0;    // parses of your aircraft (114 set) that ILARTS views see
volatile long long g_split = 0;       // ... of those, also seen by DCS's own views (two parses)
volatile long long g_remaps = 0;      // per-LOD instances taken from the spare slots
volatile long long g_noSpare = 0;     // skipped: no spare slots (LOD table unreadable or more than 8 LODs)
volatile long long g_conflicts = 0;   // spare slot bound to another LOD root: that fetch not redirected
volatile long long g_notRedirected = 0;  // ILARTS's parse fetched a slot at or beyond the LOD count: left to DCS
volatile long long g_faults = 0;
// Fetch probe (every per-LOD fetch of every NGModel object, DCS's own included): never expected.
volatile long long g_dcsRange = 0;    // slot index outside 0..15 (DCS would address memory outside the cookie)
volatile long long g_dcsOtherRoot = 0;  // DCS's slot bound to another LOD root than the one it is fetched for

// Diagnostics (always on; ILARTS.log): counters per stats line, kept in per-thread-hashed shards so the
// render workers do not all hit one cache line.
enum Ctr {
    kCalls,           // parse calls seen (installed and enabled)
    kOverN,           // ... with more collections than kMaxInfos (passed through)
    kNoViews,         // ... while no ILARTS view was listed
    kWithOurs,        // ... with at least one ILARTS collection
    kArgsFail,        // arguments not readable
    kHas114,          // argument vector longer than 114
    kNonzero114,      // ... and 114 != 0
    kSet114,          // ... and 114 >= 0.5
    kSet114Ours,      // ... and an ILARTS collection in the call
    kNearMain,        // object within kNearMainM of your camera (your aircraft and what it carries)
    kNearMainOurs,    // ... with an ILARTS collection in the call
    kNearMain114,     // ... with 114 >= 0.5
    kShared,          // 114 < 0.5, another instance, graphics instance (cookie) seen with 114 set within 2 s
    kFetches,         // per-LOD fetches seen
    kCtrs
};
struct Shard {
    volatile long long c[kCtrs];
    char pad[128 - (kCtrs * 8) % 128];   // one shard per 128 bytes
};
Shard g_shards[16];
inline void Count(Ctr c) {
    InterlockedIncrement64(&g_shards[(GetCurrentThreadId() >> 2) & 15].c[c]);
}
volatile long g_maxN = 0, g_maxOurs = 0, g_maxArgs = 0;
SRWLOCK g_diagLock = SRWLOCK_INIT;
float g_min114 = 0, g_max114 = 0;     // non-zero values of 114 seen since the last stats line
bool g_any114 = false;
void* g_nearLogged[kMaxNearLogs] = {};
volatile long g_nearLogs = 0;
volatile long g_logged114 = 0;
volatile long g_sampleBudget = 0;     // near lines left in this stats window
volatile long g_sampleOwnOnly = 0;    // ... and whether only your aircraft's parses (114 set) may take one
int g_sampleWindows = 0;
volatile long g_refusalLogs = 0;
// Per build (render graph): how many builds, how many listed ILARTS views.
volatile long long g_builds = 0, g_buildsWithViews = 0;
volatile long g_maxViews = 0;
ULONGLONG g_nextStats = 0;
int g_statsLines = 0;
// The view line (when the number of listed views changes).
int g_viewLinesLast = -1, g_viewLines = 0;
ULONGLONG g_viewTick = 0;

// Anomaly lines: each kind has its own budget, so one kind cannot use up the others' (the stats lines keep
// counting every occurrence).
enum Anomaly { kAnFault, kAnConflict, kAnNotRedirected, kAnShared, kAnDcsRange, kAnDcsOtherRoot, kAnomalies };
volatile long g_anomalyLogs[kAnomalies] = {};
bool AnomalyLine(Anomaly a) { return InterlockedIncrement(&g_anomalyLogs[a]) <= kMaxAnomalyLogs; }

// The object the split last ran for (your aircraft): a line whenever it changes.
void* volatile g_targetInst = nullptr;
void* volatile g_targetSlots = nullptr;
volatile long g_targetLogs = 0;

// Graphics instances (cookie slot arrays) of objects seen with argument 114 set, and by which instance:
// DIAGNOSTICS ONLY. Until 2026-11-19 a parse of another instance using one of them was split as well (the
// 15:25 run's theory of two instances sharing your aircraft's graphics instance). The 21:06 run refuted that
// theory (three 'fa-18c' instances near your camera, three graphics instances), and cookies come from an
// ed::BlockPool that hands a freed block straight to the next object, so an address remembered for 2 s can
// belong to another object by then. Now such a parse is only counted and logged.
struct OwnCookie {
    void* volatile slots;
    void* volatile inst;
    volatile ULONGLONG tick;
};
OwnCookie g_own[kOwnCookies] = {};

void RememberOwn(void* slots, void* inst) {
    const ULONGLONG now = GetTickCount64();
    int oldest = 0;
    for (int i = 0; i < kOwnCookies; ++i) {
        if (g_own[i].slots == slots) {
            g_own[i].inst = inst;
            g_own[i].tick = now;
            return;
        }
        if (g_own[i].tick < g_own[oldest].tick) oldest = i;
    }
    g_own[oldest].tick = now;
    g_own[oldest].inst = inst;
    g_own[oldest].slots = slots;
}

// The instance that used `slots` with 114 set within the last 2 s, if it was another one than `inst`.
void* SharedWithOwn(const void* slots, const void* inst) {
    for (int i = 0; i < kOwnCookies; ++i) {
        if (g_own[i].slots != slots) continue;
        void* owner = g_own[i].inst;
        return owner != inst && GetTickCount64() - g_own[i].tick < kOwnCookieMs ? owner : nullptr;
    }
    return nullptr;
}

void RaiseMax(volatile long* m, long v) {
    long cur = *m;
    while (v > cur) {
        const long seen = InterlockedCompareExchange(m, v, cur);
        if (seen == cur) break;
        cur = seen;
    }
}

// Which of the collections are ILARTS's views (mine[i] = view number + 1, or 0). Returns how many, or -1 on
// a fault.
int Classify(unsigned n, const void* const* infos, const Views& v, unsigned char* mine) {
    __try {
        int k = 0;
        for (unsigned i = 0; i < n; ++i) {
            mine[i] = 0;
            const auto c = reinterpret_cast<const double*>(static_cast<const unsigned char*>(infos[i]) + kCollCamera);
            for (int j = 0; j < v.n; ++j) {
                const double dx = c[0] - v.cam[j][0], dy = c[1] - v.cam[j][1], dz = c[2] - v.cam[j][2];
                if (dx * dx + dy * dy + dz * dz < kSameCameraM2) {
                    mine[i] = static_cast<unsigned char>(j + 1);
                    ++k;
                    break;
                }
            }
        }
        return k;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The instance's draw arguments and graphics cookie, as NGModel gets them: instance vtable +0x38 (model
// params), its +0x08 (arguments) and +0x18 (cookie; the per-LOD slots start at cookie + 0x18).
bool ReadArgs(void* inst, const float** begin, size_t* count, void** slots) {
    __try {
        auto params = reinterpret_cast<void* (*)(void*)>((*static_cast<void***>(inst))[7])(inst);
        if (!params) return false;
        auto vec = reinterpret_cast<const FloatVec* (*)(void*)>((*static_cast<void***>(params))[1])(params);
        if (!vec || !vec->begin || vec->end < vec->begin) return false;
        auto cookie = reinterpret_cast<unsigned char* (*)(void*)>((*static_cast<void***>(params))[3])(params);
        if (!cookie) return false;
        *begin = vec->begin;
        *count = static_cast<size_t>(vec->end - vec->begin);
        *slots = cookie + kCookieSlots;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CopyFloats(float* dst, const float* src, size_t count) {
    __try {
        memcpy(dst, src, count * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadLods(const void* model, uint64_t* count, uint32_t* first) {
    __try {
        auto m = static_cast<const unsigned char*>(model);
        *count = *reinterpret_cast<const uint64_t*>(m + kLodCountOffset);
        *first = *reinterpret_cast<const uint32_t*>(m + kFirstLodOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const void* const kFaulted = reinterpret_cast<const void*>(~uintptr_t{0});

// The LOD root a slot is bound to (null while unused), or kFaulted. Only for 0 <= index < kLodSlots.
const void* SlotRoot(const void* slots, int64_t index) {
    __try {
        return *reinterpret_cast<const void* const*>(static_cast<const unsigned char*>(slots) + index * kSlotSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kFaulted;
    }
}

// Argument 114 in a slot's copy of the arguments: the value its visibility was last evaluated with.
// -1 = slot unused, -2 = its copy is shorter (the LOD does not use argument 114), -3 = fault.
float SlotArg(const void* slots, int index) {
    __try {
        auto s = static_cast<const unsigned char*>(slots) + index * kSlotSize;
        if (!*reinterpret_cast<const void* const*>(s)) return -1.0f;
        auto b = *reinterpret_cast<const float* const*>(s + kSlotArgs);
        auto e = *reinterpret_cast<const float* const*>(s + kSlotArgs + 8);
        if (!b || e < b || static_cast<size_t>(e - b) <= kArg) return -2.0f;
        return b[kArg];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -3.0f;
    }
}

// Distance from the object (the parse's position: row 3 of wPosition3<float>, relative to the frame origin)
// to your camera (first collection's main camera - frame origin). < 0 on a fault.
float DistToMainCamera(const void* pos, const void* info0) {
    __try {
        const float* p = reinterpret_cast<const float*>(static_cast<const unsigned char*>(pos) + 0x30);
        const float* m = reinterpret_cast<const float*>(static_cast<const unsigned char*>(info0) + kCollMainRel);
        const float dx = p[0] - m[0], dy = p[1] - m[1], dz = p[2] - m[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        return d2 >= 0 && d2 < 1e12f ? sqrtf(d2) : -1.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}

// A collection's camera against your (main) camera, metres; < 0 on a fault.
double CollCameraFromMain(const void* info) {
    __try {
        auto b = static_cast<const unsigned char*>(info);
        const double* c = reinterpret_cast<const double*>(b + kCollCamera);
        const double* m = reinterpret_cast<const double*>(b + kCollMainCam);
        const double dx = c[0] - m[0], dy = c[1] - m[1], dz = c[2] - m[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        return d2 >= 0 && d2 < 1e16 ? sqrt(d2) : -1.0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0;
    }
}

void ReadModelName(const void* model, char* out, size_t size) {
    out[0] = 0;
    __try {
        auto m = static_cast<const unsigned char*>(model);
        const uint64_t cap = *reinterpret_cast<const uint64_t*>(m + kModelName + 0x18);
        const char* s = cap > 15 ? *reinterpret_cast<const char* const*>(m + kModelName) : reinterpret_cast<const char*>(m + kModelName);
        size_t i = 0;
        for (; i + 1 < size && s[i]; ++i) out[i] = (s[i] >= 32 && s[i] < 127) ? s[i] : '?';
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}

// "i=v" for the non-zero arguments 0..150 (and 114 always).
void FormatArgs(const float* args, size_t count, char* out, size_t size) {
    float a[151] = {};
    const size_t n = count < 151 ? count : 151;
    out[0] = 0;
    if (!CopyFloats(a, args, n)) return;
    size_t len = 0;
    for (size_t i = 0; i < n && len + 24 < size; ++i) {
        if (a[i] == 0.0f && i != kArg) continue;
        const int w = _snprintf_s(out + len, size - len, _TRUNCATE, "%s%zu=%.3g", len ? " " : "", i, a[i]);
        if (w < 0) break;
        len += static_cast<size_t>(w);
    }
}

// Once per distinct object near your camera (up to kMaxNearLogs): what it is and its arguments.
__declspec(noinline) void LogNear(void* self, void* inst, float d, const float* args, size_t count, int ours, unsigned n) {
    for (int i = 0; i < kMaxNearLogs; ++i)
        if (g_nearLogged[i] == inst) return;
    const long slot = InterlockedIncrement(&g_nearLogs) - 1;
    if (slot >= kMaxNearLogs) return;
    g_nearLogged[slot] = inst;
    char name[64], list[2000];
    ReadModelName(self, name, sizeof(name));
    FormatArgs(args, count, list, sizeof(list));
    Log("OWNCANOPY object %.1f m from your camera: model '%s', %zu arguments, ILARTS collections %d of %u; non-zero arguments "
        "0..150: %s", d, name, count, ours, n, list);
}

// The collections of a call, for a near line: render graph entry number (or ?), V<j> for ILARTS's view j,
// else D<m> = DCS's, with its camera's distance from your camera in metres; '*' when the call's flag is set.
void FormatColls(unsigned n, const void* const* infos, const bool* flags, const unsigned char* mine, char* out, size_t size) {
    const uintptr_t b = g_collBegin, e = g_collEnd;
    size_t len = 0;
    out[0] = 0;
    for (unsigned i = 0; i < n && len + 32 < size; ++i) {
        if (i == 24) {
            _snprintf_s(out + len, size - len, _TRUNCATE, " +%u more", n - i);
            break;
        }
        const uintptr_t p = reinterpret_cast<uintptr_t>(infos[i]);
        char idx[12] = "?";
        if (b && p >= b && p < e && (p - b) % kGraphCollStride == 0)
            _snprintf_s(idx, _countof(idx), _TRUNCATE, "%u", static_cast<unsigned>((p - b) / kGraphCollStride));
        bool f = false;
        __try {
            f = flags[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        int w;
        if (mine[i])
            w = _snprintf_s(out + len, size - len, _TRUNCATE, "%s%s:V%d%s", len ? " " : "", idx, mine[i] - 1, f ? "*" : "");
        else
            w = _snprintf_s(out + len, size - len, _TRUNCATE, "%s%s:D%.0f%s", len ? " " : "", idx, CollCameraFromMain(infos[i]),
                            f ? "*" : "");
        if (w < 0) break;
        len += static_cast<size_t>(w);
    }
}

// Which slots the object's fetches used and the value of 114 each was evaluated with: "0=1 5=0".
void FormatSlots(const void* slots, unsigned mask, char* out, size_t size) {
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < kLodSlots && len + 16 < size; ++i) {
        if (!(mask & (1u << i))) continue;
        const int w = _snprintf_s(out + len, size - len, _TRUNCATE, "%s%d=%.3g", len ? " " : "", i, SlotArg(slots, i));
        if (w < 0) break;
        len += static_cast<size_t>(w);
    }
    if (!len) _snprintf_s(out, size, _TRUNCATE, "none");
}

void LogRefusal(void* self, const char* why, uint64_t count, uint32_t first) {
    if (InterlockedIncrement(&g_refusalLogs) > kMaxRefusalLogs) return;
    char name[64];
    ReadModelName(self, name, sizeof(name));
    Log("OWNCANOPY not split (%s): model '%s' (%p), LOD entries %llu, first %u, so %lld LOD(s)", why, name, self,
        static_cast<unsigned long long>(count), first, static_cast<long long>(count) - static_cast<long long>(first));
}

void NoteFault(const char* what, void* self) {
    InterlockedIncrement64(&g_faults);
    if (!AnomalyLine(kAnFault)) return;
    char name[64];
    ReadModelName(self, name, sizeof(name));
    Log("OWNCANOPY anomaly: fault reading %s (model '%s' %p); that parse left to DCS", what, name, self);
}

// A line whenever the object the split runs for changes: your aircraft after a respawn or slot change, or
// (never expected) another object with argument 114 set.
void NoteTarget(void* self, void* inst, void* slots, uint64_t count, uint32_t first, int lods) {
    void* const prevInst = g_targetInst;
    void* const prevSlots = g_targetSlots;
    if (prevInst == inst && prevSlots == slots) return;
    if (InterlockedCompareExchangePointer(&g_targetInst, inst, prevInst) != prevInst) return;   // another thread logs it
    g_targetSlots = slots;
    if (InterlockedIncrement(&g_targetLogs) > kMaxTargetLogs) return;
    char name[64];
    ReadModelName(self, name, sizeof(name));
    if (prevInst || prevSlots)
        Log("OWNCANOPY split target: model '%s' %p instance %p graphics %p, LOD entries %llu from %u = %d LODs (ILARTS's slots "
            "%d..%d); before: instance %p graphics %p",
            name, self, inst, slots, static_cast<unsigned long long>(count), first, lods, lods, 2 * lods - 1, prevInst, prevSlots);
    else
        Log("OWNCANOPY split target: model '%s' %p instance %p graphics %p, LOD entries %llu from %u = %d LODs (ILARTS's slots "
            "%d..%d)",
            name, self, inst, slots, static_cast<unsigned long long>(count), first, lods, lods, 2 * lods - 1);
}

struct ShiftScope {
    ShiftScope(int shift, void* slots, const float* args, size_t count) {
        t_vec.begin = args;
        t_vec.end = args + count;
        t_vec.cap = t_vec.end;
        t_slots = slots;
        t_shift = shift;
    }
    ~ShiftScope() {
        t_shift = 0;
        t_slots = nullptr;
    }
};

// What the wrapper did with a call (for the near lines).
enum class Did { Plain, NoSpare, Split, OnlyIlarts };

// Your aircraft (argument 114 set: you are in its cockpit), seen by one of ILARTS's views: ILARTS's
// collections parsed with a copy of the arguments in which 114 is 0, their per-LOD instances in the spare
// slots. Kept out of ParseWrap so the arrays below are on the stack only for this call (once a frame).
__declspec(noinline) Did SplitParse(void* self, void* inst, unsigned n, const void* const* infos, const bool* flags, void** outs,
                                    void* sampler, const void* pos, const unsigned char* mine, const float* args, size_t count,
                                    void* slots, float a114, uint64_t* lodCount, uint32_t* first) {
    InterlockedIncrement64(&g_ownViews);
    if (!ReadLods(self, lodCount, first)) {
        InterlockedIncrement64(&g_noSpare);
        NoteFault("the model's LOD table", self);
        g_parse(self, inst, n, infos, flags, outs, sampler, pos);
        return Did::NoSpare;
    }
    const int lods = *lodCount <= static_cast<uint64_t>(kLodSlots) && *first < *lodCount ? static_cast<int>(*lodCount - *first) : 0;
    if (lods <= 0 || lods * 2 > kLodSlots || count > kMaxArgs) {
        InterlockedIncrement64(&g_noSpare);
        LogRefusal(self, count > kMaxArgs ? "too many arguments" : lods <= 0 ? "LOD table implausible" : "more than 8 LODs",
                   *lodCount, *first);
        g_parse(self, inst, n, infos, flags, outs, sampler, pos);
        return Did::NoSpare;
    }
    float* buf = t_buf;
    if (!CopyFloats(buf, args, count)) {
        NoteFault("the arguments", self);
        g_parse(self, inst, n, infos, flags, outs, sampler, pos);
        return Did::Plain;
    }
    buf[kArg] = 0.0f;   // as seen from outside the cockpit
    // The two subsets, each in DCS's order. The one holding DCS's last collection runs last, so whatever the
    // model notes from the last collection of the call comes from the same collection as before.
    const void* infA[kMaxInfos];
    const void* infB[kMaxInfos];
    bool flA[kMaxInfos], flB[kMaxInfos];
    void* outA[kMaxInfos];
    void* outB[kMaxInfos];
    unsigned na = 0, nb = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (mine[i]) {
            infA[na] = infos[i], flA[na] = flags[i], outA[na] = outs[i], ++na;
        } else {
            infB[nb] = infos[i], flB[nb] = flags[i], outB[nb] = outs[i], ++nb;
        }
    }
    if (InterlockedCompareExchange(&g_loggedFirst, 1, 0) == 0) {
        char name[64];
        ReadModelName(self, name, sizeof(name));
        Log("OWNCANOPY first parse of your aircraft for ILARTS's views: model '%s', %u of %u collections, argument 114 = %.2f, "
            "LOD entries %llu from %u = %d LODs (DCS's slots 0..%d, ILARTS's %d..%d, evaluated with 114 = 0)",
            name, na, n, a114, static_cast<unsigned long long>(*lodCount), *first, lods, lods - 1, lods, 2 * lods - 1);
    }
    NoteTarget(self, inst, slots, *lodCount, *first, lods);
    auto runMine = [&] {
        ShiftScope s(lods, slots, buf, count);
        g_parse(self, inst, na, infA, flA, outA, sampler, pos);
    };
    if (nb == 0) {
        runMine();
        return Did::OnlyIlarts;
    }
    InterlockedIncrement64(&g_split);
    if (mine[n - 1]) {
        g_parse(self, inst, nb, infB, flB, outB, sampler, pos);
        runMine();
    } else {
        runMine();
        g_parse(self, inst, nb, infB, flB, outB, sampler, pos);
    }
    return Did::Split;
}

// A near line: who this object is (instance, graphics instance, arguments), which collections (views) the
// call carried, what the wrapper did, and which LOD slots were used with which value of 114.
__declspec(noinline) void LogSample(void* self, void* inst, float dist, unsigned n, const void* const* infos, const bool* flags,
                                    const unsigned char* mine, int k, const float* args, size_t count, void* slots, float a114,
                                    Did did, uint64_t lodCount, uint32_t first, unsigned mask) {
    char name[64], colls[700], used[160];
    ReadModelName(self, name, sizeof(name));
    FormatColls(n, infos, flags, mine, colls, sizeof(colls));
    FormatSlots(slots, mask, used, sizeof(used));
    const char* what = did == Did::Split ? "split" : did == Did::OnlyIlarts ? "ilarts-only" : did == Did::NoSpare ? "NOT split (no spare slots)" : "plain";
    Log("OWNCANOPY near: '%s' %.1f m, thread %lu, model %p instance %p graphics %p args %p (%zu), 114=%.3g | %u collections "
        "(%d ILARTS): %s | %s, LOD entries %llu from %u | slots used (114 there): %s",
        name, dist, GetCurrentThreadId(), self, inst, slots, args, count, a114, n, k, colls, what,
        static_cast<unsigned long long>(lodCount), first, used);
}

__declspec(noinline) void LogShared(void* self, void* inst, void* owner, void* slots, float a114, int k) {
    char name[64];
    ReadModelName(self, name, sizeof(name));
    Log("OWNCANOPY anomaly: graphics instance %p of your aircraft (instance %p, 114 set within 2 s) parsed by instance %p, "
        "model '%s' %p, 114 = %.3g, %d ILARTS collection(s); left to DCS",
        slots, owner, inst, name, self, a114, k);
}

void ParseWrap(void* self, void* inst, unsigned n, const void* const* infos, const bool* flags, void** outs, void* sampler,
               const void* pos) {
    if (!g_enabled || n == 0 || !infos || !flags || !outs || !inst || !pos || t_shift)
        return g_parse(self, inst, n, infos, flags, outs, sampler, pos);
    Count(kCalls);
    RaiseMax(&g_maxN, static_cast<long>(n));
    if (n > kMaxInfos) {
        Count(kOverN);
        return g_parse(self, inst, n, infos, flags, outs, sampler, pos);
    }
    Views v;
    AcquireSRWLockShared(&g_viewLock);
    v = g_active;
    ReleaseSRWLockShared(&g_viewLock);
    unsigned char mine[kMaxInfos];
    memset(mine, 0, n);
    int k = 0;
    if (v.n == 0) {
        Count(kNoViews);
    } else {
        k = Classify(n, infos, v, mine);
        if (k < 0) {
            memset(mine, 0, n);
            k = 0;
            NoteFault("the collections' cameras", self);
        }
        if (k > 0) {
            Count(kWithOurs);
            RaiseMax(&g_maxOurs, k);
        }
    }
    const float* args = nullptr;
    size_t count = 0;
    void* slots = nullptr;
    if (!ReadArgs(inst, &args, &count, &slots) || count > 65536) {
        Count(kArgsFail);
        return g_parse(self, inst, n, infos, flags, outs, sampler, pos);
    }
    RaiseMax(&g_maxArgs, static_cast<long>(count));
    float a114 = 0;
    if (count > kArg && CopyFloats(&a114, args + kArg, 1)) {
        Count(kHas114);
        if (a114 != 0.0f) {
            Count(kNonzero114);
            AcquireSRWLockExclusive(&g_diagLock);
            if (!g_any114 || a114 < g_min114) g_min114 = a114;
            if (!g_any114 || a114 > g_max114) g_max114 = a114;
            g_any114 = true;
            ReleaseSRWLockExclusive(&g_diagLock);
            if (InterlockedCompareExchange(&g_logged114, 1, 0) == 0) {
                char name[64];
                ReadModelName(self, name, sizeof(name));
                Log("OWNCANOPY first object with argument 114 = %.3f: model '%s', %zu arguments, ILARTS collections %d of %u",
                    a114, name, count, k, n);
            }
        }
    }
    const bool own = a114 >= 0.5f;
    if (own) {
        Count(kSet114);
        if (k > 0) Count(kSet114Ours);
        RememberOwn(slots, inst);
    } else if (void* owner = SharedWithOwn(slots, inst)) {
        // Another instance on a graphics instance your aircraft used within 2 s: not split (see g_own).
        Count(kShared);
        if (AnomalyLine(kAnShared)) LogShared(self, inst, owner, slots, a114, k);
    }
    bool sample = false;
    const float dist = DistToMainCamera(pos, infos[0]);
    if (dist >= 0 && dist < kNearMainM) {
        Count(kNearMain);
        if (k > 0) Count(kNearMainOurs);
        if (own) Count(kNearMain114);
        if (g_nearLogs < kMaxNearLogs) LogNear(self, inst, dist, args, count, k, n);
        sample = g_sampleBudget > 0 && (own || !g_sampleOwnOnly) && InterlockedDecrement(&g_sampleBudget) >= 0;
    }
    if (sample) {
        t_diagSlots = slots;
        t_diagMask = 0;
    }
    Did did = Did::Plain;
    uint64_t lodCount = 0;
    uint32_t first = 0;
    if (k > 0 && count > kArg && own)
        did = SplitParse(self, inst, n, infos, flags, outs, sampler, pos, mine, args, count, slots, a114, &lodCount, &first);
    else
        g_parse(self, inst, n, infos, flags, outs, sampler, pos);
    if (!sample) return;
    t_diagSlots = nullptr;
    LogSample(self, inst, dist, n, infos, flags, mine, k, args, count, slots, a114, did, lodCount, first, t_diagMask);
}

__declspec(noinline) void LogFetchAnomaly(Anomaly a, const void* slots, int64_t index, const void* lodRoot, const void* bound,
                                          int shift) {
    switch (a) {
    case kAnDcsRange:
        Log("OWNCANOPY anomaly: DCS fetched per-LOD slot %lld of graphics %p (LOD root %p): outside the cookie's %d slots "
            "(DCS's own fetch, left alone)",
            static_cast<long long>(index), slots, lodRoot, kLodSlots);
        break;
    case kAnDcsOtherRoot:
        Log("OWNCANOPY anomaly: DCS's per-LOD slot %lld of graphics %p is bound to LOD root %p but fetched for LOD root %p "
            "(DCS's own fetch, left alone)",
            static_cast<long long>(index), slots, bound, lodRoot);
        break;
    case kAnConflict:
        Log("OWNCANOPY anomaly: spare slot %lld of graphics %p bound to LOD root %p, wanted %p; that fetch left to DCS's slot %lld",
            static_cast<long long>(index + shift), slots, bound, lodRoot, static_cast<long long>(index));
        break;
    case kAnNotRedirected:
        Log("OWNCANOPY anomaly: ILARTS's parse fetched slot %lld of graphics %p (LOD root %p), %d LOD(s): left to DCS",
            static_cast<long long>(index), slots, lodRoot, shift);
        break;
    default:
        Log("OWNCANOPY anomaly: fault reading slot %lld of graphics %p", static_cast<long long>(index), slots);
        break;
    }
}

// Every per-LOD instance fetch of NGModelLodsMT::ParseMT2 comes through here (the redirected call): DCS's
// own fetches for every object, and those of ILARTS's parse of your aircraft. Only the latter are changed.
void* FetchWrap(void* slots, int64_t index, const void* args, const void* lodRoot, uint64_t flag) {
    Count(kFetches);
    if (index < 0 || index >= kLodSlots) {
        // Never expected (ParseMT2 fetches entry i into slot i - first, i >= first). Not ours to change.
        InterlockedIncrement64(&g_dcsRange);
        if (AnomalyLine(kAnDcsRange)) LogFetchAnomaly(kAnDcsRange, slots, index, lodRoot, nullptr, 0);
        return g_fetch(slots, index, args, lodRoot, flag);
    }
    const int shift = t_shift;
    bool redirected = false;
    if (shift > 0 && slots == t_slots) {
        if (index < shift && index + shift < kLodSlots) {
            // The spare slot must be unused or bound to this same LOD (a slot keeps the root of its first fetch).
            const void* bound = SlotRoot(slots, index + shift);
            if (bound == kFaulted) {
                InterlockedIncrement64(&g_faults);
                if (AnomalyLine(kAnFault)) LogFetchAnomaly(kAnFault, slots, index + shift, lodRoot, nullptr, 0);
            } else if (!bound || bound == lodRoot) {
                index += shift;
                args = &t_vec;
                redirected = true;
                InterlockedIncrement64(&g_remaps);
            } else {
                InterlockedIncrement64(&g_conflicts);
                if (AnomalyLine(kAnConflict)) LogFetchAnomaly(kAnConflict, slots, index, lodRoot, bound, shift);
            }
        } else {
            InterlockedIncrement64(&g_notRedirected);
            if (AnomalyLine(kAnNotRedirected)) LogFetchAnomaly(kAnNotRedirected, slots, index, lodRoot, nullptr, shift);
        }
    }
    if (!redirected) {
        // DCS's own slot: it should be unused or bound to this LOD's root. NGModel's fetch does not check that; a
        // slot bound to another LOD would have this LOD evaluated into an instance laid out for the other one.
        const void* bound = SlotRoot(slots, index);
        if (bound && bound != kFaulted && bound != lodRoot) {
            InterlockedIncrement64(&g_dcsOtherRoot);
            if (AnomalyLine(kAnDcsOtherRoot)) LogFetchAnomaly(kAnDcsOtherRoot, slots, index, lodRoot, bound, 0);
        }
    }
    if (slots == t_diagSlots) t_diagMask |= 1u << index;
    return g_fetch(slots, index, args, lodRoot, flag);
}

// Each mission gets its own first-time lines, line budgets and counters (install runs once per mission).
void ResetDiagnostics() {
    InterlockedExchange(&g_loggedFirst, 0);
    InterlockedExchange(&g_logged114, 0);
    for (void*& p : g_nearLogged) p = nullptr;
    InterlockedExchange(&g_nearLogs, 0);
    InterlockedExchange(&g_sampleBudget, 0);
    InterlockedExchange(&g_sampleOwnOnly, 0);
    g_sampleWindows = 0;
    InterlockedExchange(&g_refusalLogs, 0);
    for (volatile long& a : g_anomalyLogs) InterlockedExchange(&a, 0);
    InterlockedExchangePointer(&g_targetInst, nullptr);
    InterlockedExchangePointer(&g_targetSlots, nullptr);
    InterlockedExchange(&g_targetLogs, 0);
    for (OwnCookie& o : g_own) {
        o.slots = nullptr;
        o.inst = nullptr;
        o.tick = 0;
    }
    g_nextStats = 0;
    g_statsLines = 0;
    g_viewLinesLast = -1;
    g_viewLines = 0;
    g_viewTick = 0;
    for (Shard& s : g_shards)
        for (int i = 0; i < kCtrs; ++i) InterlockedExchange64(&s.c[i], 0);
    for (volatile long long* c : {&g_ownViews, &g_split, &g_remaps, &g_noSpare, &g_conflicts, &g_notRedirected, &g_faults,
                                  &g_dcsRange, &g_dcsOtherRoot, &g_builds, &g_buildsWithViews})
        InterlockedExchange64(c, 0);
    InterlockedExchange(&g_maxN, 0);
    InterlockedExchange(&g_maxOurs, 0);
    InterlockedExchange(&g_maxArgs, 0);
    InterlockedExchange(&g_maxViews, 0);
    AcquireSRWLockExclusive(&g_diagLock);
    g_any114 = false;
    ReleaseSRWLockExclusive(&g_diagLock);
}

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

// Replaces the 5 bytes at `at` (inside one 16-byte aligned block) atomically.
bool Write5(unsigned char* at, const unsigned char* expect, const unsigned char* bytes) {
    auto block = reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(at) & ~static_cast<uintptr_t>(15));
    const size_t off = static_cast<size_t>(at - block);
    if (off + 5 > 16) return false;
    DWORD old = 0;
    if (!VirtualProtect(block, 16, PAGE_EXECUTE_READWRITE, &old)) return false;
    alignas(16) long long cur[2];
    memcpy(cur, block, 16);
    bool ok = memcmp(reinterpret_cast<unsigned char*>(cur) + off, expect, 5) == 0;
    if (ok) {
        alignas(16) long long want[2];
        memcpy(want, cur, 16);
        memcpy(reinterpret_cast<unsigned char*>(want) + off, bytes, 5);
        alignas(16) long long cmp[2] = {cur[0], cur[1]};
        ok = InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(block), want[1], want[0], cmp) != 0;
    }
    DWORD tmp = 0;
    VirtualProtect(block, 16, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), block, 16);
    return ok;
}

bool WritePtr(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    return true;
}

}  // namespace

void OwnCanopyNoteView(const double cam[3]) {
    if (!cam) return;
    AcquireSRWLockExclusive(&g_viewLock);
    bool known = false;
    for (int j = 0; j < g_pending.n && !known; ++j)
        known = g_pending.cam[j][0] == cam[0] && g_pending.cam[j][1] == cam[1] && g_pending.cam[j][2] == cam[2];
    if (!known && g_pending.n < kMaxViews) {
        for (int i = 0; i < 3; ++i) g_pending.cam[g_pending.n][i] = cam[i];
        ++g_pending.n;
    }
    ReleaseSRWLockExclusive(&g_viewLock);
}

void OwnCanopyBuildEnd(const void* graph) {
    Views v;
    AcquireSRWLockExclusive(&g_viewLock);
    g_active = g_pending;
    v = g_active;
    g_pending.n = 0;
    ReleaseSRWLockExclusive(&g_viewLock);
    if (!g_installed) return;
    InterlockedIncrement64(&g_builds);
    if (v.n > 0) InterlockedIncrement64(&g_buildsWithViews);
    RaiseMax(&g_maxViews, v.n);
    {
        // This build's CollectionInfo array, for the collection numbers in the near lines.
        uintptr_t r[2] = {};
        if (!graph || !SafeRead(static_cast<const char*>(graph) + kGraphCollBegin, r, sizeof(r)) || r[1] < r[0]) r[0] = r[1] = 0;
        g_collBegin = r[0];
        g_collEnd = r[1];
    }
    const ULONGLONG now = GetTickCount64();
    if (!g_nextStats) {
        g_nextStats = now + kStatsMs;
        InterlockedExchange(&g_sampleOwnOnly, 0);
        InterlockedExchange(&g_sampleBudget, kSamplesPerWindow);
        ++g_sampleWindows;
    }
    if (now >= g_nextStats && g_statsLines < kMaxStatsLines + kMaxSummaryLines) {
        const bool summary = g_statsLines >= kMaxStatsLines;   // this line closes a 5-minute period
        ++g_statsLines;
        g_nextStats = now + (g_statsLines < kMaxStatsLines ? kStatsMs : kSummaryMs);
        // The near lines of the next period (the first parses of objects near your camera after this point): 8 per
        // 15 s window at first, then 2 per summary period and only for your aircraft (its slots and 114 values).
        if (g_sampleWindows < kSampleWindows) {
            ++g_sampleWindows;
            InterlockedExchange(&g_sampleOwnOnly, 0);
            InterlockedExchange(&g_sampleBudget, kSamplesPerWindow);
        } else if (g_statsLines >= kMaxStatsLines) {
            InterlockedExchange(&g_sampleOwnOnly, 1);
            InterlockedExchange(&g_sampleBudget, kSamplesPerSummary);
        } else {
            InterlockedExchange(&g_sampleBudget, 0);
        }
        long long c[kCtrs] = {};
        for (Shard& s : g_shards)
            for (int i = 0; i < kCtrs; ++i) c[i] += InterlockedExchange64(&s.c[i], 0);
        float mn = 0, mx = 0;
        bool any = false;
        AcquireSRWLockExclusive(&g_diagLock);
        mn = g_min114, mx = g_max114, any = g_any114;
        g_any114 = false;
        ReleaseSRWLockExclusive(&g_diagLock);
        char range[48] = "none";
        if (any) _snprintf_s(range, _countof(range), _TRUNCATE, "%.3f..%.3f", mn, mx);
        Log("OWNCANOPY stats %ds: builds=%lld with_views=%lld views_max=%ld last_views=%d | parses=%lld over_%u=%lld (max n %ld) "
            "no_views=%lld with_ilarts=%lld (max %ld of them) args_unreadable=%lld (max args %ld) | arg114: present=%lld "
            "nonzero=%lld (%s) set=%lld set_with_ilarts=%lld | near_your_camera=%lld with_ilarts=%lld with_114=%lld | "
            "shared_graphics=%lld | own_parses=%lld split=%lld lod_remaps=%lld slot_conflicts=%lld not_redirected=%lld "
            "no_spare_slots=%lld faults=%lld | fetches=%lld dcs_slot_out_of_range=%lld dcs_slot_other_root=%lld",
            static_cast<int>((summary ? kSummaryMs : kStatsMs) / 1000), InterlockedExchange64(&g_builds, 0),
            InterlockedExchange64(&g_buildsWithViews, 0), InterlockedExchange(&g_maxViews, 0), v.n, c[kCalls], kMaxInfos, c[kOverN],
            InterlockedExchange(&g_maxN, 0), c[kNoViews], c[kWithOurs], InterlockedExchange(&g_maxOurs, 0), c[kArgsFail],
            InterlockedExchange(&g_maxArgs, 0), c[kHas114], c[kNonzero114], range, c[kSet114], c[kSet114Ours], c[kNearMain],
            c[kNearMainOurs], c[kNearMain114], c[kShared], InterlockedExchange64(&g_ownViews, 0), InterlockedExchange64(&g_split, 0),
            InterlockedExchange64(&g_remaps, 0), InterlockedExchange64(&g_conflicts, 0), InterlockedExchange64(&g_notRedirected, 0),
            InterlockedExchange64(&g_noSpare, 0), InterlockedExchange64(&g_faults, 0), c[kFetches], InterlockedExchange64(&g_dcsRange, 0),
            InterlockedExchange64(&g_dcsOtherRoot, 0));
        if (g_statsLines == kMaxStatsLines)
            Log("OWNCANOPY stats: from now on one line every %d s (counts over that time)", static_cast<int>(kSummaryMs / 1000));
        else if (g_statsLines == kMaxStatsLines + kMaxSummaryLines)
            Log("OWNCANOPY stats: last line (limit %d)", kMaxStatsLines + kMaxSummaryLines);
    }
    // When the number of listed views changes (at most every 10 s, 20 lines): how many of the render graph's
    // collections carry one of these cameras (0 would mean the camera offset is wrong).
    if (v.n == g_viewLinesLast || now - g_viewTick < 10000 || g_viewLines >= 20 || !graph) return;
    g_viewLinesLast = v.n;
    g_viewTick = now;
    ++g_viewLines;
    unsigned char* range[2] = {};
    int total = 0, matched = 0;
    char which[kMaxViews][96] = {};   // per view: its collection numbers
    if (SafeRead(static_cast<const char*>(graph) + kGraphCollBegin, range, sizeof(range)) && range[0] && range[1] > range[0] &&
        static_cast<size_t>(range[1] - range[0]) % kGraphCollStride == 0 &&
        static_cast<size_t>(range[1] - range[0]) / kGraphCollStride <= kMaxInfos) {
        for (unsigned char* e = range[0]; e < range[1]; e += kGraphCollStride) {
            ++total;
            double c[3];
            if (!SafeRead(e + kCollCamera, c, sizeof(c))) continue;
            for (int j = 0; j < v.n; ++j) {
                const double dx = c[0] - v.cam[j][0], dy = c[1] - v.cam[j][1], dz = c[2] - v.cam[j][2];
                if (dx * dx + dy * dy + dz * dz < kSameCameraM2) {
                    ++matched;
                    const size_t len = strlen(which[j]);
                    _snprintf_s(which[j] + len, sizeof(which[j]) - len, _TRUNCATE, "%s%d", len ? "," : "", total - 1);
                    break;
                }
            }
        }
    }
    char list[kMaxViews * 140] = {};
    for (int j = 0; j < v.n; ++j) {
        const size_t len = strlen(list);
        _snprintf_s(list + len, sizeof(list) - len, _TRUNCATE, "; V%d camera (%.1f, %.1f, %.1f): %s", j, v.cam[j][0], v.cam[j][1],
                    v.cam[j][2], which[j][0] ? which[j] : "none");
    }
    Log("OWNCANOPY %d ILARTS view(s) this frame; %d of %d render collections are theirs%s", v.n, matched, total, list);
}

std::string OwnCanopyInstall() {
    if (EngineDisabled()) return "engine calls disabled";
    if (g_installed) return "";
    HMODULE ng = GetModuleHandleW(L"NGModel.dll");
    if (!ng) return "NGModel.dll not loaded";
    if (!CompatOk()) return CompatWhy();
    auto base = reinterpret_cast<unsigned char*>(ng);
    unsigned char head[sizeof(kParseHead)];
    if (!SafeRead(base + kParseRva, head, sizeof(head)) || memcmp(head, kParseHead, sizeof(head)) != 0)
        return "NGModel+0x3E030 is not the expected code";
    unsigned char fhead[sizeof(kFetchHead)];
    if (!SafeRead(base + kFetchRva, fhead, sizeof(fhead)) || memcmp(fhead, kFetchHead, sizeof(fhead)) != 0)
        return "NGModel+0x37650 is not the expected code";
    void** slot = reinterpret_cast<void**>(base + kVtblSlotRva);
    void* cur = nullptr;
    if (!SafeRead(slot, &cur, sizeof(cur)) || cur != base + kParseRva) return "NGModelLodsMT vtable slot 31 is not ParseMT2";
    unsigned char call[5];
    if (!SafeRead(base + kFetchCallRva, call, sizeof(call)) || call[0] != 0xE8) return "NGModel+0x3DBE4 is not a call";
    int32_t rel = 0;
    memcpy(&rel, call + 1, 4);
    if (base + kFetchCallRva + 5 + rel != base + kFetchRva) return "NGModel+0x3DBE4 calls something else";
    ResetDiagnostics();
    g_parse = reinterpret_cast<ParseFn>(base + kParseRva);
    g_fetch = reinterpret_cast<FetchFn>(base + kFetchRva);
    g_callSite = base + kFetchCallRva;
    memcpy(g_callOrig, call, sizeof(call));
    if (!g_stub) {
        unsigned char* mem = AllocNear(g_callSite, 64);
        if (!mem) return "no memory near NGModel.dll";
        mem[0] = 0xFF;  // jmp [rip+0] ; dq FetchWrap
        mem[1] = 0x25;
        memset(mem + 2, 0, 4);
        const uint64_t target = reinterpret_cast<uint64_t>(&FetchWrap);
        memcpy(mem + 6, &target, 8);
        DWORD old = 0;
        if (!VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &old)) return "VirtualProtect (stub) failed";
        FlushInstructionCache(GetCurrentProcess(), mem, 64);
        g_stub = mem;
    }
    const long long nrel = reinterpret_cast<intptr_t>(g_stub) - reinterpret_cast<intptr_t>(g_callSite + 5);
    if (nrel < INT32_MIN || nrel > INT32_MAX) return "stub out of rel32 range";
    unsigned char nb[5] = {0xE8, 0, 0, 0, 0};
    const int32_t r32 = static_cast<int32_t>(nrel);
    memcpy(nb + 1, &r32, 4);
    // The fetch redirect first (it does nothing until a parse for ILARTS's views runs), then the parse.
    if (!Write5(g_callSite, g_callOrig, nb)) return "could not patch NGModel+0x3DBE4";
    if (!WritePtr(slot, reinterpret_cast<void*>(&ParseWrap))) {
        Write5(g_callSite, nb, g_callOrig);
        return "could not patch the NGModelLodsMT vtable";
    }
    g_vtblSlot = slot;
    g_installed = true;
    Log("OWNCANOPY installed: NGModelLodsMT::ParseMT2 (vtable NGModel+0x5A100) -> wrapper, NGModel+0x3DBE4 (per-LOD "
        "instance) -> stub %p", g_stub);
    return "";
}

void OwnCanopyRemove() {
    g_enabled = 0;
    // No view list carries over into the next mission (its first parses would match cameras of this one).
    AcquireSRWLockExclusive(&g_viewLock);
    g_pending.n = 0;
    g_active.n = 0;
    ReleaseSRWLockExclusive(&g_viewLock);
    if (!g_installed) return;
    // The parse first (no new ILARTS parse starts), then the fetch call. A render worker still inside the
    // wrapper, the stub or FetchWrap finishes normally: the code stays loaded and the stub stays allocated.
    WritePtr(g_vtblSlot, reinterpret_cast<void*>(g_parse));
    unsigned char nb[5];
    memcpy(nb, g_callSite, 5);
    Write5(g_callSite, nb, g_callOrig);
    g_installed = false;
    Log("OWNCANOPY removed (NGModel restored; the stub stays allocated)");
}

void OwnCanopyEnable(bool on) { InterlockedExchange(&g_enabled, on ? 1 : 0); }

#ifdef OWN_CANOPY_TEST
// tools/own_canopy_test.cpp: drives the wrappers with stand-ins for NGModel's functions.
void OwnCanopyTestSet(void* parse, void* fetch) {
    g_parse = reinterpret_cast<ParseFn>(parse);
    g_fetch = reinterpret_cast<FetchFn>(fetch);
}
void* OwnCanopyTestParseWrap() { return reinterpret_cast<void*>(&ParseWrap); }
void* OwnCanopyTestFetchWrap() { return reinterpret_cast<void*>(&FetchWrap); }
void OwnCanopyTestStatsNow() { g_nextStats = 1; }
void OwnCanopyTestSampleNow() { g_sampleBudget = kSamplesPerWindow; }
void OwnCanopyTestReset() { ResetDiagnostics(); }
void OwnCanopyTestInstalled(bool on) { g_installed = on; }
#endif

std::string OwnCanopyStats() {
    Views v;
    AcquireSRWLockShared(&g_viewLock);
    v = g_active;
    ReleaseSRWLockShared(&g_viewLock);
    char b[512];
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "installed=%d enabled=%d ilarts_views=%d (since the last OWNCANOPY stats line:) own_parses=%lld split=%lld "
                "lod_remaps=%lld slot_conflicts=%lld not_redirected=%lld no_spare_slots=%lld faults=%lld dcs_slot_out_of_range=%lld "
                "dcs_slot_other_root=%lld",
                g_installed ? 1 : 0, g_enabled ? 1 : 0, v.n, g_ownViews, g_split, g_remaps, g_conflicts, g_notRedirected, g_noSpare,
                g_faults, g_dcsRange, g_dcsOtherRoot);
    return b;
}

}  // namespace lsodiag
