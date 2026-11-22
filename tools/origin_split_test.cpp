// Offline test of src/origin_split.cpp (no DCS):
//  1. the per-object wrapper, driven with a stand-in for GraphicsVista's GraphSceneObject::collectRenderables and
//     fake CollectionInfo blocks (origin float3 at +0x50C); the same for enlight's SplineRopeParsable::collectRenderables (arresting cables), its own wrapper and original;
//  2. with a DCS install given, installing and removing the vtable patches on the real Visualizer.dll and
//     enlight.dll, mapped with DONT_RESOLVE_DLL_REFERENCES (no DCS code runs); GraphicsVista.dll is mapped too and
//     its export written into Visualizer's import slot, as the loader would, so the install's code checks run as in
//     DCS. Each class installs on its own (enlight missing or not matching leaves the models' patch in place, and
//     the other way round).
//
//   origin_split_test.exe ["C:\Program Files\Eagle Dynamics\DCS World"]
#include "../src/origin_split.h"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::vector<std::string> g_logs;
}

namespace lsodiag {
void Log(const char* format, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    printf("  log: %s\n", line);
    g_logs.push_back(line);
}
bool SafeRead(const void* address, void* out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &got) && got == size;
}
bool PatchSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    return true;
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

using CollectFn = void (*)(void* self, unsigned n, const void* const* infos, const bool* flags, void** outs,
                           void* sampler);

struct Call {
    void* self;
    std::vector<const void*> infos;
    std::vector<int> flags;   // -1: flags pointer was null
    std::vector<void*> outs;
    void* sampler;
    const void* const* infosPtr;
};
std::vector<Call> g_calls;

void Original(void* self, unsigned n, const void* const* infos, const bool* flags, void** outs, void* sampler) {
    Call c{self, {}, {}, {}, sampler, infos};
    for (unsigned i = 0; i < n; ++i) {
        c.infos.push_back(infos[i]);
        c.flags.push_back(flags ? (flags[i] ? 1 : 0) : -1);
        c.outs.push_back(outs[i]);
    }
    g_calls.push_back(c);
}

// The cable's original: recorded separately, so a wrapper calling the other class's original shows.
std::vector<Call> g_ropeCalls;
void OriginalRope(void* self, unsigned n, const void* const* infos, const bool* flags, void** outs, void* sampler) {
    Call c{self, {}, {}, {}, sampler, infos};
    for (unsigned i = 0; i < n; ++i) {
        c.infos.push_back(infos[i]);
        c.flags.push_back(flags ? (flags[i] ? 1 : 0) : -1);
        c.outs.push_back(outs[i]);
    }
    g_ropeCalls.push_back(c);
}

constexpr size_t kInfoSize = 0x6D8;
struct Info {
    alignas(16) unsigned char b[kInfoSize];
};
void SetOrigin(Info& in, float x, float y, float z) {
    memset(in.b, 0, sizeof(in.b));
    const float o[3] = {x, y, z};
    memcpy(in.b + 0x50C, o, 12);
}

// A fake GraphSceneObject: the ISceneParsable part at +0x48, the Model* at -0x30 from it.
struct FakeObject {
    unsigned char b[0x100];
};

size_t CountLogs(const char* needle) {
    size_t n = 0;
    for (const std::string& s : g_logs)
        if (s.find(needle) != std::string::npos) ++n;
    return n;
}

