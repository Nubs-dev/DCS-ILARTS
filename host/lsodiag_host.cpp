// Offline test host for LSODiagnostic.dll. Never used inside DCS.
//
//   lsodiag_host.exe <path\to\LSODiagnostic.dll> [--map <DCS bin dir> [<extra dll path> ...]]
//
// Without --map: exercises logging and the "module missing" paths.
// With --map: maps the named DCS DLLs into this process with DONT_RESOLVE_DLL_REFERENCES.
// That flag maps and relocates the image but runs no DllMain and loads no dependencies, so
// no DCS code executes. It lets the DLL's export resolution be tested against the real
// binaries. Exported globals then hold their static initial value (normally null), so the
// vtable check is expected to be skipped.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: %s <LSODiagnostic.dll> [--map <DCS bin dir> [extra.dll ...]]\n", argv[0]);
        return 2;
    }
    std::vector<HMODULE> mapped;
    if (argc >= 4 && std::wstring(argv[2]) == L"--map") {
        std::wstring bin = argv[3];
        std::vector<std::wstring> paths;
        for (const wchar_t* n : {L"renderer.dll", L"dx11backend.dll", L"dxgui.dll", L"Visualizer.dll", L"GraphicsCore.dll"})
            paths.push_back(bin + L"\\" + n);
        for (int i = 4; i < argc; ++i) paths.push_back(argv[i]);
        for (const auto& p : paths) {
            HMODULE m = LoadLibraryExW(p.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
            wprintf(L"map %-80s => %p%s\n", p.c_str(), m, m ? L"" : L"  (failed)");
            if (m) mapped.push_back(m);
        }
    }

    HMODULE diag = LoadLibraryW(argv[1]);
    if (!diag) {
        fwprintf(stderr, L"LoadLibrary(%s) failed: %lu\n", argv[1], GetLastError());
        return 1;
    }
    using Fn = DWORD(WINAPI*)(void*);
    auto run = reinterpret_cast<Fn>(GetProcAddress(diag, "RunLSODiagnostic"));
    auto start = reinterpret_cast<Fn>(GetProcAddress(diag, "StartLSODiagnostic"));
    wprintf(L"RunLSODiagnostic=%p StartLSODiagnostic=%p\n", reinterpret_cast<void*>(run), reinterpret_cast<void*>(start));
    if (!run || !start) return 1;

    DWORD rc = run(nullptr);
    wprintf(L"RunLSODiagnostic returned %lu\n", rc);

    // Exercise the async path and the unload-safety reference: start, then immediately
    // FreeLibrary. The worker's own module reference must keep the code mapped.
    rc = start(nullptr);
    wprintf(L"StartLSODiagnostic returned %lu\n", rc);
    DWORD rc2 = start(nullptr);
    wprintf(L"second StartLSODiagnostic returned %lu (expect 170 ERROR_BUSY, or 0 if first already finished)\n", rc2);
    FreeLibrary(diag);
    Sleep(2000);  // let the worker finish before process exit

    for (HMODULE m : mapped) FreeLibrary(m);
    wprintf(L"done\n");
    return 0;
}
