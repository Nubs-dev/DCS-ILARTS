// Shared logging and read-only memory helpers.
#pragma once

#include <windows.h>

#include <string>

namespace lsodiag {

// Logging: UTF-8, timestamped, flushed per line, mirrored to OutputDebugString.
// OpenLog/CloseLog are reference counted so nested users (Lua call inside a pass) are safe.
bool OpenLog();
void CloseLog();
void Log(const char* format, ...);
// Verbose diagnostics (development): large one-off dumps and per-second statistics. Off by default; the
// hook's VERBOSE switch turns it on.
bool Verbose();
void SetVerbose(bool on);
const std::wstring& LogPath();

std::string Utf8(const std::wstring& w);
std::wstring ModulePath(HMODULE module);
std::string BaseName(const std::wstring& path);
std::string FileVersion(const std::wstring& path);
std::string DescribeAddress(const void* p);  // "module.dll+0xRVA" or a note

// Reads via ReadProcessMemory: fails cleanly instead of faulting on unmapped memory.
bool SafeRead(const void* address, void* out, size_t size);
inline bool SafeReadPtr(const void* address, const void** out) { return SafeRead(address, out, sizeof(*out)); }

// Import address table slot of dll!func in mod (by name), or null. PatchSlot swaps it atomically.
void** FindIatSlot(HMODULE mod, const char* dll, const char* func);
bool PatchSlot(void** slot, void* value);

// The read-only pass implemented in diagnostic.cpp. Caller must hold the log open.
void RunDiagnosticPass();
// Same pass wrapped in the busy guard and log open/close.
// Returns ERROR_SUCCESS / ERROR_BUSY / ERROR_OPEN_FAILED.
DWORD RunGuardedPass();

}  // namespace lsodiag