void TestWrapper() {
    printf("wrapper:\n");
    OriginSplitTestSet(reinterpret_cast<void*>(&Original));
    auto wrap = reinterpret_cast<CollectFn>(OriginSplitTestWrap());
    const float carrier[3] = {-88448.0f, 0.0f, 176448.0f};   // the views at the carrier (2026-11-19 21:31:46)
    const float frame[3] = {-18816.0f, 7808.0f, 178048.0f};  // the frame origin next to the player
    std::vector<Info> pool(300);
    FakeObject obj{};
    void* model = reinterpret_cast<void*>(0x1234);
    memcpy(obj.b + 0x48 - 0x30, &model, 8);
    void* self = obj.b + 0x48;
    void* sampler = reinterpret_cast<void*>(0x5A5A);
    const void* infos[300];
    bool flags[300];
    void* outs[300];
    for (int i = 0; i < 300; ++i) {
        infos[i] = &pool[i];
        flags[i] = (i % 3) == 0;
        outs[i] = reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000 + i));
    }
    auto at = [&](int i, const float* o) { SetOrigin(pool[i], o[0], o[1], o[2]); };

    // Frame without the far view: the stock case, one origin.
    OriginSplitFrame(false, nullptr, 20000.0);
    for (int i = 0; i < 5; ++i) at(i, frame);
    g_calls.clear();
    wrap(self, 5, infos, flags, outs, sampler);
    CHECK(g_calls.size() == 1);
    CHECK(g_calls.size() == 1 && g_calls[0].infosPtr == infos && g_calls[0].infos.size() == 5);
    CHECK(g_calls.size() == 1 && g_calls[0].self == self && g_calls[0].sampler == sampler);

    // n = 1 and n = 0: straight through.
    g_calls.clear();
    wrap(self, 1, infos, flags, outs, sampler);
    wrap(self, 0, infos, flags, outs, sampler);
    CHECK(g_calls.size() == 2 && g_calls[0].infosPtr == infos);

    // Far view: ILARTS's collection first, last collection DCS's -> carrier group, then DCS's.
    OriginSplitFrame(true, carrier, 72000.0);
    at(0, carrier), at(1, frame), at(2, frame), at(3, carrier), at(4, frame);
    g_calls.clear();
    g_logs.clear();
    wrap(self, 5, infos, flags, outs, sampler);
    CHECK(g_calls.size() == 2);
    if (g_calls.size() == 2) {
        CHECK(g_calls[0].infos == (std::vector<const void*>{infos[0], infos[3]}));
        CHECK(g_calls[0].flags == (std::vector<int>{1, 1}));
        CHECK(g_calls[0].outs == (std::vector<void*>{outs[0], outs[3]}));
        CHECK(g_calls[1].infos == (std::vector<const void*>{infos[1], infos[2], infos[4]}));
        CHECK(g_calls[1].flags == (std::vector<int>{0, 0, 0}));
        CHECK(g_calls[1].outs == (std::vector<void*>{outs[1], outs[2], outs[4]}));
        CHECK(g_calls[0].self == self && g_calls[1].self == self && g_calls[1].sampler == sampler);
    }
    CHECK(CountLogs("PLATORIGIN split: model") == 1);
    CHECK(CountLogs("the first was ILARTS's") == 1);
    CHECK(CountLogs("70.1 km from where it is, next to your camera") == 1);

    // Same model again in this episode: no second line.
    g_calls.clear();
    wrap(self, 5, infos, flags, outs, sampler);
    CHECK(g_calls.size() == 2);
    CHECK(CountLogs("PLATORIGIN split: model") == 1);

    // DCS's collection first, last collection ILARTS's -> DCS's group first, the carrier group last.
    FakeObject obj2{};
    void* model2 = reinterpret_cast<void*>(0x5678);
    memcpy(obj2.b + 0x48 - 0x30, &model2, 8);
    at(0, frame), at(1, carrier), at(2, carrier), at(3, frame), at(4, carrier);
    g_calls.clear();
    wrap(obj2.b + 0x48, 5, infos, flags, outs, sampler);
    CHECK(g_calls.size() == 2);
    if (g_calls.size() == 2) {
        CHECK(g_calls[0].infos == (std::vector<const void*>{infos[0], infos[3]}));
        CHECK(g_calls[1].infos == (std::vector<const void*>{infos[1], infos[2], infos[4]}));
    }
    CHECK(CountLogs("the first was DCS's") == 1);

    // The first collection's group also holds the last collection: the other group runs first.
    at(0, carrier), at(1, frame), at(2, carrier);
    g_calls.clear();
    wrap(self, 3, infos, flags, outs, sampler);
    CHECK(g_calls.size() == 2);
    if (g_calls.size() == 2) {
        CHECK(g_calls[0].infos == (std::vector<const void*>{infos[1]}));
        CHECK(g_calls[1].infos == (std::vector<const void*>{infos[0], infos[2]}));
        CHECK(g_calls[1].outs == (std::vector<void*>{outs[0], outs[2]}));
    }

    // Three origins (a second carrier view origin, hypothetically): three calls, last group last.
    const float third[3] = {1.0f, 2.0f, 3.0f};
    at(0, third), at(1, carrier), at(2, frame), at(3, third), at(4, carrier);
    g_calls.clear();
    wrap(self, 5, infos, flags, outs, sampler);
    CHECK(g_calls.size() == 3);
    if (g_calls.size() == 3) {
        CHECK(g_calls[0].infos == (std::vector<const void*>{infos[0], infos[3]}));
        CHECK(g_calls[1].infos == (std::vector<const void*>{infos[2]}));
        CHECK(g_calls[2].infos == (std::vector<const void*>{infos[1], infos[4]}));
    }

    // Null flags: the sub-calls get null too.
    at(0, carrier), at(1, frame);
    g_calls.clear();
    wrap(self, 2, infos, nullptr, outs, sampler);
    CHECK(g_calls.size() == 2 && g_calls[0].flags == (std::vector<int>{-1}) && g_calls[1].flags == (std::vector<int>{-1}));

    // More than four origins: one plain call.
    const float o5[5][3] = {{1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0}, {5, 0, 0}};
    for (int i = 0; i < 5; ++i) at(i, o5[i]);
    g_calls.clear();
    wrap(self, 5, infos, flags, outs, sampler);
    CHECK(g_calls.size() == 1 && g_calls[0].infosPtr == infos);

    // More than 256 collections, mixed: one plain call (counted).
    for (int i = 0; i < 300; ++i) at(i, i == 299 ? carrier : frame);
    g_calls.clear();
    wrap(self, 300, infos, flags, outs, sampler);
    CHECK(g_calls.size() == 1 && g_calls[0].infos.size() == 300);

    // An unreadable collection: one plain call (counted as a fault), no crash.
    void* reserved = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    const void* bad[3] = {infos[0], reserved, infos[2]};
    at(0, carrier), at(2, frame);
    g_calls.clear();
    wrap(self, 3, bad, flags, outs, sampler);
    CHECK(g_calls.size() == 1 && g_calls[0].infosPtr == bad);
    VirtualFree(reserved, 0, MEM_RELEASE);

    // Split off (A/B): counted and passed through unsplit.
    OriginSplitEnable(false);
    CHECK(!OriginSplitEnabled());
    at(0, carrier), at(1, frame), at(2, frame);
    g_calls.clear();
    wrap(self, 3, infos, flags, outs, sampler);
    CHECK(g_calls.size() == 1 && g_calls[0].infosPtr == infos);
    OriginSplitEnable(true);

    // The carrier views' origin unknown this frame (far view off): still split by origin.
    OriginSplitFrame(false, nullptr, -1.0);
    g_calls.clear();
    wrap(self, 3, infos, flags, outs, sampler);
    CHECK(g_calls.size() == 2);

    // The periodic line.
    g_logs.clear();
    OriginSplitTestStatsNow();
    OriginSplitFrame(true, carrier, 74000.0);
    CHECK(CountLogs("PLATORIGIN split 30s:") == 1);
    CHECK(CountLogs("object calls on several origins=") == 1);
    CHECK(CountLogs("arresting cables 0/0") == 1);

    // The arresting cable (SplineRopeParsable): its own wrapper and original.
    OriginSplitTestSet(reinterpret_cast<void*>(&OriginalRope), 1);
    auto rope = reinterpret_cast<CollectFn>(OriginSplitTestWrap(1));
    CHECK(reinterpret_cast<void*>(rope) != reinterpret_cast<void*>(wrap));
    unsigned char cable[0x40] = {};   // no Model* read for a cable: any object will do
    // Stock: one origin, straight through to the cable's original with DCS's own arrays.
    for (int i = 0; i < 4; ++i) at(i, frame);
    g_calls.clear();
    g_ropeCalls.clear();
    rope(cable, 4, infos, flags, outs, sampler);
    CHECK(g_calls.empty());
    CHECK(g_ropeCalls.size() == 1 && g_ropeCalls[0].infosPtr == infos && g_ropeCalls[0].self == cable);
    rope(cable, 1, infos, flags, outs, sampler);
    CHECK(g_ropeCalls.size() == 2 && g_calls.empty());
    // A trap seen from 40 nm with the PLAT looking at it: ILARTS's collection first, DCS's last -> the carrier group,
    // then DCS's.
    g_logs.clear();
    OriginSplitFrame(false, nullptr, -1.0);
    OriginSplitFrame(true, carrier, 74000.0);   // a far-view episode again, but within the minute: no new lines
    at(0, carrier), at(1, frame), at(2, carrier), at(3, frame);
    g_ropeCalls.clear();
    rope(cable, 4, infos, flags, outs, sampler);
    CHECK(g_calls.empty());
    CHECK(g_ropeCalls.size() == 2);
    if (g_ropeCalls.size() == 2) {
        CHECK(g_ropeCalls[0].infos == (std::vector<const void*>{infos[0], infos[2]}));
        CHECK(g_ropeCalls[0].flags == (std::vector<int>{1, 0}));
        CHECK(g_ropeCalls[0].outs == (std::vector<void*>{outs[0], outs[2]}));
        CHECK(g_ropeCalls[1].infos == (std::vector<const void*>{infos[1], infos[3]}));
        CHECK(g_ropeCalls[1].flags == (std::vector<int>{0, 1}));
        CHECK(g_ropeCalls[1].outs == (std::vector<void*>{outs[1], outs[3]}));
        CHECK(g_ropeCalls[0].self == cable && g_ropeCalls[1].self == cable && g_ropeCalls[1].sampler == sampler);
    }
    // Its own per-object line (two models seen so far in this episode, so it is the third), once.
    CHECK(CountLogs("PLATORIGIN split: arresting cable (SplineRopeParsable ") == 1);
    CHECK(CountLogs("in 4 collections with 2 origins, 2 at the carrier views' origin; the first was ILARTS's") == 1);
    rope(cable, 4, infos, flags, outs, sampler);
    CHECK(CountLogs("PLATORIGIN split: arresting cable") == 1);
    // Split off: passed through unsplit to the cable's original.
    OriginSplitEnable(false);
    g_ropeCalls.clear();
    rope(cable, 4, infos, flags, outs, sampler);
    CHECK(g_ropeCalls.size() == 1 && g_ropeCalls[0].infosPtr == infos);
    OriginSplitEnable(true);
    // More than four origins and an unreadable collection: one plain call to the cable's original.
    for (int i = 0; i < 5; ++i) at(i, o5[i]);
    g_ropeCalls.clear();
    rope(cable, 5, infos, flags, outs, sampler);
    void* reserved2 = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    const void* bad2[2] = {infos[0], reserved2};
    rope(cable, 2, bad2, flags, outs, sampler);
    VirtualFree(reserved2, 0, MEM_RELEASE);
    CHECK(g_ropeCalls.size() == 2 && g_ropeCalls[0].infosPtr == infos && g_ropeCalls[1].infosPtr == bad2);
    CHECK(g_calls.empty());
    // The 30 s line counts the cable calls on their own (3 on several origins, 2 split; the > 4 origins call is not one).
    g_logs.clear();
    OriginSplitTestStatsNow();
    OriginSplitFrame(true, carrier, 74000.0);
    CHECK(CountLogs("models 0/0, arresting cables 3/2") == 1);
    // Nothing happened since: no line.
    g_logs.clear();
    OriginSplitTestStatsNow();
    OriginSplitFrame(false, nullptr, -1.0);
    CHECK(CountLogs("PLATORIGIN split 30s:") == 0);
    printf("  stats: %s\n", OriginSplitStats().c_str());
}

