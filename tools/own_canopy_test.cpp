// Offline test of src/own_canopy.cpp (no DCS):
//  1. the parse / per-LOD fetch wrappers, driven with stand-ins for NGModel's ParseMT2 and its fetch function;
//  2. the diagnostics: anomaly lines, split target lines, stats lines that go on after the first 60, the reset
//     at each mission;
//  3. with a DCS install given, installing and removing the two patches on the real NGModel.dll (twice, as
//     for a mission restart), mapped with DONT_RESOLVE_DLL_REFERENCES (no DCS code runs).
//
//   own_canopy_test.exe ["C:\Program Files\Eagle Dynamics\DCS World"]
#include "../src/own_canopy.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::vector<std::string> g_lines;   // every Log line, for the checks
}

namespace lsodiag {
void Log(const char* format, ...) {
    char b[4096];
    va_list ap;
    va_start(ap, format);
    vsnprintf(b, sizeof(b), format, ap);
    va_end(ap);
    g_lines.push_back(b);
    printf("  log: %s\n", b);
}
bool SafeRead(const void* address, void* out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &got) && got == size;
}
bool CompatOk() { return true; }
std::string CompatWhy() { return {}; }
bool EngineDisabled() { return false; }
}  // namespace lsodiag

using namespace lsodiag;

