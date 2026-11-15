#include "render_callback.h"

#include "common.h"
#include "compat.h"
#include "engine.h"

#include <atomic>

namespace lsodiag {
namespace {

constexpr const char* kRegister =
    "?registerRendererCallback@Graphics@@YAXPEAVRendererCallback@1@W4RendererCallbackEvent@1@H@Z";
constexpr int kEventEndFrame = 6;
constexpr int kPriorityLast = 0x7FFFFFFF;  // same as AVIMaker
// GraphicsCore (same RVA in 2.9.25 and 2.9.31): event registry = 9 x std::vector<{RendererCallback*, int}> at this RVA,
// stride 0x18 (begin, end, capacity). Used read-only to verify our registration.
constexpr uintptr_t kRegistryRva = 0x11DC30;

std::atomic<RenderFrameFn> g_fn{nullptr};
std::atomic<unsigned long> g_renderThread{0};
std::atomic<unsigned long long> g_calls{0};
std::atomic<unsigned long long> g_eventCalls[9];
std::atomic<unsigned long> g_eventThread[9];
bool g_installed = false;

void OnEvent(int ev) {
    unsigned long tid = GetCurrentThreadId();
    unsigned long expected = 0;
    if (g_eventThread[ev].compare_exchange_strong(expected, tid)) {
        Log("RENDERCB first event %d callback on thread %lu", ev, tid);
    }
    g_eventCalls[ev].fetch_add(1, std::memory_order_relaxed);
    if (ev == kEventEndFrame) {
        expected = 0;
        g_renderThread.compare_exchange_strong(expected, tid);
        g_calls.fetch_add(1, std::memory_order_relaxed);
    }
    if (RenderFrameFn fn = g_fn.load(std::memory_order_acquire)) fn(ev);
}

int DisableEngineFromCallback(DWORD code) {
    DisableEngine("renderer callback", code);
    return 0;
}

void Guarded(int ev) {
    __try {
        OnEvent(ev);
    } __except (DisableEngineFromCallback(GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Binary-compatible with Graphics::RendererCallback: vptr only, destructor in slot 0, nine
// handlers in slots 1..9 (fireRendererCallbacks: event 0->slot1 1->2 2->3 3->4 4->9 5->5 6->6
// 7->8 8->7). Declaration order == vtable order.
class AllEventsCallback {
public:
    virtual ~AllEventsCallback() {}       // slot 0
    virtual void slot1() { Guarded(0); }  // event 0
    virtual void slot2() { Guarded(1); }  // event 1
    virtual void slot3() { Guarded(2); }  // event 2
    virtual void slot4() { Guarded(3); }  // event 3
    virtual void slot5() { Guarded(5); }  // event 5
    virtual void slot6() { Guarded(6); }  // event 6: Renderer::EndFrame
    virtual void slot7() { Guarded(8); }  // event 8
    virtual void slot8() { Guarded(7); }  // event 7
    virtual void slot9() { Guarded(4); }  // event 4
};

}  // namespace

std::string EnsureRenderCallback(RenderFrameFn fn) {
    g_fn.store(fn, std::memory_order_release);
    if (g_installed) return {};

    HMODULE gc = GetModuleHandleW(L"GraphicsCore.dll");
    if (!gc) return "GraphicsCore.dll not loaded";
    if (!CompatOk()) return CompatWhy();
    using RegisterFn = void (*)(void* cb, int event, int priority);
    auto reg = reinterpret_cast<RegisterFn>(GetProcAddress(gc, kRegister));
    if (!reg) return "registerRendererCallback export not found";

    // Pin this DLL: the render thread may call us until process exit.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&EnsureRenderCallback), &self)) {
        return "failed to pin ILARTS.dll";
    }

    // One object registered for all 9 events (9 inserts, once per process, never removed).
    auto* cb = new AllEventsCallback();  // intentionally never deleted (never unregistered)
    for (int ev = 0; ev < 9; ++ev) reg(cb, ev, kPriorityLast);
    g_installed = true;

    // Read-only verification of the registry vector for event 6.
    auto* vec = reinterpret_cast<const char*>(gc) + kRegistryRva + kEventEndFrame * 0x18;
    const char* begin = nullptr;
    const char* end = nullptr;
    size_t found = SIZE_MAX, count = 0;
    if (SafeRead(vec, &begin, sizeof(begin)) && SafeRead(vec + 8, &end, sizeof(end)) && begin && end >= begin) {
        count = static_cast<size_t>(end - begin) / 16;
        for (size_t i = 0; i < count && i < 256; ++i) {
            const void* p = nullptr;
            if (SafeReadPtr(begin + i * 16, &p) && p == cb) found = i;
        }
    }
    Log("RENDERCB registered %p for events 0-8 priority 0x%X from thread %lu; event 6 registry has %zu entries, ours %s",
        static_cast<void*>(cb), kPriorityLast, GetCurrentThreadId(), count,
        found == SIZE_MAX ? "NOT FOUND (registry layout differs?)" : ("at index " + std::to_string(found)).c_str());
    return {};
}

void LogRendererCallbackRegistry() {
    HMODULE gc = GetModuleHandleW(L"GraphicsCore.dll");
    if (!gc || !CompatOk()) {
        Log("RENDERCB registry: GraphicsCore.dll missing or DCS not compatible");
        return;
    }
    // event -> vtable slot that fireRendererCallbacks calls (GraphicsCore 0xBE530 jump table)
    static const int kSlotForEvent[9] = {1, 2, 3, 4, 9, 5, 6, 8, 7};
    for (int ev = 0; ev < 9; ++ev) {
        auto* vec = reinterpret_cast<const char*>(gc) + kRegistryRva + ev * 0x18;
        const char* begin = nullptr;
        const char* end = nullptr;
        if (!SafeRead(vec, &begin, sizeof(begin)) || !SafeRead(vec + 8, &end, sizeof(end)) || end < begin) {
            Log("RENDERCB event %d: unreadable", ev);
            continue;
        }
        size_t n = begin ? static_cast<size_t>(end - begin) / 16 : 0;
        Log("RENDERCB event %d (calls vtable slot %d): %zu callbacks", ev, kSlotForEvent[ev], n);
        for (size_t i = 0; i < n && i < 64; ++i) {
            const void* cb = nullptr;
            int prio = 0;
            const void* vtbl = nullptr;
            const void* handler = nullptr;
            SafeReadPtr(begin + i * 16, &cb);
            SafeRead(begin + i * 16 + 8, &prio, sizeof(prio));
            if (cb) SafeReadPtr(cb, &vtbl);
            if (vtbl) SafeReadPtr(static_cast<const void* const*>(vtbl) + kSlotForEvent[ev], &handler);
            Log("RENDERCB    [%zu] cb=%p prio=0x%X vtable=%s handler=%s", i, cb, prio, DescribeAddress(vtbl).c_str(),
                DescribeAddress(handler).c_str());
        }
    }
}

unsigned long RenderThreadId() { return g_renderThread.load(); }

std::string RenderEventStats() {
    std::string out;
    char buf[64];
    for (int ev = 0; ev < 9; ++ev) {
        _snprintf_s(buf, _countof(buf), _TRUNCATE, "%se%d=%llu@%lu", ev ? " " : "", ev,
                    g_eventCalls[ev].load(), g_eventThread[ev].load());
        out += buf;
    }
    return out;
}
unsigned long long RenderCallbackCount() { return g_calls.load(); }

}  // namespace lsodiag