void TestPatch(const std::wstring& dcs) {
    printf("patch on Visualizer.dll:\n");
    const std::wstring visPath = dcs + L"\\bin\\Visualizer.dll", gvPath = dcs + L"\\bin\\GraphicsVista.dll";
    HMODULE vis = LoadLibraryExW(visPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    HMODULE gv = LoadLibraryExW(gvPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!vis || !gv) {
        printf("  skipped: cannot map %ls / %ls (%lu)\n", visPath.c_str(), gvPath.c_str(), GetLastError());
        return;
    }
    auto base = reinterpret_cast<unsigned char*>(vis);
    // What the loader would have written into the import slot.
    void* fn = reinterpret_cast<void*>(GetProcAddress(
        gv, "?collectRenderables@GraphSceneObject@Graphics@@UEAAXIPEAPEBUCollectionInfo@render@@QEB_NPEAPEAV?$vector@"
            "PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@AEAUILightProbeSampler@4@@Z"));
    CHECK(fn == reinterpret_cast<unsigned char*>(gv) + 0xC8C0);
    // First with the import unresolved: refused (it must lead to GraphicsVista's function).
    std::string why = OriginSplitInstall();
    printf("  install with the import unresolved: %s\n", why.empty() ? "ok" : why.c_str());
    CHECK(!why.empty());
    CHECK(PatchSlot(reinterpret_cast<void**>(base + 0x22E3E0), fn));
    void* before = nullptr;
    memcpy(&before, base + 0x23BF00, 8);
    CHECK(before == base + 0x1D4EB3);
    unsigned char around[32];
    memcpy(around, base + 0x23BEF0, 32);
    // enlight.dll not mapped yet: the models' patch alone, the cable's refused (logged).
    g_logs.clear();
    why = OriginSplitInstall();
    printf("  install: %s\n", why.empty() ? "ok" : why.c_str());
    CHECK(why.empty());
    CHECK(CountLogs("NOT installed for SplineRopeParsable collectRenderables (arresting cables): enlight.dll not loaded") == 1);
    if (why.empty()) {
        void* slot = nullptr;
        memcpy(&slot, base + 0x23BF00, 8);
        CHECK(slot == OriginSplitTestWrap());
        CHECK(memcmp(base + 0x23BEF0, around, 16) == 0 && memcmp(base + 0x23BF08, around + 24, 8) == 0);
        OriginSplitRemove();
        memcpy(&slot, base + 0x23BF00, 8);
        CHECK(slot == before);
        CHECK(memcmp(base + 0x23BEF0, around, 32) == 0);
    }

    printf("patch on enlight.dll:\n");
    const std::wstring enPath = dcs + L"\\bin\\enlight.dll";
    HMODULE en = LoadLibraryExW(enPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!en) {
        printf("  FAIL: cannot map %ls (%lu)\n", enPath.c_str(), GetLastError());
        ++g_fail;
        return;
    }
    auto eb = reinterpret_cast<unsigned char*>(en);
    void* ropeBefore = nullptr;
    memcpy(&ropeBefore, eb + 0x7FE10, 8);
    CHECK(ropeBefore == eb + 0x63B50);
    unsigned char ropeAround[32];
    memcpy(ropeAround, eb + 0x7FE08, 32);
    g_logs.clear();
    why = OriginSplitInstall();
    printf("  install both: %s\n", why.empty() ? "ok" : why.c_str());
    CHECK(why.empty());
    CHECK(CountLogs("PLATORIGIN object split installed:") == 2);
    CHECK(CountLogs("NOT installed") == 0);
    void* slot = nullptr;
    if (why.empty()) {
        memcpy(&slot, eb + 0x7FE10, 8);
        CHECK(slot == OriginSplitTestWrap(1));
        memcpy(&slot, base + 0x23BF00, 8);
        CHECK(slot == OriginSplitTestWrap(0));
        CHECK(memcmp(eb + 0x7FE08, ropeAround, 8) == 0 && memcmp(eb + 0x7FE18, ropeAround + 16, 16) == 0);
        // Installing again while installed: nothing changes.
        CHECK(OriginSplitInstall().empty());
        memcpy(&slot, eb + 0x7FE10, 8);
        CHECK(slot == OriginSplitTestWrap(1));
        OriginSplitRemove();
        memcpy(&slot, eb + 0x7FE10, 8);
        CHECK(slot == ropeBefore);
        memcpy(&slot, base + 0x23BF00, 8);
        CHECK(slot == before);
        CHECK(memcmp(eb + 0x7FE08, ropeAround, 32) == 0);
    }

    // The cable's code differs (the origin offset in `lea rdx,[rbx+50Ch]`): the cable is refused, the models' patch
    // still goes in.
    unsigned char* lea = eb + 0x63CFB;
    DWORD old = 0;
    CHECK(VirtualProtect(lea, 1, PAGE_EXECUTE_READWRITE, &old));
    const unsigned char keep = *lea;
    CHECK(keep == 0x0C);
    *lea = 0x1C;
    g_logs.clear();
    why = OriginSplitInstall();
    printf("  install with the cable's code changed: %s\n", why.empty() ? "ok" : why.c_str());
    CHECK(why.empty());
    CHECK(CountLogs("NOT installed for SplineRopeParsable collectRenderables (arresting cables): enlight+0x63B50 is not the "
                    "expected code") == 1);
    memcpy(&slot, eb + 0x7FE10, 8);
    CHECK(slot == ropeBefore);
    memcpy(&slot, base + 0x23BF00, 8);
    CHECK(slot == OriginSplitTestWrap(0));
    OriginSplitRemove();
    *lea = keep;

    // The slot no longer holds the exported function (someone else's patch there): refused too, left alone.
    void* other = eb + 0x63B51;
    CHECK(PatchSlot(reinterpret_cast<void**>(eb + 0x7FE10), other));
    g_logs.clear();
    CHECK(OriginSplitInstall().empty());
    CHECK(CountLogs("enlight+0x7FE10 does not hold SplineRopeParsable::collectRenderables") == 1);
    memcpy(&slot, eb + 0x7FE10, 8);
    CHECK(slot == other);
    OriginSplitRemove();
    CHECK(PatchSlot(reinterpret_cast<void**>(eb + 0x7FE10), ropeBefore));

    // Visualizer refusing (its import unresolved again): the cable's patch goes in alone and comes out again.
    void* resolved = nullptr;
    memcpy(&resolved, base + 0x22E3E0, 8);
    CHECK(PatchSlot(reinterpret_cast<void**>(base + 0x22E3E0), nullptr));
    g_logs.clear();
    CHECK(OriginSplitInstall().empty());
    CHECK(CountLogs("NOT installed for GraphSceneObject collectRenderables (models)") == 1);
    memcpy(&slot, eb + 0x7FE10, 8);
    CHECK(slot == OriginSplitTestWrap(1));
    memcpy(&slot, base + 0x23BF00, 8);
    CHECK(slot == before);
    OriginSplitRemove();
    memcpy(&slot, eb + 0x7FE10, 8);
    CHECK(slot == ropeBefore);
    CHECK(PatchSlot(reinterpret_cast<void**>(base + 0x22E3E0), resolved));
    CHECK(memcmp(eb + 0x7FE08, ropeAround, 32) == 0);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    TestWrapper();
    if (argc > 1) TestPatch(argv[1]);
    printf(g_fail ? "FAILED (%d)\n" : "ALL OK\n", g_fail);
    return g_fail ? 1 : 0;
}