namespace {

int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("  FAIL line %d: %s\n", __LINE__, #c);                \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

// Log lines containing `what` since index `from`.
int Lines(const char* what, size_t from = 0) {
    int n = 0;
    for (size_t i = from; i < g_lines.size(); ++i)
        if (g_lines[i].find(what) != std::string::npos) ++n;
    return n;
}

struct FloatVec {
    const float* begin;
    const float* end;
    const float* cap;
};

// Stand-in model instance: vtable slot 7 -> model params, whose slot 1 -> the draw arguments.
struct Params {
    void** vt;
    FloatVec vec;
    unsigned char* cookie;
};
struct Inst {
    void** vt;
    Params* params;
};
void* GetParams(void* inst) { return static_cast<Inst*>(inst)->params; }
const FloatVec* GetArgs(void* params) { return &static_cast<Params*>(params)->vec; }
void* GetCookie(void* params) { return static_cast<Params*>(params)->cookie; }
void* g_instVt[8] = {};
void* g_paramsVt[4] = {};

using FetchFn = void* (*)(void* slots, int64_t index, const void* args, const void* lodRoot, uint64_t flag);
using ParseFn = void (*)(void* self, void* inst, unsigned n, const void* const* infos, const bool* flags, void** outs,
                         void* sampler, const void* pos);

struct Fetch {
    int64_t index;
    float a114;
    const void* args;
};
struct Call {
    std::vector<const void*> infos;
    std::vector<bool> flags;
    std::vector<void*> outs;
    std::vector<Fetch> fetches;
    const void* pos;
};
std::vector<Call> g_calls;
int g_lods = 6;
int g_extraIndex = -1;   // >= 0: the stand-in parse also fetches this slot index (an LOD beyond the count)
constexpr size_t kSlotSize = 0xF18;
// The graphics instance: 0x18 bytes, then 16 per-LOD slots of 0xF18 (+0x00 = the LOD root bound on first fetch).
unsigned char g_cookie[0x18 + 16 * kSlotSize];
unsigned char g_cookie2[0x18 + 16 * kSlotSize];
unsigned char g_otherSlots[16 * kSlotSize];
const void* LodRoot(int l) { return reinterpret_cast<const void*>(static_cast<uintptr_t>(0x100 + l)); }
void ClearSlots(unsigned char* cookie) { memset(cookie, 0, 0x18 + 16 * kSlotSize); }

void* FakeFetch(void* slots, int64_t index, const void* args, const void* lodRoot, uint64_t) {
    Fetch f{index, static_cast<const FloatVec*>(args)->begin[114], args};
    if (!g_calls.empty()) g_calls.back().fetches.push_back(f);
    if (index < 0 || index >= 16) return nullptr;   // (NGModel would write outside the cookie)
    auto slot = reinterpret_cast<const void**>(static_cast<unsigned char*>(slots) + index * kSlotSize);
    if (!*slot) *slot = lodRoot;   // as NGModel: a slot keeps the root of its first fetch
    return static_cast<unsigned char*>(slots) + index;
}

// Stand-in ParseMT2: records the call and fetches every LOD's instance through the (redirected) fetch.
void FakeParse(void* self, void* inst, unsigned n, const void* const* infos, const bool* flags, void** outs, void*, const void* pos) {
    (void)self;
    Call c;
    for (unsigned i = 0; i < n; ++i) {
        c.infos.push_back(infos[i]);
        c.flags.push_back(flags[i]);
        c.outs.push_back(outs[i]);
    }
    c.pos = pos;
    g_calls.push_back(c);
    const FloatVec* args = GetArgs(GetParams(inst));
    auto fetch = reinterpret_cast<FetchFn>(OwnCanopyTestFetchWrap());
    unsigned char* cookie = static_cast<Inst*>(inst)->params->cookie;
    for (int l = 0; l < g_lods; ++l) fetch(cookie + 0x18, l, args, LodRoot(l), 0);
    if (g_extraIndex >= 0) fetch(cookie + 0x18, g_extraIndex, args, LodRoot(g_extraIndex), 0);
    // Another object's fetch on the same thread (as if nested) is never redirected.
    CHECK(fetch(g_otherSlots, 0, args, LodRoot(0), 0) == g_otherSlots);
    CHECK(g_calls.back().fetches.back().a114 == args->begin[114]);
    g_calls.back().fetches.pop_back();
}

struct Coll {
    unsigned char bytes[0x6D8] = {};
    void SetCamera(double x, double y, double z) {
        const double c[3] = {x, y, z};
        memcpy(bytes + 0x318, c, sizeof(c));
    }
};

void TestWrappers() {
    printf("wrappers:\n");
    g_instVt[7] = reinterpret_cast<void*>(&GetParams);
    g_paramsVt[1] = reinterpret_cast<void*>(&GetArgs);
    g_paramsVt[3] = reinterpret_cast<void*>(&GetCookie);
    std::vector<float> args(600, 0.25f);
    Params params{g_paramsVt, {args.data(), args.data() + args.size(), args.data() + args.size()}, g_cookie};
    Inst inst{g_instVt, &params};
    unsigned char model[0x600] = {};
    // NGModel's LOD table: entries fill from 0 upward, +0x5A0 = how many (qword), +0x5B0 = first one drawn.
    auto setLods = [&](int lods, uint32_t first = 0) {
        g_lods = lods;
        const uint64_t count = lods + first;
        memcpy(model + 0x5A0, &count, 8);
        memcpy(model + 0x5B0, &first, 4);
        ClearSlots(g_cookie);
    };
    setLods(6);
    OwnCanopyTestSet(reinterpret_cast<void*>(&FakeParse), reinterpret_cast<void*>(&FakeFetch));
    auto parse = reinterpret_cast<ParseFn>(OwnCanopyTestParseWrap());

    // Collections: 0, 1 = your view (cockpit camera), 2 = PLAT, 3 = a shadow pass, 4 = island camera.
    Coll coll[5];
    coll[0].SetCamera(1000.0, 30.0, 2000.0);
    coll[1].SetCamera(1000.0, 30.0, 2000.0);
    coll[2].SetCamera(1100.5, 20.5, 2050.25);
    coll[3].SetCamera(900.0, 900.0, 900.0);
    coll[4].SetCamera(1060.0, 38.0, 2020.0);
    const void* infos[5];
    bool flags[5] = {true, false, true, false, true};
    void* outs[5];
    for (int i = 0; i < 5; ++i) infos[i] = &coll[i], outs[i] = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + i));
    // The parse's position (row 3 = object relative to the frame origin) and, in the first collection, your
    // camera relative to the same origin: 6 m apart (your aircraft). Model name as NGModel keeps it.
    float pos[16] = {};
    pos[12] = 10.0f, pos[13] = 2.0f, pos[14] = -4.0f;
    const float mainRel[3] = {16.0f, 2.0f, -4.0f};
    for (Coll& c : coll) memcpy(c.bytes + 0x500, mainRel, sizeof(mainRel));
    memcpy(model + 0x10, "fa-18c", 7);
    const uint64_t cap = 15;
    memcpy(model + 0x28, &cap, 8);

