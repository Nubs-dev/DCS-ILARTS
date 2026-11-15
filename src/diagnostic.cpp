// DCS LSO diagnostic DLL - read-only pass.
//
// What this does:
//   * logs process/module information and DCS binary versions
//   * resolves selected exports by their exact decorated (mangled) names
//   * reads (never writes) the RenderAPI global pointers exported by renderer.dll
//   * identifies the vtable behind each global pointer and which module owns it
//   * maps DX11Renderer vtable slots back to exported method names
//
// What this does NOT do:
//   * call any DCS engine function or C++ method (engine calls live in probe.cpp, opt-in)
//   * hook, patch or write to any DCS memory
//   * touch D3D11 / DXGI state

#include "common.h"

#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace lsodiag {
namespace {

std::atomic<LONG> g_busy{0};  // 1 while a diagnostic pass is running

struct ExportEntry {
    std::string name;
    const void* address;
};

const IMAGE_NT_HEADERS64* NtHeaders(HMODULE module) {
    auto* base = reinterpret_cast<const BYTE*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    return nt;
}

std::vector<ExportEntry> ReadExports(HMODULE module) {
    std::vector<ExportEntry> out;
    const IMAGE_NT_HEADERS64* nt = NtHeaders(module);
    if (!nt) return out;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return out;
    auto* base = reinterpret_cast<const BYTE*>(module);
    auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
    auto* ords = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);
    out.reserve(exp->NumberOfNames);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        if (ords[i] >= exp->NumberOfFunctions) continue;
        DWORD rva = funcs[ords[i]];
        // Forwarded exports point back inside the export directory; they are strings, not code.
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) continue;
        out.push_back({reinterpret_cast<const char*>(base + names[i]), base + rva});
    }
    return out;
}

void LogMatchingExports(HMODULE module, const char* moduleName, std::initializer_list<const char*> filters) {
    if (!module) {
        Log("MODULE MISSING: %s", moduleName);
        return;
    }
    std::vector<ExportEntry> exports = ReadExports(module);
    Log("EXPORTS in %s (%zu total), filtered:", moduleName, exports.size());
    unsigned matches = 0;
    for (const auto& e : exports) {
        bool hit = filters.size() == 0;
        for (const char* f : filters) hit = hit || strstr(e.name.c_str(), f) != nullptr;
        if (!hit) continue;
        Log("  rva=0x%08llX %s",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(e.address) - reinterpret_cast<uintptr_t>(module)),
            e.name.c_str());
        ++matches;
    }
    Log("  (%u matches)", matches);
}

void SnapshotModules() {
    HANDLE snap = INVALID_HANDLE_VALUE;
    // CreateToolhelp32Snapshot can transiently fail with ERROR_BAD_LENGTH while modules load.
    for (int attempt = 0; attempt < 5 && snap == INVALID_HANDLE_VALUE; ++attempt) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snap == INVALID_HANDLE_VALUE) {
        Log("CreateToolhelp32Snapshot failed: %lu", GetLastError());
        return;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    unsigned count = 0;
    if (Module32FirstW(snap, &entry)) {
        do {
            Log("MODULE %-32s base=%p size=0x%08lX %s", Utf8(entry.szModule).c_str(), entry.modBaseAddr,
                entry.modBaseSize, Utf8(entry.szExePath).c_str());
            ++count;
        } while (Module32NextW(snap, &entry));
    } else {
        Log("Module32First failed: %lu", GetLastError());
    }
    Log("(%u modules)", count);
    CloseHandle(snap);
}

struct Global {
    const char* decorated;
    const char* label;
};

// Reads an exported global pointer and identifies the object it points at by its vtable.
const void* InspectGlobal(HMODULE renderer, const Global& g) {
    FARPROC symbol = renderer ? GetProcAddress(renderer, g.decorated) : nullptr;
    if (!symbol) {
        Log("GLOBAL %-24s export not found (%s)", g.label, g.decorated);
        return nullptr;
    }
    const void* object = nullptr;
    if (!SafeReadPtr(reinterpret_cast<const void*>(symbol), &object)) {
        Log("GLOBAL %-24s symbol=%p READ FAILED", g.label, reinterpret_cast<void*>(symbol));
        return nullptr;
    }
    if (!object) {
        Log("GLOBAL %-24s symbol=%p value=null (not initialised yet?)", g.label, reinterpret_cast<void*>(symbol));
        return nullptr;
    }
    const void* vtable = nullptr;
    bool vtOk = SafeReadPtr(object, &vtable);
    Log("GLOBAL %-24s symbol=%p value=%p [%s] vtable=%p [%s]", g.label, reinterpret_cast<void*>(symbol), object,
        DescribeAddress(object).c_str(), vtOk ? vtable : nullptr,
        vtOk ? DescribeAddress(vtable).c_str() : "READ FAILED");
    return object;
}

