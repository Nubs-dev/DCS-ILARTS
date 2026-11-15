#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <vector>

namespace lsodiag {
namespace {
std::mutex g_logMutex;
std::atomic<bool> g_verbose{false};
constexpr LONGLONG kLogRotateBytes = 5LL * 1024 * 1024;  // at open: a bigger log becomes diagnostic.old.log
HANDLE g_log = INVALID_HANDLE_VALUE;
int g_logRefs = 0;
std::wstring g_logPath;

std::wstring ResolveLogPath() {
    wchar_t base[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        n = GetTempPathW(MAX_PATH, base);  // never fall back to the unpredictable CWD
        if (n == 0 || n >= MAX_PATH) return L"ILARTS.log";
    }
    std::wstring dir(base);
    if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    dir += L"\\ILARTS";
    CreateDirectoryW(dir.c_str(), nullptr);  // ERROR_ALREADY_EXISTS is fine
    return dir + L"\\ILARTS.log";
}
}  // namespace

bool OpenLog() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log == INVALID_HANDLE_VALUE) {
        g_logPath = ResolveLogPath();
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fa) &&
            ((static_cast<LONGLONG>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) > kLogRotateBytes) {
            std::wstring old = g_logPath.substr(0, g_logPath.size() - 4) + L".old.log";
            MoveFileExW(g_logPath.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        g_log = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_log == INVALID_HANDLE_VALUE) return false;
    }
    ++g_logRefs;
    return true;
}

void CloseLog() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logRefs > 0 && --g_logRefs == 0 && g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
}

const std::wstring& LogPath() { return g_logPath; }

bool Verbose() { return g_verbose.load(std::memory_order_relaxed); }
void SetVerbose(bool on) { g_verbose.store(on); }

void Log(const char* format, ...) {
    char body[4096];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(body, _countof(body), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[4200];
    int len = _snprintf_s(line, _countof(line), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%5lu] %s\r\n",
                          t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                          GetCurrentThreadId(), body);
    if (len < 0) len = static_cast<int>(strlen(line));

    OutputDebugStringA(line);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_log, line, static_cast<DWORD>(len), &written, nullptr);
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ModulePath(HMODULE module) {
    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!n) return L"<path unavailable>";
    path.resize(n);
    return path;
}

std::string BaseName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return Utf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
}

std::string DescribeAddress(const void* p) {
    if (!p) return "null";
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(p), &m) || !m) {
        return "<not in any module (heap/stack/unmapped)>";
    }
    char buf[600];
    _snprintf_s(buf, _countof(buf), _TRUNCATE, "%s+0x%llX", BaseName(ModulePath(m)).c_str(),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(m)));
    return buf;
}

std::string FileVersion(const std::wstring& path) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return "<no version resource>";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return "<version read failed>";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return "<no fixed info>";
    char buf[64];
    _snprintf_s(buf, _countof(buf), _TRUNCATE, "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS),
                LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return buf;
}

bool SafeRead(const void* address, void* out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &got) && got == size;
}

// Import-table patching (unprotected DCS modules only).
void** FindIatSlot(HMODULE mod, const char* dll, const char* func) {
    auto base = reinterpret_cast<unsigned char*>(mod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), func) == 0)
                return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

bool PatchSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    return true;
}

}  // namespace lsodiag