    // Disabled: one plain call.
    g_calls.clear();
    args[114] = 1.0f;
    parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 1 && g_calls[0].infos.size() == 5);
    for (const Fetch& f : g_calls[0].fetches) CHECK(f.index < 6 && f.a114 == 1.0f);

    OwnCanopyEnable(true);
    // Enabled, no ILARTS view known yet: one plain call.
    g_calls.clear();
    parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 1 && g_calls[0].infos.size() == 5);

    // The views of this build (PLAT and island camera), published at the end of the build.
    const double plat[3] = {1100.5, 20.5, 2050.25}, island[3] = {1060.0, 38.0, 2020.0};
    OwnCanopyNoteView(plat);
    OwnCanopyNoteView(island);
    OwnCanopyNoteView(plat);   // a view set up twice counts once
    OwnCanopyBuildEnd(nullptr);

    // Your aircraft (114 = 1), seen by DCS's views and ILARTS's: two calls; ILARTS's last (collection 4 is last).
    OwnCanopyTestSampleNow();   // and its OWNCANOPY near lines
    size_t mark = g_lines.size();
    g_calls.clear();
    parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 2);
    if (g_calls.size() == 2) {
        const Call& dcs = g_calls[0];
        const Call& ours = g_calls[1];
        CHECK(dcs.infos.size() == 3 && dcs.infos[0] == infos[0] && dcs.infos[1] == infos[1] && dcs.infos[2] == infos[3]);
        CHECK(dcs.flags[0] == true && dcs.flags[1] == false && dcs.flags[2] == false);
        CHECK(dcs.outs[2] == outs[3]);
        CHECK(ours.infos.size() == 2 && ours.infos[0] == infos[2] && ours.infos[1] == infos[4]);
        CHECK(ours.flags[0] == true && ours.flags[1] == true && ours.outs[0] == outs[2] && ours.outs[1] == outs[4]);
        CHECK(dcs.pos == pos && ours.pos == pos);
        for (const Fetch& f : dcs.fetches) CHECK(f.index >= 0 && f.index < 6 && f.a114 == 1.0f && f.args != nullptr);
        for (size_t l = 0; l < ours.fetches.size(); ++l)
            CHECK(ours.fetches[l].index == static_cast<int64_t>(l) + 6 && ours.fetches[l].a114 == 0.0f);
        CHECK(ours.fetches.size() == 6);
    }
    CHECK(args[114] == 1.0f);   // DCS's own arguments are never written
    CHECK(Lines("first parse of your aircraft", mark) == 1);
    CHECK(Lines("OWNCANOPY split target: model 'fa-18c'", mark) == 1);   // the object the split runs for
    CHECK(Lines("OWNCANOPY near: 'fa-18c'", mark) == 1);
    CHECK(Lines("anomaly", mark) == 0);

    // More than 64 collections (the island and a deck camera on): still split.
    std::vector<Coll> many(100);
    std::vector<const void*> manyInfos(100);
    bool manyFlags[100] = {};
    std::vector<void*> manyOuts(100, nullptr);
    for (int i = 0; i < 100; ++i) {
        many[i].SetCamera(1000.0, 30.0, 2000.0 + i);
        memcpy(many[i].bytes + 0x500, mainRel, sizeof(mainRel));
        manyInfos[i] = &many[i];
    }
    many[70].SetCamera(1100.5, 20.5, 2050.25);
    many[71].SetCamera(1060.0, 38.0, 2020.0);
    g_calls.clear();
    parse(model, &inst, 100, manyInfos.data(), manyFlags, manyOuts.data(), nullptr, pos);
    CHECK(g_calls.size() == 2 && g_calls[0].infos.size() == 2 && g_calls[0].infos[0] == &many[70] && g_calls[1].infos.size() == 98);
    if (g_calls.size() == 2) CHECK(g_calls[0].fetches[0].index == 6 && g_calls[1].fetches[0].index == 0);

    // Last collection one of DCS's: ILARTS's call first.
    const void* infos2[4] = {infos[2], infos[0], infos[4], infos[3]};
    g_calls.clear();
    parse(model, &inst, 4, infos2, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 2 && g_calls[0].infos.size() == 2 && g_calls[0].infos[0] == infos[2] && g_calls[1].infos.size() == 2);
    if (g_calls.size() == 2) CHECK(g_calls[0].fetches[0].index == 6 && g_calls[1].fetches[0].index == 0);

    // Only ILARTS's views: one call, spare slots.
    const void* infos3[2] = {infos[2], infos[4]};
    g_calls.clear();
    parse(model, &inst, 2, infos3, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 1 && g_calls[0].fetches.size() == 6 && g_calls[0].fetches[5].index == 11 && g_calls[0].fetches[5].a114 == 0.0f);

    // Any other object (114 = 0, its own graphics instance): one plain call.
    args[114] = 0.0f;
    params.cookie = g_cookie2;
    g_calls.clear();
    parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 1 && g_calls[0].infos.size() == 5 && g_calls[0].fetches[0].index == 0);
    params.cookie = g_cookie;
    args[114] = 1.0f;

    // A model with 30 arguments (most weapons and pods): plain, argument 114 never read or written.
    params.vec.end = args.data() + 30;
    g_calls.clear();
    parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 1 && g_calls[0].infos.size() == 5);
    params.vec.end = args.data() + args.size();

    // The F/A-18C as loaded (run 2026-11-19 21:06): 6 LOD files plus the collision shell sorted first = 7 entries
    // from 1. DCS's slots 0..5, ILARTS's 6..11.
    setLods(6, 1);
    g_calls.clear();
    parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 2 && g_calls[1].fetches.size() == 6 && g_calls[1].fetches[0].index == 6 && g_calls[1].fetches[5].index == 11);
    // A model whose first entry is 1 with 5 LODs uses slots 0..4, ILARTS's 5..9.
    setLods(5, 1);
    g_calls.clear();
    parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 2 && g_calls[1].fetches.size() == 5 && g_calls[1].fetches[0].index == 5 && g_calls[1].fetches[4].index == 9);
    setLods(6);

    // A spare slot already bound to another LOD root: that fetch is not redirected (and gets DCS's arguments);
    // one anomaly line.
    {
        auto bound = reinterpret_cast<const void**>(g_cookie + 0x18 + 8 * kSlotSize);
        *bound = LodRoot(7);
        mark = g_lines.size();
        g_calls.clear();
        parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
        CHECK(g_calls.size() == 2);
        if (g_calls.size() == 2) {
            const Call& ours = g_calls[1];
            CHECK(ours.fetches.size() == 6 && ours.fetches[2].index == 2 && ours.fetches[2].a114 == 1.0f);
            CHECK(ours.fetches[1].index == 7 && ours.fetches[1].a114 == 0.0f && ours.fetches[3].index == 9);
        }
        CHECK(Lines("anomaly: spare slot 8", mark) == 1);
        ClearSlots(g_cookie);
    }

    // ILARTS's parse fetching an LOD at or beyond the count (never expected): left to DCS, one anomaly line; and
    // as DCS's slot 6 is ILARTS's spare slot for LOD 0, the fetch probe reports that binding too.
    {
        g_extraIndex = 6;
        mark = g_lines.size();
        g_calls.clear();
        parse(model, &inst, 2, infos3, flags, outs, nullptr, pos);
        CHECK(g_calls.size() == 1);
        if (g_calls.size() == 1) {
            CHECK(g_calls[0].fetches.size() == 7 && g_calls[0].fetches[0].index == 6 && g_calls[0].fetches[0].a114 == 0.0f);
            CHECK(g_calls[0].fetches[6].index == 6 && g_calls[0].fetches[6].a114 == 1.0f);
        }
        CHECK(Lines("anomaly: ILARTS's parse fetched slot 6", mark) == 1);
        CHECK(Lines("anomaly: DCS's per-LOD slot 6", mark) == 1);
        g_extraIndex = -1;
        ClearSlots(g_cookie);
    }

    // A second instance with 114 = 0 on the graphics instance your aircraft just used (the 15:25 run's theory;
    // a recycled cookie address looks the same): NOT split any more, one anomaly line. One with its own
    // graphics instance: plain, no line.
    {
        std::vector<float> args2(600, 0.25f);
        args2[114] = 0.0f;
        Params params2{g_paramsVt, {args2.data(), args2.data() + args2.size(), args2.data() + args2.size()}, g_cookie};
        Inst inst2{g_instVt, &params2};
        parse(model, &inst, 5, infos, flags, outs, nullptr, pos);   // the 114 = 1 instance marks the graphics instance
        mark = g_lines.size();
        g_calls.clear();
        parse(model, &inst2, 5, infos, flags, outs, nullptr, pos);
        CHECK(g_calls.size() == 1);
        if (g_calls.size() == 1)
            for (const Fetch& f : g_calls[0].fetches) CHECK(f.index < 6 && f.a114 == 0.0f);
        CHECK(Lines("anomaly: graphics instance", mark) == 1);
        CHECK(args2[114] == 0.0f);
        // Your own instance with 114 = 0 (an outside view): plain, no anomaly.
        args[114] = 0.0f;
        mark = g_lines.size();
        g_calls.clear();
        parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
        CHECK(g_calls.size() == 1 && Lines("anomaly", mark) == 0);
        args[114] = 1.0f;
        params2.cookie = g_cookie2;
        mark = g_lines.size();
        g_calls.clear();
        parse(model, &inst2, 5, infos, flags, outs, nullptr, pos);
        CHECK(g_calls.size() == 1 && g_calls[0].fetches[0].index == 0);
        CHECK(Lines("anomaly", mark) == 0);
        ClearSlots(g_cookie);
    }

    // Another object with 114 set (your new aircraft after a respawn, or never expected another one): a split
    // target line, and one more when it goes back.
    {
        std::vector<float> args3(600, 0.0f);
        args3[114] = 1.0f;
        Params params3{g_paramsVt, {args3.data(), args3.data() + args3.size(), args3.data() + args3.size()}, g_cookie2};
        Inst inst3{g_instVt, &params3};
        mark = g_lines.size();
        g_calls.clear();
        parse(model, &inst3, 5, infos, flags, outs, nullptr, pos);
        CHECK(g_calls.size() == 2);
        if (g_calls.size() == 2) CHECK(g_calls[1].fetches[0].index == 6);
        parse(model, &inst3, 5, infos, flags, outs, nullptr, pos);
        CHECK(Lines("split target", mark) == 1 && Lines("before: instance", mark) == 1);
        parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
        CHECK(Lines("split target", mark) == 2);
        ClearSlots(g_cookie2);
        ClearSlots(g_cookie);
    }

    // A model with more than 8 LODs (no spare slots): plain. 8 LODs: ILARTS's slots end at 15.
    setLods(9);
    g_calls.clear();
    parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 1 && g_calls[0].fetches[8].index == 8);
    setLods(8);
    g_calls.clear();
    parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 2 && g_calls[1].fetches[7].index == 15);
    setLods(6);

    // A fault reading the model (LOD table): that parse left to DCS (one plain call), an anomaly line.
    mark = g_lines.size();
    g_calls.clear();
    parse(reinterpret_cast<void*>(static_cast<uintptr_t>(0x10)), &inst, 5, infos, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 1 && g_calls[0].infos.size() == 5);
    CHECK(Lines("anomaly: fault reading the model's LOD table", mark) == 1);

    // Next build without ILARTS views: plain again.
    OwnCanopyBuildEnd(nullptr);
    g_calls.clear();
    parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 1);

    // Off again: plain.
    OwnCanopyNoteView(plat);
    OwnCanopyBuildEnd(nullptr);
    OwnCanopyEnable(false);
    g_calls.clear();
    parse(model, &inst, 5, infos, flags, outs, nullptr, pos);
    CHECK(g_calls.size() == 1);
    printf("  stats: %s\n", OwnCanopyStats().c_str());

    // The fetch probe on DCS's own fetches (no ILARTS parse running): never changed, unexpected ones logged.
    {
        OwnCanopyTestReset();   // clean counters and line budgets
        auto fetch = reinterpret_cast<FetchFn>(OwnCanopyTestFetchWrap());
        const FloatVec* a = GetArgs(&params);
        unsigned char* slots = g_cookie + 0x18;
        mark = g_lines.size();
        g_calls.assign(1, Call{});
        fetch(slots, 0, a, LodRoot(0), 0);   // binds slot 0
        fetch(slots, 0, a, LodRoot(0), 0);   // the same LOD again: fine
        CHECK(Lines("anomaly", mark) == 0);
        fetch(slots, 0, a, LodRoot(3), 0);   // slot 0 bound to another LOD (DCS's allResourcesAreReady binds entry 0)
        CHECK(Lines("anomaly: DCS's per-LOD slot 0", mark) == 1);
        fetch(slots, -1, a, LodRoot(0), 0);  // DCS fetching slot -1 (a forced LOD below the first): outside the cookie
        fetch(slots, 16, a, LodRoot(0), 0);
        CHECK(Lines("outside the cookie's 16 slots", mark) == 2);
        CHECK(g_calls[0].fetches.size() == 5);
        for (const Fetch& f : g_calls[0].fetches) CHECK(f.args == a);   // DCS's arguments, DCS's index
        CHECK(g_calls[0].fetches[2].index == 0 && g_calls[0].fetches[3].index == -1 && g_calls[0].fetches[4].index == 16);
        // Each kind has its own budget: 8 lines, then only counted.
        for (int i = 0; i < 20; ++i) fetch(slots, -2, a, LodRoot(0), 0);
        CHECK(Lines("outside the cookie's 16 slots", mark) == 8);
        CHECK(OwnCanopyStats().find("dcs_slot_out_of_range=22") != std::string::npos);
        CHECK(OwnCanopyStats().find("dcs_slot_other_root=1") != std::string::npos);
        ClearSlots(g_cookie);
    }
}

