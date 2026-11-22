#include "origin_split.h"

#include "common.h"
#include "compat.h"
#include "engine.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace lsodiag {
namespace {

// Visualizer.dll 2.9.31.29434. GraphSceneObject (RTTI .?AVGraphSceneObject@Graphics@@, its ISceneParsable part at
// +0x48) has its vtable for that part at +0x23BEF8 (complete object locator +0x256548 just before it): [0] +0x11F428,
// [1] +0x1D4EB3 = collectRenderables, [2] +0x8F8A0, [3] +0x9F680. +0x1D4EB3 is `jmp [rip+X]` through the import
// slot +0x22E3E0 of GraphicsVista's ?collectRenderables@GraphSceneObject@Graphics@@... (export +0xC8C0); it is the
// only reference to that import, and Visualizer is the only module importing it.
// Scene.dll calls it per object at +0xEF5B / +0xF95B: `mov r10,[vtable+8] ... call r10` with
// (this, n, CollectionInfo const** infos, bool const* flags, vector<ISceneRenderable*>** outs, ILightProbeSampler&).
constexpr uintptr_t kSlotRva = 0x23BF00;
constexpr uintptr_t kThunkRva = 0x1D4EB3;
constexpr uintptr_t kIatRva = 0x22E3E0;
constexpr unsigned char kThunk[] = {0xFF, 0x25, 0x27, 0x95, 0x05, 0x00};   // jmp [rip+0x59527] -> +0x22E3E0
constexpr const char* kCollectName =
    "?collectRenderables@GraphSceneObject@Graphics@@UEAAXIPEAPEBUCollectionInfo@render@@QEB_NPEAPEAV?$vector@"
    "PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@AEAUILightProbeSampler@4@@Z";
// GraphicsVista.dll +0xC8C0 (same build): `mov rax,[r8]` (infos[0]) at +0xC93D, then the object's position minus
// infos[0]'s origin: movss [rax+0x514] / [rax+0x510] / [rax+0x50C] at +0xC9AC / +0xC9C0 / +0xC9D4. That one position
// goes to the model's ParseMT2 (+0xD4CE) and to the per-collection "dot" test for every collection of the call.
constexpr uintptr_t kGvFnRva = 0xC8C0;
struct CodeBytes {
    uintptr_t rva;
    unsigned char b[8];
    size_t n;
};
constexpr CodeBytes kGvCode[] = {
    {0xC93D, {0x49, 0x8B, 0x00}, 3},                                        // mov rax,[r8]
    {0xC9AC, {0xF3, 0x0F, 0x10, 0x80, 0x14, 0x05, 0x00, 0x00}, 8},          // movss xmm0,[rax+514h]
    {0xC9C0, {0xF3, 0x0F, 0x10, 0x88, 0x10, 0x05, 0x00, 0x00}, 8},          // movss xmm1,[rax+510h]
    {0xC9D4, {0xF3, 0x0F, 0x10, 0x80, 0x0C, 0x05, 0x00, 0x00}, 8},          // movss xmm0,[rax+50Ch]
};
// enlight.dll 2.9.31.29434. SplineRopeParsable (RTTI .?AVSplineRopeParsable@enlight@@, ISceneParsable at +0): the
// arresting cable while it is engaged - Transport.dll's woShip::initArrestingGears (+0x149360) creates its
// SplineRopeProxy (ship +0x16B8), the arresting gear update (+0x15A1F9) feeds it the cable's points in world space
// (updateParams), hide() when the cable rests (the static wires are part of the carrier model). Its vtable is the
// export ??_7SplineRopeParsable@enlight@@6B@ (+0x7FE08); slot 1 (+0x7FE10, relocated pointer) = the exported
// collectRenderables +0x63B50, same arguments as GraphSceneObject's. It loops over the collections, and for the first
// one that sees the cable (+0x63CEF, again +0x63DCD for the shadow path) computes the cable's points ONCE per call:
// +0x63550 (rope, &infos[i]->origin +0x50C) = each double point minus that float origin (+0x63620 / +0x63637 /
// +0x6364E), in frame memory; that one array goes into every collection's renderable. Same bug as above.
constexpr uintptr_t kRopeVtRva = 0x7FE08;
constexpr uintptr_t kRopeSlotRva = 0x7FE10;
constexpr uintptr_t kRopeFnRva = 0x63B50;
constexpr const char* kRopeVtName = "??_7SplineRopeParsable@enlight@@6B@";
constexpr const char* kRopeCollectName =
    "?collectRenderables@SplineRopeParsable@enlight@@UEAAXIPEAPEBUCollectionInfo@render@@QEB_NPEAPEAV?$vector@"
    "PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@AEAUILightProbeSampler@4@@Z";
constexpr CodeBytes kRopeCode[] = {
    {0x63B50, {0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20}, 7},               // mov rax,rsp; mov [rax+20h],r9
    {0x63BE0, {0x49, 0x8B, 0x1C, 0x0F}, 4},                                 // mov rbx,[r15+rcx] (infos[i])
    {0x63CEF, {0x4D, 0x85, 0xED, 0x75, 0x13, 0x48, 0x8B, 0x4E}, 8},         // test r13,r13; jne; mov rcx,[rsi+8]
    {0x63CF8, {0x48, 0x8D, 0x93, 0x0C, 0x05, 0x00, 0x00, 0xE8}, 8},         // lea rdx,[rbx+50Ch]; call +0x63550
    {0x63DCD, {0x4D, 0x85, 0xED, 0x75, 0x13, 0x48, 0x8B, 0x4E}, 8},         // the same for the shadow path
    {0x63DD6, {0x48, 0x8D, 0x93, 0x0C, 0x05, 0x00, 0x00, 0xE8}, 8},
    {0x63620, {0xF3, 0x0F, 0x10, 0x46, 0x04}, 5},                           // movss xmm0,[rsi+4]
    {0x63637, {0xF3, 0x0F, 0x10, 0x56, 0x08}, 5},                           // movss xmm2,[rsi+8]
    {0x6364E, {0xF3, 0x0F, 0x10, 0x0E}, 4},                                 // movss xmm1,[rsi]
};
constexpr size_t kOriginOffset = 0x50C;   // CollectionInfo: float3 origin (0x10 header + SceneRenderContext +0x4FC)
constexpr unsigned kMaxInfos = 256;
constexpr int kMaxGroups = 4;
// For the log: the GraphSceneObject's Model* (ModelPtr) at -0x30 from the ISceneParsable part; an NGModelLodsMT
// (vtable NGModel.dll +0x5A008) has its name as an ed::string at +0x10 (capacity +0x28).
constexpr ptrdiff_t kModelPtrOffset = -0x30;
constexpr uintptr_t kNgVtableRva = 0x5A008;
constexpr size_t kModelName = 0x10;
constexpr int kModelsPerEpisode = 6;   // per-object lines per far-view episode (models and cables together)
constexpr ULONGLONG kEpisodeGapMs = 60000;   // a new far-view episode (new model lines) at most once a minute
constexpr ULONGLONG kStatsEveryMs = 30000;

using CollectFn = void (*)(void* self, unsigned n, const void* const* infos, const bool* flags, void** outs,
                           void* sampler);

// The wrapped vtable slots, one per class, each with its own original. The log names the class by `what`.
enum { kModel = 0, kRope = 1, kTargets = 2 };
struct Target {
    const char* what;    // in the log lines
    const char* where;   // the patched slot, for the install lines
    void** slot;
    CollectFn orig;
    bool installed;
    volatile long long mixed, split;   // totals, per class
};
Target g_t[kTargets] = {
    {"GraphSceneObject collectRenderables (models)", "Visualizer+0x23BF00", nullptr, nullptr, false, 0, 0},
    {"SplineRopeParsable collectRenderables (arresting cables)", "enlight+0x7FE10", nullptr, nullptr, false, 0, 0},
};
bool g_installed = false;
const void* g_ngVtable = nullptr;
std::atomic<bool> g_enabled{true};

// The carrier views' origin of the current frame (for the log only: which kind of collection came first).
std::atomic<uint32_t> g_carrierBits[3] = {};
std::atomic<int> g_carrierSet{0};

// Totals (never reset); the periodic line prints what changed since the previous line.
volatile long long g_mixed = 0, g_ilartsFirst = 0, g_dcsFirst = 0, g_splitCalls = 0, g_overCap = 0, g_tooMany = 0,
                   g_faults = 0;
volatile long g_groupsMax = 0;

// Per-object lines: the first kModelsPerEpisode objects (models by their Model*, cables by themselves) per far-view
// episode.
SRWLOCK g_seenLock = SRWLOCK_INIT;
const void* g_seen[kModelsPerEpisode] = {};
std::atomic<int> g_seenN{0};

// Frame side (the render graph build thread only).
long long s_farFrames = 0;
double s_distMin = -1, s_distMax = -1;
bool s_wasFar = false;
ULONGLONG s_episodeTick = 0, s_nextStats = 0;
constexpr int kStatN = 7 + 2 * kTargets;
long long s_last[kStatN] = {};

bool SameOrigin(const float* a, const float* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

// group[i] = index of collection i's origin among the distinct ones (in order of first appearance), origins[] =
// those. Returns how many there are; 0 = a collection could not be read; -1 = more than kMaxGroups.
int Classify(unsigned n, const void* const* infos, unsigned char* group, float (*origins)[3]) {
    __try {
        int ng = 0;
        for (unsigned i = 0; i < n; ++i) {
            const float* o = reinterpret_cast<const float*>(static_cast<const char*>(infos[i]) + kOriginOffset);
            int g = 0;
            while (g < ng && !SameOrigin(origins[g], o)) ++g;
            if (g == ng) {
                if (ng == kMaxGroups) return -1;
                origins[ng][0] = o[0], origins[ng][1] = o[1], origins[ng][2] = o[2];
                ++ng;
            }
            group[i] = static_cast<unsigned char>(g);
        }
        return ng;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// True when the collections carry more than one origin (calls with more than kMaxInfos, which are not split).
bool Mixed(unsigned n, const void* const* infos) {
    __try {
        const float* o0 = reinterpret_cast<const float*>(static_cast<const char*>(infos[0]) + kOriginOffset);
        for (unsigned i = 1; i < n; ++i)
            if (!SameOrigin(o0, reinterpret_cast<const float*>(static_cast<const char*>(infos[i]) + kOriginOffset)))
                return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool ReadCarrier(float out[3]) {
    if (!g_carrierSet.load(std::memory_order_acquire)) return false;
    for (int i = 0; i < 3; ++i) {
        const uint32_t b = g_carrierBits[i].load(std::memory_order_relaxed);
        memcpy(&out[i], &b, 4);
    }
    return true;
}

const void* ReadModel(const void* self) {
    __try {
        return *reinterpret_cast<const void* const*>(static_cast<const char*>(self) + kModelPtrOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The model's name if it is an NGModelLodsMT (EDM model), else "".
void ReadModelName(const void* model, char* out, size_t size) {
    out[0] = 0;
    if (!model || !g_ngVtable) return;
    __try {
        auto m = static_cast<const unsigned char*>(model);
        if (*reinterpret_cast<const void* const*>(m) != g_ngVtable) return;
        const uint64_t cap = *reinterpret_cast<const uint64_t*>(m + kModelName + 0x18);
        const char* s = cap > 15 ? *reinterpret_cast<const char* const*>(m + kModelName) : reinterpret_cast<const char*>(m + kModelName);
        size_t i = 0;
        for (; i + 1 < size && s[i]; ++i) out[i] = (s[i] >= 32 && s[i] < 127) ? s[i] : '?';
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}

void RaiseMax(volatile long* at, long v) {
    long cur = *at;
    while (v > cur) {
        const long seen = InterlockedCompareExchange(at, v, cur);
        if (seen == cur) break;
        cur = seen;
    }
}

// One line for the first calls of an object in a far-view episode: what DCS would have done without the split.
__declspec(noinline) void NoteModel(int t, const void* self, unsigned n, const unsigned char* group, int ng,
                                    const float (*origins)[3], int carrierGroup) {
    const void* key = t == kModel ? ReadModel(self) : self;
    AcquireSRWLockExclusive(&g_seenLock);
    int seen = g_seenN.load(std::memory_order_relaxed);
    bool log = seen < kModelsPerEpisode;
    for (int i = 0; log && i < seen; ++i)
        if (g_seen[i] == key) log = false;
    if (log) {
        g_seen[seen] = key;
        g_seenN.store(seen + 1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_seenLock);
    if (!log) return;
    unsigned counts[kMaxGroups] = {};
    for (unsigned i = 0; i < n; ++i) ++counts[group[i]];
    double off = 0;   // farthest origin from the first collection's
    for (int g = 1; g < ng; ++g) {
        const double dx = origins[g][0] - origins[0][0], dy = origins[g][1] - origins[0][1], dz = origins[g][2] - origins[0][2];
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > off) off = d;
    }
    char obj[120];
    if (t == kModel) {
        char name[64];
        ReadModelName(key, name, sizeof(name));
        _snprintf_s(obj, _countof(obj), _TRUNCATE, "model '%s' (object %p)", *name ? name : "?", key);
    } else {
        _snprintf_s(obj, _countof(obj), _TRUNCATE, "arresting cable (SplineRopeParsable %p)", key);
    }
    char what[200];
    if (carrierGroup < 0)
        _snprintf_s(what, _countof(what), _TRUNCATE, "the carrier views' origin is not known this frame");
    else if (carrierGroup == 0)
        _snprintf_s(what, _countof(what), _TRUNCATE,
                    "the first was ILARTS's: unsplit, DCS's views would draw it %.1f km from where it is, next to your camera",
                    off / 1000.0);
    else
        _snprintf_s(what, _countof(what), _TRUNCATE,
                    "the first was DCS's: unsplit, ILARTS's views would draw it %.1f km from where it is (missing there)",
                    off / 1000.0);
    const unsigned atCarrier = carrierGroup >= 0 ? counts[carrierGroup] : 0;
    Log("PLATORIGIN split: %s in %u collections with %d origins, %u at the carrier views' origin; %s%s", obj, n, ng,
        atCarrier, what, g_enabled.load(std::memory_order_relaxed) ? "" : " [split OFF]");
}

// One DCS call per origin, each with its collections in DCS's order; the group holding the last collection runs last,
// so whatever the object notes from the last collection of the call comes from the same one as before. Separate (and
// not inlined) so that the wrapper's common path keeps a small stack frame on DCS's worker threads.
__declspec(noinline) void SplitCall(CollectFn orig, void* self, unsigned n, const void* const* infos, const bool* flags,
                                    void** outs, void* sampler, const unsigned char* group, int ng) {
    const int lastGroup = group[n - 1];
    const void* inf[kMaxInfos];
    bool fl[kMaxInfos];
    void* out[kMaxInfos];
    for (int pass = 0; pass < ng; ++pass) {
        const int g = pass == ng - 1 ? lastGroup : (pass < lastGroup ? pass : pass + 1);
        unsigned k = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (group[i] != g) continue;
            inf[k] = infos[i];
            fl[k] = flags ? flags[i] : false;
            out[k] = outs[i];
            ++k;
        }
        if (k) orig(self, k, inf, flags ? fl : nullptr, out, sampler);
    }
}

// The wrapper for the class t (each slot gets its own instance, so the common path needs no lookup).
template <int t>
void CollectWrap(void* self, unsigned n, const void* const* infos, const bool* flags, void** outs, void* sampler) {
    const CollectFn orig = g_t[t].orig;
    if (n < 2 || !infos || !outs) return orig(self, n, infos, flags, outs, sampler);
    if (n > kMaxInfos) {
        if (Mixed(n, infos)) InterlockedIncrement64(&g_overCap);
        return orig(self, n, infos, flags, outs, sampler);
    }
    unsigned char group[kMaxInfos];
    float origins[kMaxGroups][3];
    const int ng = Classify(n, infos, group, origins);
    if (ng == 1) return orig(self, n, infos, flags, outs, sampler);   // every stock call
    if (ng == 0) {
        InterlockedIncrement64(&g_faults);
        return orig(self, n, infos, flags, outs, sampler);
    }
    if (ng < 0) {
        InterlockedIncrement64(&g_tooMany);
        return orig(self, n, infos, flags, outs, sampler);
    }
    // Collections on different origins: ILARTS's views at the carrier and DCS's views share this object.
    InterlockedIncrement64(&g_mixed);
    InterlockedIncrement64(&g_t[t].mixed);
    RaiseMax(&g_groupsMax, ng);
    int carrierGroup = -1;
    float carrier[3];
    if (ReadCarrier(carrier))
        for (int g = 0; g < ng; ++g)
            if (SameOrigin(origins[g], carrier)) carrierGroup = g;
    if (carrierGroup == 0)
        InterlockedIncrement64(&g_ilartsFirst);
    else if (carrierGroup > 0)
        InterlockedIncrement64(&g_dcsFirst);
    if (g_seenN.load(std::memory_order_relaxed) < kModelsPerEpisode) NoteModel(t, self, n, group, ng, origins, carrierGroup);
    if (!g_enabled.load(std::memory_order_relaxed)) return orig(self, n, infos, flags, outs, sampler);
    InterlockedIncrement64(&g_splitCalls);
    InterlockedIncrement64(&g_t[t].split);
    SplitCall(orig, self, n, infos, flags, outs, sampler, group, ng);
}

constexpr CollectFn kWrappers[kTargets] = {&CollectWrap<kModel>, &CollectWrap<kRope>};

bool CodeMatches(const unsigned char* base, const CodeBytes* code, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        unsigned char b[8];
        if (!SafeRead(base + code[i].rva, b, code[i].n) || memcmp(b, code[i].b, code[i].n) != 0) return false;
    }
    return true;
}

// rel32 call at `at` (E8) leads to `target`.
bool CallsTo(const unsigned char* base, uintptr_t at, uintptr_t target) {
    unsigned char b[5];
    if (!SafeRead(base + at, b, sizeof(b)) || b[0] != 0xE8) return false;
    int32_t rel;
    memcpy(&rel, b + 1, 4);
    return at + 5 + static_cast<intptr_t>(rel) == target;
}

bool Patch(int t, void** slot, void* cur) {
    g_t[t].orig = reinterpret_cast<CollectFn>(cur);
    if (!PatchSlot(slot, reinterpret_cast<void*>(kWrappers[t]))) return false;
    g_t[t].slot = slot;
    g_t[t].installed = true;
    return true;
}

// GraphSceneObject (Visualizer's vtable, GraphicsVista's function).
std::string InstallModel() {
    HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
    if (!vis) return "Visualizer.dll not loaded";
    auto base = reinterpret_cast<unsigned char*>(vis);
    unsigned char th[sizeof(kThunk)];
    if (!SafeRead(base + kThunkRva, th, sizeof(th)) || memcmp(th, kThunk, sizeof(th)) != 0)
        return "Visualizer+0x1D4EB3 is not the expected import thunk";
    void** slot = reinterpret_cast<void**>(base + kSlotRva);
    void* cur = nullptr;
    if (!SafeRead(slot, &cur, sizeof(cur)) || cur != base + kThunkRva)
        return "Visualizer+0x23BF00 does not hold GraphSceneObject::collectRenderables";
    if (HMODULE gv = GetModuleHandleW(L"GraphicsVista.dll")) {
        // In DCS the import is resolved: it must lead to the function whose code this relies on.
        auto gvBase = reinterpret_cast<unsigned char*>(gv);
        void* fn = reinterpret_cast<void*>(GetProcAddress(gv, kCollectName));
        void* iat = nullptr;
        if (!fn || fn != gvBase + kGvFnRva) return "GraphicsVista does not export collectRenderables at +0xC8C0";
        if (!SafeRead(base + kIatRva, &iat, sizeof(iat)) || iat != fn) return "Visualizer's import does not lead to GraphicsVista+0xC8C0";
        if (!CodeMatches(gvBase, kGvCode, _countof(kGvCode))) return "GraphicsVista+0xC8C0 is not the expected code";
    }
    if (HMODULE ng = GetModuleHandleW(L"NGModel.dll")) g_ngVtable = reinterpret_cast<unsigned char*>(ng) + kNgVtableRva;
    if (!Patch(kModel, slot, cur)) return "could not write Visualizer+0x23BF00";
    return "";
}

// SplineRopeParsable (enlight's own vtable and function).
std::string InstallRope() {
    HMODULE en = GetModuleHandleW(L"enlight.dll");
    if (!en) return "enlight.dll not loaded";
    auto base = reinterpret_cast<unsigned char*>(en);
    if (reinterpret_cast<unsigned char*>(GetProcAddress(en, kRopeVtName)) != base + kRopeVtRva)
        return "enlight does not export the SplineRopeParsable vtable at +0x7FE08";
    void* fn = reinterpret_cast<void*>(GetProcAddress(en, kRopeCollectName));
    if (fn != base + kRopeFnRva) return "enlight does not export SplineRopeParsable::collectRenderables at +0x63B50";
    void** slot = reinterpret_cast<void**>(base + kRopeSlotRva);
    void* cur = nullptr;
    if (!SafeRead(slot, &cur, sizeof(cur)) || cur != fn)
        return "enlight+0x7FE10 does not hold SplineRopeParsable::collectRenderables";
    if (!CodeMatches(base, kRopeCode, _countof(kRopeCode)) || !CallsTo(base, 0x63CFF, 0x63550) ||
        !CallsTo(base, 0x63DDD, 0x63550))
        return "enlight+0x63B50 is not the expected code";
    if (!Patch(kRope, slot, cur)) return "could not write enlight+0x7FE10";
    return "";
}

}  // namespace

std::string OriginSplitInstall() {
    if (g_installed) return "";
    if (EngineDisabled()) return "engine calls disabled";
    if (!CompatOk()) return CompatWhy();
    // Each class on its own: one that does not match leaves the other in place.
    std::string why[kTargets];
    why[kModel] = InstallModel();
    why[kRope] = InstallRope();
    for (int t = 0; t < kTargets; ++t)
        if (g_t[t].installed) g_installed = true;
    if (!g_installed) return "models: " + why[kModel] + "; arresting cables: " + why[kRope];
    for (int t = 0; t < kTargets; ++t) {
        if (g_t[t].installed)
            Log("PLATORIGIN object split installed: %s (%s) -> wrapper; an object call whose collections carry different "
                "origins is split per origin", g_t[t].where, g_t[t].what);
        else
            Log("PLATORIGIN object split NOT installed for %s: %s", g_t[t].what, why[t].c_str());
    }
    return "";
}

void OriginSplitRemove() {
    if (!g_installed) return;
    for (Target& t : g_t) {
        if (!t.installed) continue;
        PatchSlot(t.slot, reinterpret_cast<void*>(t.orig));
        t.slot = nullptr;
        t.installed = false;
    }
    g_installed = false;
    Log("PLATORIGIN object split removed");
}

void OriginSplitEnable(bool on) {
    if (g_enabled.exchange(on) != on) Log("PLATORIGIN object split %s", on ? "ON" : "OFF (counted and logged, not split)");
}

bool OriginSplitEnabled() { return g_enabled.load(std::memory_order_relaxed); }

void OriginSplitFrame(bool carrierOwn, const float carrier[3], double mainToPlatM) {
    const ULONGLONG now = GetTickCount64();
    if (carrierOwn && carrier) {
        for (int i = 0; i < 3; ++i) {
            uint32_t b;
            memcpy(&b, &carrier[i], 4);
            g_carrierBits[i].store(b, std::memory_order_relaxed);
        }
        g_carrierSet.store(1, std::memory_order_release);
        ++s_farFrames;
        if (mainToPlatM >= 0) {
            if (s_distMin < 0 || mainToPlatM < s_distMin) s_distMin = mainToPlatM;
            if (mainToPlatM > s_distMax) s_distMax = mainToPlatM;
        }
        if (!s_wasFar && now - s_episodeTick > kEpisodeGapMs) {
            // a new far-view episode: the first objects get their lines again
            s_episodeTick = now;
            AcquireSRWLockExclusive(&g_seenLock);
            g_seenN.store(0, std::memory_order_relaxed);
            ReleaseSRWLockExclusive(&g_seenLock);
        }
    } else {
        g_carrierSet.store(0, std::memory_order_release);
    }
    s_wasFar = carrierOwn;
    if (now < s_nextStats) return;
    s_nextStats = now + kStatsEveryMs;
    const long long cur[kStatN] = {g_mixed, g_ilartsFirst, g_dcsFirst, g_splitCalls, g_overCap, g_tooMany, g_faults,
                                   g_t[kModel].mixed, g_t[kModel].split, g_t[kRope].mixed, g_t[kRope].split};
    long long d[kStatN];
    bool any = s_farFrames > 0;
    for (int i = 0; i < kStatN; ++i) {
        d[i] = cur[i] - s_last[i];
        s_last[i] = cur[i];
        if (d[i]) any = true;
    }
    if (any) {
        char dist[48] = "?";
        if (s_distMin >= 0) _snprintf_s(dist, _countof(dist), _TRUNCATE, "%.1f..%.1f km", s_distMin / 1000.0, s_distMax / 1000.0);
        Log("PLATORIGIN split %llus: frames with the carrier views on their own origin=%lld | object calls on several origins=%lld "
            "(ILARTS's first=%lld, DCS's first=%lld) split=%lld | models %lld/%lld, arresting cables %lld/%lld (several "
            "origins/split) | over %u collections=%lld more than %d origins=%lld faults=%lld | main camera to PLAT %s | split %s",
            kStatsEveryMs / 1000, s_farFrames, d[0], d[1], d[2], d[3], d[7], d[8], d[9], d[10], kMaxInfos, d[4], kMaxGroups,
            d[5], d[6], dist, g_enabled.load(std::memory_order_relaxed) ? "on" : "OFF");
    }
    s_farFrames = 0;
    s_distMin = s_distMax = -1;
}

std::string OriginSplitStats() {
    char b[400];
    _snprintf_s(b, _countof(b), _TRUNCATE,
                "split installed=%d (models %d, cables %d) enabled=%d mixed=%lld ilarts_first=%lld dcs_first=%lld split=%lld "
                "(models %lld/%lld, cables %lld/%lld) groups_max=%ld over_cap=%lld too_many=%lld faults=%lld",
                g_installed ? 1 : 0, g_t[kModel].installed ? 1 : 0, g_t[kRope].installed ? 1 : 0, g_enabled.load() ? 1 : 0,
                g_mixed, g_ilartsFirst, g_dcsFirst, g_splitCalls, g_t[kModel].mixed, g_t[kModel].split, g_t[kRope].mixed,
                g_t[kRope].split, g_groupsMax, g_overCap, g_tooMany, g_faults);
    return b;
}

#ifdef ORIGIN_SPLIT_TEST
void OriginSplitTestSet(void* original, int target) { g_t[target].orig = reinterpret_cast<CollectFn>(original); }
void* OriginSplitTestWrap(int target) { return reinterpret_cast<void*>(kWrappers[target]); }
void OriginSplitTestStatsNow() { s_nextStats = 0; }
#endif

}  // namespace lsodiag