// Verifies RenderAPI::pRenderApi is a DX11Renderer by comparing its vtable pointer with the
// vtable exported by dx11backend.dll, then lists which exported methods occupy which slots.
void InspectRendererVtable(HMODULE dx11, const void* renderApi) {
    if (!dx11 || !renderApi) {
        Log("VTABLE CHECK skipped (dx11backend=%p pRenderApi=%p)", dx11, renderApi);
        return;
    }
    auto exportedVtbl = reinterpret_cast<const void*>(GetProcAddress(dx11, "??_7DX11Renderer@RenderAPI@@6B@"));
    const void* actualVtbl = nullptr;
    if (!exportedVtbl || !SafeReadPtr(renderApi, &actualVtbl)) {
        Log("VTABLE CHECK failed: exported vtable=%p", exportedVtbl);
        return;
    }
    bool match = exportedVtbl == actualVtbl;
    Log("VTABLE CHECK pRenderApi vptr=%p exported DX11Renderer vftable=%p => %s", actualVtbl, exportedVtbl,
        match ? "MATCH: pRenderApi is a DX11Renderer (primary base)"
              : "MISMATCH: not a plain DX11Renderer (or a non-primary base subobject); do not assume layout");
    if (!match) return;

    // Identical functions may be folded by the linker, so one address can carry several names.
    std::unordered_multimap<const void*, std::string> byAddress;
    for (const auto& e : ReadExports(dx11)) {
        if (e.name.find("@DX11Renderer@RenderAPI@@") != std::string::npos) byAddress.emplace(e.address, e.name);
    }

    const IMAGE_NT_HEADERS64* nt = NtHeaders(dx11);
    auto lo = reinterpret_cast<uintptr_t>(dx11);
    auto hi = lo + (nt ? nt->OptionalHeader.SizeOfImage : 0);
    auto* slots = static_cast<const void* const*>(exportedVtbl);
    Log("DX11Renderer vtable slots (stops at first entry outside dx11backend.dll):");
    for (int i = 0; i < 512; ++i) {
        const void* fn = nullptr;
        if (!SafeReadPtr(&slots[i], &fn)) break;
        auto v = reinterpret_cast<uintptr_t>(fn);
        if (v < lo || v >= hi) {
            Log("  (end at slot %d: %p)", i, fn);
            break;
        }
        auto range = byAddress.equal_range(fn);
        if (range.first == range.second) {
            Log("  [%3d] +0x%03X rva=0x%llX <unexported>", i, i * 8, static_cast<unsigned long long>(v - lo));
        }
        for (auto it = range.first; it != range.second; ++it) {
            Log("  [%3d] +0x%03X rva=0x%llX %s", i, i * 8, static_cast<unsigned long long>(v - lo), it->second.c_str());
        }
    }
}

void LogDcsVersions() {
    const std::wstring exe = ModulePath(GetModuleHandleW(nullptr));
    Log("Executable: %s  version=%s", Utf8(exe).c_str(), FileVersion(exe).c_str());
    const wchar_t* modules[] = {L"renderer.dll", L"dx11backend.dll", L"dxgui.dll", L"Visualizer.dll",
                                L"GraphicsCore.dll", L"edSupercarrier.dll"};
    for (const wchar_t* name : modules) {
        HMODULE m = GetModuleHandleW(name);
        if (!m) {
            Log("VERSION %-20s not loaded", Utf8(name).c_str());
            continue;
        }
        const std::wstring path = ModulePath(m);
        Log("VERSION %-20s %s base=%p %s", Utf8(name).c_str(), FileVersion(path).c_str(), m, Utf8(path).c_str());
    }
}

}  // namespace

