#include "compat.h"

#include "common.h"
#include "engine.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace lsodiag {
namespace {

enum class SigKind { Code, Export, Ptrs };

struct Sig {
    const wchar_t* module;
    SigKind kind;
    uintptr_t rva;           // Code / Ptrs
    const char* exportName;  // Export
    const unsigned char* bytes;
    const unsigned char* mask;  // 0 = relocated by the loader, not compared
    const uint32_t* rvas;       // Ptrs: expected targets as RVAs
    int count;                  // bytes (Code / Export) or pointers (Ptrs)
    const char* why;
};

#include "compat_sigs.inc"

// DCS.exe is packed (its code is not on disk), so it is checked by structure at run time: the globalView
// object's vtable and its slot 31 (the LSO camera getter the PLAT patches build on).
constexpr uintptr_t kGlobalViewVtblRva = 0x138BC00, kSlot31Rva = 0xE1CC20;
constexpr int kSlot31 = 31;

std::mutex g_mutex;
CompatResult g_result;
bool g_final = false;

std::string Hex(uintptr_t v) {
    char b[24];
    _snprintf_s(b, _countof(b), _TRUNCATE, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

std::string Narrow(const wchar_t* w) { return Utf8(w); }

// "" if the fingerprint matches, else a short description.
std::string CheckSig(const Sig& s) {
    HMODULE m = GetModuleHandleW(s.module);
    if (!m) return Narrow(s.module) + " not loaded";
    const uintptr_t base = reinterpret_cast<uintptr_t>(m);
    const unsigned char* at = nullptr;
    std::string where;
    if (s.kind == SigKind::Export) {
        at = reinterpret_cast<const unsigned char*>(GetProcAddress(m, s.exportName));
        where = Narrow(s.module) + "!" + s.exportName;
        if (!at) return where + " missing (" + s.why + ")";
    } else {
        at = reinterpret_cast<const unsigned char*>(base + s.rva);
        where = Narrow(s.module) + "+" + Hex(s.rva);
    }
    if (s.kind == SigKind::Ptrs) {
        for (int i = 0; i < s.count; ++i) {
            uintptr_t p = 0;
            if (!SafeRead(at + 8 * i, &p, sizeof(p))) return where + " unreadable (" + s.why + ")";
            if (p - base != s.rvas[i]) return where + " differs (" + s.why + ")";
        }
        return {};
    }
    std::vector<unsigned char> cur(static_cast<size_t>(s.count));
    if (!SafeRead(at, cur.data(), cur.size())) return where + " unreadable (" + s.why + ")";
    for (int i = 0; i < s.count; ++i) {
        if (s.mask[i] && cur[i] != s.bytes[i]) return where + " differs (" + s.why + ")";
    }
    return {};
}

// DCS.exe structure check. "" = matches; skipped when the vtable is already this DLL's private copy
// (the check passed before the PLAT patch was installed).
std::string CheckGlobalView() {
    HMODULE exe = GetModuleHandleW(nullptr);
    HMODULE inter = GetModuleHandleW(L"Inter.dll");
    if (!inter) return "Inter.dll not loaded";
    auto sym = reinterpret_cast<const void*>(GetProcAddress(inter, "?globalView@@3PEAVIView@@EA"));
    const void* view = nullptr;
    if (!sym || !SafeReadPtr(sym, &view) || !view) return "globalView not available yet";
    uintptr_t vtbl = 0, slot = 0;
    if (!SafeRead(view, &vtbl, sizeof(vtbl))) return "globalView unreadable";
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(vtbl), &owner);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&CheckGlobalView), &self);
    if (owner && owner == self) return {};
    const uintptr_t exeBase = reinterpret_cast<uintptr_t>(exe);
    if (vtbl - exeBase != kGlobalViewVtblRva) return "DCS.exe globalView vtable moved (" + Hex(vtbl - exeBase) + ")";
    if (!SafeRead(reinterpret_cast<const void*>(vtbl + 8 * kSlot31), &slot, sizeof(slot))) return "DCS.exe globalView vtable unreadable";
    if (slot - exeBase != kSlot31Rva) return "DCS.exe LSO camera getter moved (" + Hex(slot - exeBase) + ")";
    return {};
}

bool Pending(const std::string& why) {
    return why.find("not loaded") != std::string::npos || why.find("not available yet") != std::string::npos;
}

}  // namespace

const char* CompatStateName(CompatState s) {
    switch (s) {
        case CompatState::Verified: return "verified";
        case CompatState::Matched: return "matched";
        case CompatState::Incompatible: return "incompatible";
        default: return "pending";
    }
}

CompatResult CompatCheck() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_final) return g_result;
    CompatResult r;
    r.dcsVersion = FileVersion(ModulePath(GetModuleHandleW(nullptr)));
    std::vector<std::string> fails;
    int checked = 0;
    for (const Sig& s : kSigs) {
        std::string why = CheckSig(s);
        if (Pending(why)) {
            r.detail = why;
            return g_result = r;  // a module is not loaded yet: ask again later
        }
        ++checked;
        if (!why.empty()) fails.push_back(why);
    }
    {
        std::string why = CheckGlobalView();
        if (Pending(why)) {
            r.detail = why;
            return g_result = r;
        }
        ++checked;
        if (!why.empty()) fails.push_back(why);
    }
    r.checked = checked;
    r.failed = static_cast<int>(fails.size());
    for (size_t i = 0; i < fails.size() && i < 3; ++i) r.detail += (i ? "; " : "") + fails[i];
    if (fails.size() > 3) r.detail += "; +" + std::to_string(fails.size() - 3) + " more";
    if (r.dcsVersion == kVerifiedBuild) {
        // The build everything was verified on. A mismatch here means the fingerprint table is wrong,
        // not DCS: keep running (as before this check existed) and say so loudly in the log.
        r.state = CompatState::Verified;
        if (r.failed) Log("COMPAT WARNING: DCS %s is the verified build but %d of %d fingerprints differ: %s", r.dcsVersion.c_str(),
                          r.failed, r.checked, r.detail.c_str());
    } else {
        r.state = r.failed ? CompatState::Incompatible : CompatState::Matched;
    }
    for (const auto& f : fails) Log("COMPAT mismatch: %s", f.c_str());
    Log("COMPAT DCS %s (verified build %s): %s, %d fingerprints checked, %d differ", r.dcsVersion.c_str(), kVerifiedBuild,
        CompatStateName(r.state), r.checked, r.failed);
    g_final = true;
    return g_result = r;
}

bool CompatOk() {
    const CompatResult r = CompatCheck();
    return r.state == CompatState::Verified || r.state == CompatState::Matched;
}

std::string CompatWhy() {
    const CompatResult r = CompatCheck();
    switch (r.state) {
        case CompatState::Verified:
        case CompatState::Matched: return {};
        case CompatState::Pending: return "DCS compatibility not checked yet (" + r.detail + ")";
        default: return "DCS " + r.dcsVersion + " is not compatible (" + r.detail + ")";
    }
}

}  // namespace lsodiag