// The stats lines go on after the first 60 (every 5 minutes, counting over that time), and a new mission
// starts with fresh first-time lines and budgets.
void TestDiagnostics() {
    printf("diagnostics:\n");
    OwnCanopyTestInstalled(true);
    OwnCanopyTestReset();
    const size_t mark = g_lines.size();
    for (int i = 0; i < 64; ++i) {
        OwnCanopyTestStatsNow();
        OwnCanopyBuildEnd(nullptr);
    }
    CHECK(Lines("OWNCANOPY stats 15s:", mark) == 60);
    CHECK(Lines("OWNCANOPY stats 300s:", mark) == 4);
    CHECK(Lines("from now on one line every 300 s", mark) == 1);
    CHECK(Lines("fetches=", mark) == 64 && Lines("dcs_slot_out_of_range=", mark) == 64 && Lines("not_redirected=", mark) == 64);
    // The counters accumulate between lines: the anomaly counts of TestWrappers were cleared by the reset.
    CHECK(Lines("dcs_slot_out_of_range=0", mark) == 64);
    OwnCanopyTestInstalled(false);
}

void TestPatches(const std::wstring& dcs) {
    printf("patches on NGModel.dll:\n");
    const std::wstring path = dcs + L"\\bin\\NGModel.dll";
    HMODULE ng = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!ng) {
        printf("  skipped: cannot map %ls (%lu)\n", path.c_str(), GetLastError());
        return;
    }
    auto base = reinterpret_cast<unsigned char*>(ng);
    unsigned char before[16], vt[8];
    memcpy(before, base + 0x3DBE0, 16);
    memcpy(vt, base + 0x5A100, 8);
    // Twice: a mission, then the next one after a restart (install after remove).
    for (int mission = 0; mission < 2; ++mission) {
        const size_t mark = g_lines.size();
        const std::string why = OwnCanopyInstall();
        printf("  install %d: %s\n", mission + 1, why.empty() ? "ok" : why.c_str());
        CHECK(why.empty());
        if (!why.empty()) break;
        void* slot = nullptr;
        memcpy(&slot, base + 0x5A100, 8);
        CHECK(slot == OwnCanopyTestParseWrap());
        CHECK(base[0x3DBE4] == 0xE8);
        int32_t rel = 0;
        memcpy(&rel, base + 0x3DBE5, 4);
        const unsigned char* stub = base + 0x3DBE9 + rel;
        uint64_t target = 0;
        memcpy(&target, stub + 6, 8);
        CHECK(stub[0] == 0xFF && stub[1] == 0x25 && target == reinterpret_cast<uint64_t>(OwnCanopyTestFetchWrap()));
        CHECK(memcmp(base + 0x3DBE0, before, 4) == 0 && memcmp(base + 0x3DBE9, before + 9, 7) == 0);
        OwnCanopyBuildEnd(nullptr);   // first build of the mission: the stats clock starts
        OwnCanopyTestStatsNow();      // the periodic stats line, now
        OwnCanopyBuildEnd(nullptr);
        CHECK(Lines("OWNCANOPY stats 15s:", mark) == 1);   // each mission starts with its own 15 s lines
        OwnCanopyRemove();
        CHECK(memcmp(base + 0x3DBE0, before, 16) == 0);
        CHECK(memcmp(base + 0x5A100, vt, 8) == 0);
        CHECK(OwnCanopyStats().find("installed=0 enabled=0 ilarts_views=0") != std::string::npos);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    TestWrappers();
    TestDiagnostics();
    if (argc > 1) TestPatches(argv[1]);
    printf(g_fail ? "FAILED (%d)\n" : "ALL OK\n", g_fail);
    return g_fail ? 1 : 0;
}
