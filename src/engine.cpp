#include "engine.h"

#include "common.h"
#include "compat.h"

namespace lsodiag {
namespace {

constexpr const char* kVtbl = "??_7DX11Renderer@RenderAPI@@6B@";
constexpr const char* kIsRtExists = "?isRenderTargetExists@DX11Renderer@RenderAPI@@UEAA_NPEBD@Z";
constexpr const char* kGetResList = "?getResourcesList@DX11Renderer@RenderAPI@@UEAAPEBDXZ";
constexpr const char* kFreeStr = "?freeStringMem@DX11Renderer@RenderAPI@@UEAAXPEBD@Z";
constexpr const char* kGetRt =
    "?getRenderTarget@DX11Renderer@RenderAPI@@UEAA?AV?$Ptr@UITexture@RenderAPI@@UDeleter@12@@ed@@PEBD@Z";
// Non-virtual getters (QEAA): verified as trivial field loads; not required to be in the vtable.
constexpr const char* kGetDevice = "?getDevice@DX11Renderer@RenderAPI@@QEAAPEAUID3D11Device@@XZ";
constexpr const char* kGetContext = "?getDeviceContext@DX11Renderer@RenderAPI@@QEAAPEAUID3D11DeviceContext@@XZ";

bool g_disabled = false;

bool InVtable(const void* const* vtbl, const void* fn) {
    for (int i = 0; i < 512; ++i) {
        const void* slot = nullptr;
        if (!SafeReadPtr(&vtbl[i], &slot) || !slot) return false;
        if (slot == fn) return true;
    }
    return false;
}

}  // namespace

bool EngineDisabled() { return g_disabled; }

void DisableEngine(const char* what, DWORD code) {
    g_disabled = true;
    Log("ENGINE %s raised exception 0x%08lX - all engine calls now DISABLED for this session", what, code);
}

std::string ResolveRenderer(Renderer& r) {
    if (g_disabled) return "engine calls disabled after an earlier exception";
    const std::wstring exe = ModulePath(GetModuleHandleW(nullptr));
    // bin-mt (desktop shortcut) vs bin (Start menu): in 2.9.31.29434 (as in 2.9.25) every module in bin-mt is
    // byte-identical to bin (only DCS_updater.exe is missing), so the same verification applies. A
    // build where they differ is caught by the version / export / vtable checks below.
    (void)exe;

    HMODULE renderer = GetModuleHandleW(L"renderer.dll");
    HMODULE dx11 = GetModuleHandleW(L"dx11backend.dll");
    if (!renderer || !dx11) return "renderer.dll or dx11backend.dll not loaded";
    if (!CompatOk()) return CompatWhy();

    auto sym = reinterpret_cast<const void*>(GetProcAddress(renderer, "?pRenderApi@RenderAPI@@3PEAUIRenderAPI@1@EA"));
    const void* obj = nullptr;
    if (!sym || !SafeReadPtr(sym, &obj) || !obj) return "RenderAPI::pRenderApi unavailable or null";

    auto vtbl = reinterpret_cast<const void* const*>(GetProcAddress(dx11, kVtbl));
    const void* vptr = nullptr;
    if (!vtbl || !SafeReadPtr(obj, &vptr)) return "cannot read DX11Renderer vtable";
    if (vptr != vtbl) return "pRenderApi vptr does not match exported DX11Renderer vftable";

    auto get = [dx11](const char* n) { return reinterpret_cast<const void*>(GetProcAddress(dx11, n)); };
    const void* isRt = get(kIsRtExists);
    const void* getRes = get(kGetResList);
    const void* freeS = get(kFreeStr);
    const void* getRt = get(kGetRt);
    const void* getDev = get(kGetDevice);
    const void* getCtx = get(kGetContext);
    if (!isRt || !getRes || !freeS || !getRt || !getDev || !getCtx) return "a required DX11Renderer export is missing";
    for (const void* virt : {isRt, getRes, freeS, getRt}) {
        if (!InVtable(vtbl, virt)) return "a required virtual export is not in the DX11Renderer vtable";
    }

    r.self = const_cast<void*>(obj);
    r.isRtExists = reinterpret_cast<IsRtExistsFn>(const_cast<void*>(isRt));
    r.getResList = reinterpret_cast<GetResListFn>(const_cast<void*>(getRes));
    r.freeStr = reinterpret_cast<FreeStrFn>(const_cast<void*>(freeS));
    r.getRenderTarget = reinterpret_cast<GetRenderTargetFn>(const_cast<void*>(getRt));
    r.getDevice = reinterpret_cast<GetPtrFn>(const_cast<void*>(getDev));
    r.getDeviceContext = reinterpret_cast<GetPtrFn>(const_cast<void*>(getCtx));
    return {};
}

// SEH frames below must not contain objects with destructors.

int SehIsRtExists(const Renderer& r, const char* name, DWORD* code) {
    __try {
        return r.isRtExists(r.self, name) ? 1 : 0;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

const char* SehGetResList(const Renderer& r, DWORD* code) {
    __try {
        return r.getResList(r.self);
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SehFreeStr(const Renderer& r, const char* p, DWORD* code) {
    __try {
        r.freeStr(r.self, p);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* SehGetRenderTarget(const Renderer& r, const char* name, DWORD* code) {
    void* slot = nullptr;  // ed::Ptr<ITexture> is a single intrusive pointer
    __try {
        r.getRenderTarget(r.self, &slot, name);
        return slot;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* SehGetPtr(GetPtrFn fn, void* self, DWORD* code) {
    __try {
        return fn(self);
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SehReleaseTexture(void* texture, DWORD* code) {
    if (!texture) return true;
    __try {
        auto* refs = reinterpret_cast<volatile LONG*>(static_cast<char*>(texture) + kTextureRefCountOffset);
        if (InterlockedDecrement(refs) == 0) {
            using DestroyFn = void (*)(void*);
            auto vtbl = *reinterpret_cast<DestroyFn**>(texture);
            vtbl[kTextureReleaseSlot](texture);
        }
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace lsodiag
