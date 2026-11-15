#include "mfd.h"

#include "common.h"
#include "compat.h"
#include "engine.h"

#include <cstdint>
#include <cstdio>

namespace lsodiag {
namespace {

constexpr uintptr_t kSceneManagerRenderGraphVtblRva = 0x242128;  // Visualizer.dll 2.9.31 (RTTI)
constexpr size_t kSceneRendererOffset = 0x6D8;                    // SceneManagerRenderGraph -> DCSSceneRenderer*
constexpr size_t kViewportsBegin = 0xC9F8, kViewportsEnd = 0xCA00, kViewportStride = 0x3F90;
constexpr size_t kSizesPtr = 0x7C8, kSizeEntry = 20, kSizeW = 8, kSizeH = 0xC;
constexpr int kMaxViewports = 128;

constexpr const char* kResize = "?resizeMFDViewport@DCSSceneRenderer@@QEAAXIVVec2i@osg@@@Z";
using ResizeFn = void (*)(void* self, unsigned int index, uint64_t packedVec2i);  // Vec2i {x,y} by value

SRWLOCK g_lock = SRWLOCK_INIT;
struct Request {
    int index = -1, w = 0, h = 0;
    bool loggedApplied = false, loggedWaiting = false;
    ULONGLONG nextCheck = 0;
};
Request g_reqs[4];  // the PLAT's viewport and the extra cameras' viewports

struct SceneRenderer {
    void* self = nullptr;
    ResizeFn resize = nullptr;
    int count = 0;
    const char* sizes = nullptr;
};

// All reads go through SafeRead; returns "" or why the scene renderer must not be used.
std::string Resolve(SceneRenderer& s) {
    if (EngineDisabled()) return "engine calls disabled after an earlier exception";
    HMODULE inter = GetModuleHandleW(L"Inter.dll");
    HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
    HMODULE sr = GetModuleHandleW(L"SceneRenderer.dll");
    if (!inter || !vis || !sr) return "Inter/Visualizer/SceneRenderer not loaded";
    if (!CompatOk()) return CompatWhy();
    auto sym = reinterpret_cast<const void*>(GetProcAddress(inter, "?visualizer@@3PEAVsmSceneManager@@EA"));
    const void* obj = nullptr;
    if (!sym || !SafeReadPtr(sym, &obj) || !obj) return "Inter.dll!visualizer unavailable or null";
    const void* vptr = nullptr;
    const void* expected = reinterpret_cast<const char*>(vis) + kSceneManagerRenderGraphVtblRva;
    if (!SafeReadPtr(obj, &vptr) || vptr != expected) return "visualizer is not a SceneManagerRenderGraph (vtable mismatch)";
    const void* scene = nullptr;
    if (!SafeReadPtr(static_cast<const char*>(obj) + kSceneRendererOffset, &scene) || !scene)
        return "DCSSceneRenderer pointer unavailable";
    const char* begin = nullptr;
    const char* end = nullptr;
    if (!SafeRead(static_cast<const char*>(scene) + kViewportsBegin, &begin, sizeof(begin)) ||
        !SafeRead(static_cast<const char*>(scene) + kViewportsEnd, &end, sizeof(end)))
        return "viewport vector unreadable";
    if (!begin || end < begin || (end - begin) % kViewportStride != 0 || (end - begin) / kViewportStride > kMaxViewports)
        return "viewport vector implausible";
    const char* sizes = nullptr;
    if (!SafeRead(static_cast<const char*>(scene) + kSizesPtr, &sizes, sizeof(sizes)) || !sizes) return "size table unavailable";
    s.self = const_cast<void*>(scene);
    s.count = static_cast<int>((end - begin) / kViewportStride);
    s.sizes = sizes;
    s.resize = reinterpret_cast<ResizeFn>(GetProcAddress(sr, kResize));
    if (!s.resize) return "resizeMFDViewport export not found";
    return {};
}

bool ReadSize(const SceneRenderer& s, int index, int& w, int& h) {
    const char* e = s.sizes + static_cast<size_t>(index) * kSizeEntry;
    return SafeRead(e + kSizeW, &w, sizeof(w)) && SafeRead(e + kSizeH, &h, sizeof(h));
}

bool SehResize(const SceneRenderer& s, int index, int w, int h, DWORD* code) {
    uint64_t packed = (static_cast<uint64_t>(static_cast<uint32_t>(h)) << 32) | static_cast<uint32_t>(w);
    __try {
        s.resize(s.self, static_cast<unsigned int>(index), packed);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

constexpr int kEnforceSlot = 43;                                  // SceneManagerRenderGraph vtable
constexpr uintptr_t kEnforceRva = 0x194230;                       // enforceResolutionOfMFD (2.9.31)
using EnforceFn = void (*)(void* self, int index, int w, int h);

bool SehEnforce(EnforceFn fn, void* self, int index, int w, int h, DWORD* code) {
    __try {
        fn(self, index, w, h);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

std::string MfdEnforceResolution(int index, int w, int h) {
    if (index < 0 || index >= kMaxViewports || w <= 0 || h <= 0 || w > 4096 || h > 4096) return "argument out of range";
    if (EngineDisabled()) return "engine calls disabled after an earlier exception";
    HMODULE inter = GetModuleHandleW(L"Inter.dll");
    HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
    if (!inter || !vis) return "Inter/Visualizer not loaded";
    if (!CompatOk()) return CompatWhy();
    auto sym = reinterpret_cast<const void*>(GetProcAddress(inter, "?visualizer@@3PEAVsmSceneManager@@EA"));
    const void* obj = nullptr;
    if (!sym || !SafeReadPtr(sym, &obj) || !obj) return "Inter.dll!visualizer unavailable or null";
    const void* vptr = nullptr;
    auto* expectedVtbl = reinterpret_cast<const void* const*>(reinterpret_cast<const char*>(vis) + kSceneManagerRenderGraphVtblRva);
    if (!SafeReadPtr(obj, &vptr) || vptr != expectedVtbl) return "visualizer is not a SceneManagerRenderGraph (vtable mismatch)";
    const void* fn = nullptr;
    const void* expectedFn = reinterpret_cast<const char*>(vis) + kEnforceRva;
    if (!SafeReadPtr(&expectedVtbl[kEnforceSlot], &fn) || fn != expectedFn)
        return "vtable slot 43 is not enforceResolutionOfMFD";
    DWORD code = 0;
    if (!SehEnforce(reinterpret_cast<EnforceFn>(const_cast<void*>(fn)), const_cast<void*>(obj), index, w, h, &code)) {
        DisableEngine("enforceResolutionOfMFD", code);
        return "exception in enforceResolutionOfMFD";
    }
    Log("MFD enforceResolutionOfMFD(%d, %d, %d) called on thread %lu (records size for get_target_size, queues render-thread resize)",
        index, w, h, GetCurrentThreadId());
    return {};
}

std::string MfdViewportSize(int index, int* w, int* h) {
    *w = *h = 0;
    SceneRenderer s;
    const std::string why = Resolve(s);
    if (!why.empty()) return why;
    if (index < 0 || index >= s.count) return "viewport " + std::to_string(index) + " does not exist (" + std::to_string(s.count) + " viewports)";
    if (!ReadSize(s, index, *w, *h)) return "size entry unreadable";
    return {};
}

std::string MfdLogViewports() {
    SceneRenderer s;
    std::string why = Resolve(s);
    if (!why.empty()) {
        Log("MFD viewports unavailable: %s", why.c_str());
        return why;
    }
    Log("MFD DCSSceneRenderer=%p viewports=%d sizeTable=%p", s.self, s.count, static_cast<const void*>(s.sizes));
    for (int i = 0; i < s.count; ++i) {
        DWORD e[5] = {};
        if (!SafeRead(s.sizes + static_cast<size_t>(i) * kSizeEntry, e, sizeof(e))) {
            Log("MFD  [%2d] size entry unreadable", i);
            continue;
        }
        Log("MFD  [%2d] %4ld x %-4ld  (entry %08lX %08lX %08lX %08lX %08lX)", i, static_cast<long>(e[2]),
            static_cast<long>(e[3]), e[0], e[1], e[2], e[3], e[4]);
    }
    return {};
}

std::string MfdRequestResolution(int index, int w, int h) {
    if (index < 0 || index >= kMaxViewports) return "index out of range";
    if (w < 0 || h < 0 || w > 4096 || h > 4096) return "size out of range (0..4096)";
    AcquireSRWLockExclusive(&g_lock);
    Request* slot = nullptr;
    for (Request& r : g_reqs)
        if (r.index == index) slot = &r;
    for (Request& r : g_reqs)
        if (!slot && r.index < 0) slot = &r;
    if (slot) {
        *slot = Request{};
        if (w > 0 && h > 0) {
            slot->index = index;
            slot->w = w;
            slot->h = h;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!slot) return "too many viewport size requests";
    Log("MFD resolution request: viewport %d -> %dx%d (applied on the render thread)", index, w, h);
    return {};
}

void MfdOnRenderThread() {
    if (!TryAcquireSRWLockExclusive(&g_lock)) return;
    const ULONGLONG now = GetTickCount64();
    for (Request& req : g_reqs) {
        if (req.index < 0 || now < req.nextCheck) continue;
        req.nextCheck = now + 1000;  // check / re-apply once a second
        SceneRenderer s;
        std::string why = Resolve(s);
        if (!why.empty()) {
            if (!req.loggedWaiting) Log("MFD resize waiting: %s", why.c_str());
            req.loggedWaiting = true;
        } else if (req.index >= s.count) {
            if (!req.loggedWaiting) Log("MFD resize waiting: viewport %d does not exist yet (%d viewports)", req.index, s.count);
            req.loggedWaiting = true;
        } else {
            int cw = 0, ch = 0;
            ReadSize(s, req.index, cw, ch);
            if (cw != req.w || ch != req.h) {
                DWORD code = 0;
                if (SehResize(s, req.index, req.w, req.h, &code)) {
                    int nw = 0, nh = 0;
                    ReadSize(s, req.index, nw, nh);
                    Log("MFD viewport %d resized on render thread %lu: %dx%d -> %dx%d (now reads %dx%d)", req.index,
                        GetCurrentThreadId(), cw, ch, req.w, req.h, nw, nh);
                } else {
                    DisableEngine("resizeMFDViewport", code);
                    req.index = -1;
                }
            } else if (!req.loggedApplied) {
                Log("MFD viewport %d is at the requested %dx%d", req.index, cw, ch);
                req.loggedApplied = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace lsodiag