void RunDiagnosticPass() {
    Log("============================================================");
    Log("DCS LSO diagnostic pass. PID=%lu", GetCurrentProcessId());
    Log("READ-ONLY pass: no DCS functions are called; no memory or graphics state is modified.");
    Log("Log file: %s", Utf8(LogPath()).c_str());
    LogDcsVersions();
    SnapshotModules();

    HMODULE renderer = GetModuleHandleW(L"renderer.dll");
    HMODULE dx11 = GetModuleHandleW(L"dx11backend.dll");
    HMODULE dxgui = GetModuleHandleW(L"dxgui.dll");
    HMODULE sc = GetModuleHandleW(L"edSupercarrier.dll");

    Log("--- renderer.dll globals ---");
    const Global globals[] = {
        {"?pRenderApi@RenderAPI@@3PEAUIRenderAPI@1@EA", "RenderAPI::pRenderApi"},
        {"?pTexMan@RenderAPI@@3PEAUITextureManager@1@EA", "RenderAPI::pTexMan"},
        {"?pShMan@RenderAPI@@3PEAUIShaderManager@1@EA", "RenderAPI::pShMan"},
        {"?pStateMan@RenderAPI@@3PEAUIStateManager@1@EA", "RenderAPI::pStateMan"},
        {"?pGpuLowAccess@RenderAPI@@3PEAVIGpuLowAccess@1@EA", "RenderAPI::pGpuLowAccess"},
    };
    const void* renderApi = nullptr;
    for (const auto& g : globals) {
        const void* v = InspectGlobal(renderer, g);
        if (&g == &globals[0]) renderApi = v;
    }

    Log("--- DX11Renderer identity ---");
    InspectRendererVtable(dx11, renderApi);

    Log("--- relevant exports ---");
    LogMatchingExports(dx11, "dx11backend.dll", {"RenderTarget", "copyTexture", "getResourcesList", "TextureManager"});
    LogMatchingExports(dxgui, "dxgui.dll", {"Surface", "Offscreen", "@StaticSkin@", "?get@GUI@gui@@"});
    LogMatchingExports(sc, "edSupercarrier.dll", {});

    Log("Diagnostic pass complete.");
}

// Busy guard: claim before starting a pass, release when it ends.
bool ClaimPass() { return g_busy.exchange(1) == 0; }
void ReleasePass() { g_busy.store(0); }

// Runs one pass that the caller has already claimed. Returns ERROR_SUCCESS / ERROR_OPEN_FAILED.
DWORD RunClaimedPass() {
    DWORD rc = ERROR_OPEN_FAILED;
    if (OpenLog()) {
        RunDiagnosticPass();
        CloseLog();
        rc = ERROR_SUCCESS;
    }
    ReleasePass();
    return rc;
}

DWORD RunGuardedPass() { return ClaimPass() ? RunClaimedPass() : ERROR_BUSY; }

}  // namespace lsodiag

// Synchronous entry point. Runs one diagnostic pass on the calling thread.
// Returns ERROR_SUCCESS, ERROR_BUSY (a pass is already running) or ERROR_OPEN_FAILED.
extern "C" __declspec(dllexport) DWORD WINAPI RunLSODiagnostic(void*) { return lsodiag::RunGuardedPass(); }

// Asynchronous entry point: starts one pass on a worker thread and returns immediately.
// The worker holds its own reference to this DLL (released by FreeLibraryAndExitThread),
// so a FreeLibrary by the host while the pass is running cannot unmap code under it.
extern "C" __declspec(dllexport) DWORD WINAPI StartLSODiagnostic(void*) {
    if (!lsodiag::ClaimPass()) return ERROR_BUSY;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&StartLSODiagnostic),
                            &self)) {
        DWORD e = GetLastError();
        lsodiag::ReleasePass();
        return e;
    }
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](void* module) -> DWORD {
            lsodiag::RunClaimedPass();  // releases the claim
            FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
        },
        self, 0, nullptr);
    if (!thread) {
        DWORD e = GetLastError();
        FreeLibrary(self);
        lsodiag::ReleasePass();
        return e;
    }
    CloseHandle(thread);
    return ERROR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // Deliberately empty: no work under the loader lock. Work starts only via the exports.
    }
    return TRUE;
}
