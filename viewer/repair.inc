// ---------------------------------------------------------------- installation check and repair
// (Included by lso_viewer.cpp after console.inc.)
//
// SETTINGS > Installation > REPAIR, "ILARTS.exe --repair" (the installer runs it after installing), and after a DCS
// update: puts back what the mod needs outside its own folder and says whether this DCS can run it.
//   * the hook: Saved Games\DCS\Scripts\Hooks\ILARTS-hook.lua, as shipped (a copy lives next to ILARTS.exe)
//   * no hook of the old name (LSODiagnostic-hook.lua: two hooks load the mod twice)
//   * ILARTS.dll in Mods\tech\ILARTS\bin (a development build next to this program: copied there)
//   * viewer_path.txt pointing at this program when it does not live in the mod's folder
//   * Saved Games\DCS\Config\autoexec.cfg allowing what the hook needs ("userhooks" unsafe API, dostring_in
//     "server"): added to whatever is allowed already, never replacing it; backed up first, removed on uninstall
//   * the virtual camera component when cameras are on and it is missing (asks for administrator rights)
//   * DCS's version against the build this release was made for, and what the DLL found on that version the
//     last time it ran (Header::compat, remembered in [dcs])
// DCS updates do not touch Saved Games; what an update can break is the DLL's compatibility. That is reported;
// only a new ILARTS release fixes it.

std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}
bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::string ReadFileBytes(const std::wstring& p) {
    std::string out;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER n{};
    GetFileSizeEx(h, &n);
    out.resize(static_cast<size_t>(n.QuadPart));
    DWORD got = 0;
    if (!out.empty()) ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    out.resize(got);
    CloseHandle(h);
    return out;
}
bool WriteFileBytes(const std::wstring& p, const std::string& data) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr);
    CloseHandle(h);
    return ok && put == data.size();
}
std::wstring ExeDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d = exe;
    return d.substr(0, d.find_last_of(L"\\/"));
}
std::wstring ParentDir(const std::wstring& p, int up = 1) {
    std::wstring d = p;
    for (int i = 0; i < up; ++i) {
        const size_t at = d.find_last_of(L"\\/");
        if (at == std::wstring::npos) return {};
        d = d.substr(0, at);
    }
    return d;
}
// DCS's Saved Games folder: the one this program is installed in (Mods\tech\ILARTS\bin), else the one that has the
// mod, else the one DCS uses (DCS, then DCS.openbeta).
std::wstring SavedGamesDcs() {
    if (!g_testSavedGames.empty()) return g_testSavedGames;  // --test-saved-games (tuning.inc)
    const std::wstring exeDir = ExeDir();
    if (LowerW(exeDir).size() > 21 && LowerW(exeDir).rfind(L"\\mods\\tech\\ilarts\\bin") == LowerW(exeDir).size() - 21) return ParentDir(exeDir, 4);
    wchar_t up[MAX_PATH];
    ExpandEnvironmentStringsW(L"%USERPROFILE%\\Saved Games", up, MAX_PATH);
    const std::wstring cands[] = {std::wstring(up) + L"\\DCS", std::wstring(up) + L"\\DCS.openbeta"};
    for (const auto& c : cands)
        if (FileExists(c + L"\\Mods\\tech\\ILARTS")) return c;
    for (const auto& c : cands)
        if (FileExists(c)) return c;
    return cands[0];
}
// DCS.exe's version ("2.9.31.29434") from where Eagle Dynamics' installer says DCS is; "" if not found.
std::wstring InstalledDcsVersion(std::wstring* path = nullptr) {
    for (const wchar_t* key : {L"Software\\Eagle Dynamics\\DCS World", L"Software\\Eagle Dynamics\\DCS World OpenBeta"}) {
        wchar_t dir[MAX_PATH] = {};
        DWORD n = sizeof(dir);
        if (RegGetValueW(HKEY_CURRENT_USER, key, L"Path", RRF_RT_REG_SZ, nullptr, dir, &n) != ERROR_SUCCESS) continue;
        const std::wstring exe = std::wstring(dir) + L"\\bin\\DCS.exe";
        DWORD h = 0;
        const DWORD sz = GetFileVersionInfoSizeW(exe.c_str(), &h);
        if (!sz) continue;
        std::vector<uint8_t> buf(sz);
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (!GetFileVersionInfoW(exe.c_str(), 0, sz, buf.data()) || !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi)
            continue;
        if (path) *path = dir;
        wchar_t v[64];
        swprintf_s(v, L"%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        return v;
    }
    return {};
}

// What the DLL reported about the DCS it ran in ([dcs] version, verified, state): kept for when DCS is not running.
void RememberCompat() {
    if (!HeaderValid() || app.header->compat.state == 0) return;
    const std::wstring v = Ascii(app.header->compat.dcsVersion, sizeof(app.header->compat.dcsVersion));
    const std::wstring ver = Ascii(app.header->compat.verifiedVersion, sizeof(app.header->compat.verifiedVersion));
    const std::wstring st = std::to_wstring(app.header->compat.state);
    static std::wstring last;
    if (last == v + L"|" + ver + L"|" + st) return;
    last = v + L"|" + ver + L"|" + st;
    WritePrivateProfileStringW(L"dcs", L"version", v.c_str(), app.iniPath.c_str());
    WritePrivateProfileStringW(L"dcs", L"verified", ver.c_str(), app.iniPath.c_str());
    WritePrivateProfileStringW(L"dcs", L"state", st.c_str(), app.iniPath.c_str());
}

// The hook's needs in autoexec.cfg, added to what other mods allow (Lua, run by DCS after its defaults).
const char kAutoexecBlock[] =
    "\r\n-- DCS ILARTS: its hook reads the carrier, aircraft and wind from the mission (adds to what is allowed already)\r\n"
    "do\r\n"
    "  local function allow(t, v) t = t or {} for _, x in ipairs(t) do if x == v then return t end end table.insert(t, v) return t end\r\n"
    "  net.allow_unsafe_api = allow(net.allow_unsafe_api, \"userhooks\")\r\n"
    "  net.allow_dostring_in = allow(net.allow_dostring_in, \"server\")\r\n"
    "end\r\n";

bool AutoexecAllows(const std::string& cfg) {
    if (cfg.find("-- DCS ILARTS:") != std::string::npos) return true;
    auto has = [&](const char* var, const char* item) {  // "var = { ... "item" ... }" on one assignment
        for (size_t at = cfg.find(var); at != std::string::npos; at = cfg.find(var, at + 1)) {
            const size_t open = cfg.find('{', at), close = cfg.find('}', at);
            if (open == std::string::npos || close == std::string::npos || open > close) continue;
            if (cfg.substr(at, open - at).find('=') == std::string::npos) continue;
            if (cfg.substr(open, close - open).find(item) != std::string::npos) return true;
        }
        return false;
    };
    return has("net.allow_unsafe_api", "\"userhooks\"") && has("net.allow_dostring_in", "\"server\"");
}

struct RepairResult {
    std::vector<std::wstring> fixed, problems, notes;
};

bool DcsRunning() {
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) found = _wcsicmp(pe.szExeFile, L"DCS.exe") == 0;
    CloseHandle(snap);
    return found;
}

// autoexec.cfg is the one file outside the mod's own that ILARTS changes. Before the first change it is copied to
// autoexec.cfg.ILARTS-backup; the change is one marked block, appended. Uninstalling (the installer runs
// "--remove-permissions"; OMM / OvGME users click SETTINGS > Installation > REMOVE) takes out exactly that block, so
// whatever else was edited since is kept. The backup goes once the file is back to it; a file ILARTS created goes
// once only the block was in it. The REMOVE key also takes out the two unmarked lines ILARTS 1.0 asked for by hand
// (backed up first); lines merged with other mods' entries are left alone (the key shows ALLOWED).
const wchar_t kAutoexecBackup[] = L".ILARTS-backup";
std::wstring AutoexecPath() { return SavedGamesDcs() + L"\\Config\\autoexec.cfg"; }

// Where the block is in the text: [begin, end) including the line break in front of it, or npos.
std::pair<size_t, size_t> AutoexecBlock(const std::string& text) {
    size_t b = text.find("-- DCS ILARTS:");
    if (b == std::string::npos) return {std::string::npos, std::string::npos};
    size_t e = text.find("\nend", b);
    if (e == std::string::npos) return {std::string::npos, std::string::npos};
    e = text.find('\n', e + 1);
    e = e == std::string::npos ? text.size() : e + 1;
    if (b >= 1 && text[b - 1] == '\n') --b;  // the blank line the block was appended with
    if (b >= 1 && text[b - 1] == '\r') --b;
    return {b, e};
}
// ILARTS 1.0 asked for these two lines to be added by hand; they carry no marker. [begin, end) of each whole line
// (with its line break), only when both are there, else empty.
std::vector<std::pair<size_t, size_t>> AutoexecLegacyLines(const std::string& text) {
    static const char* const kLines[] = {"net.allow_unsafe_api={\"userhooks\"}", "net.allow_dostring_in={\"mission\",\"server\"}"};
    std::vector<std::pair<size_t, size_t>> found;
    for (const char* want : kLines) {
        bool hit = false;
        for (size_t b = 0; b < text.size() && !hit;) {
            size_t e = text.find('\n', b);
            e = e == std::string::npos ? text.size() : e + 1;
            std::string bare;
            for (size_t i = b; i < e; ++i)
                if (!strchr(" \t\r\n", text[i])) bare += text[i];
            if (bare == want) {
                found.push_back({b, e});
                hit = true;
            }
            b = e;
        }
        if (!hit) return {};
    }
    return found;
}
// What SETTINGS > Installation > REMOVE can take out: the block, or the two lines 1.0 asked for.
bool AutoexecHasOurs() {
    const std::string text = ReadFileBytes(AutoexecPath());
    return AutoexecBlock(text).first != std::string::npos || !AutoexecLegacyLines(text).empty();
}
bool AutoexecAllowsNow() { return AutoexecAllows(ReadFileBytes(AutoexecPath())); }

void AddPermissions(RepairResult& r) {
    const std::wstring cfg = AutoexecPath(), cfgDir = cfg.substr(0, cfg.find_last_of(L'\\'));
    const std::string text = ReadFileBytes(cfg);
    if (AutoexecAllows(text)) return;
    SHCreateDirectoryExW(nullptr, cfgDir.c_str(), nullptr);
    const std::wstring backup = cfg + kAutoexecBackup;
    if (FileExists(cfg) && !FileExists(backup) && !CopyFileW(cfg.c_str(), backup.c_str(), TRUE)) {
        r.problems.push_back(L"Could not back up " + cfg + L" (left unchanged)");
        return;
    }
    if (WriteFileBytes(cfg, text + kAutoexecBlock)) r.fixed.push_back(L"autoexec.cfg: hook permissions added");
    else r.problems.push_back(L"Could not write " + cfg);
}

// legacy: also the two hand-added lines of 1.0 (the REMOVE key; the uninstaller takes out only the block).
RepairResult RemovePermissions(bool legacy = false) {
    RepairResult r;
    const std::wstring cfg = AutoexecPath(), backup = cfg + kAutoexecBackup;
    std::string text = ReadFileBytes(cfg);
    const auto [b, e] = AutoexecBlock(text);
    if (b != std::string::npos) {
        text.erase(b, e - b);
    } else {
        auto lines = legacy ? AutoexecLegacyLines(text) : std::vector<std::pair<size_t, size_t>>{};
        if (lines.empty()) {
            r.notes.push_back(L"autoexec.cfg has no ILARTS permissions");
            return r;
        }
        if (!FileExists(backup) && !CopyFileW(cfg.c_str(), backup.c_str(), TRUE)) {
            r.problems.push_back(L"Could not back up " + cfg + L" (left unchanged)");
            return r;
        }
        std::sort(lines.rbegin(), lines.rend());  // from the end, so the earlier positions hold
        for (const auto& [lb, le] : lines) text.erase(lb, le - lb);
    }
    const bool onlyOurs = text.find_first_not_of(" \t\r\n") == std::string::npos && !FileExists(backup);
    if (onlyOurs ? !DeleteFileW(cfg.c_str()) : !WriteFileBytes(cfg, text)) {
        r.problems.push_back(L"Could not write " + cfg);
        return r;
    }
    r.fixed.push_back(L"autoexec.cfg: ILARTS permissions removed");
    if (FileExists(backup) && ReadFileBytes(backup) == text) DeleteFileW(backup.c_str());
    return r;
}

RepairResult Repair(bool interactive) {
    RepairResult r;
    const std::wstring sg = SavedGamesDcs(), exeDir = ExeDir();
    const bool dcsRunning = DcsRunning();
    if (!FileExists(sg)) {
        r.problems.push_back(L"DCS's Saved Games folder was not found (" + sg + L")");
        return r;
    }
    // the hook
    std::wstring bundled = exeDir + L"\\ILARTS-hook.lua";
    if (!FileExists(bundled)) bundled = ParentDir(exeDir, 2) + L"\\deploy\\Scripts\\Hooks\\ILARTS-hook.lua";  // development build
    const std::wstring hooks = sg + L"\\Scripts\\Hooks", hook = hooks + L"\\ILARTS-hook.lua";
    if (FileExists(bundled)) {
        const std::string want = ReadFileBytes(bundled);
        if (!want.empty() && ReadFileBytes(hook) != want) {
            SHCreateDirectoryExW(nullptr, hooks.c_str(), nullptr);
            if (WriteFileBytes(hook, want)) r.fixed.push_back(FileExists(hook) ? L"Hook restored" : L"Hook installed");
            else r.problems.push_back(L"Could not write " + hook);
        }
    } else if (!FileExists(hook)) {
        r.problems.push_back(L"The hook is missing and no copy was found: reinstall DCS ILARTS");
    }
    const std::wstring oldHook = hooks + L"\\LSODiagnostic-hook.lua";
    if (FileExists(oldHook) && DeleteFileW(oldHook.c_str())) r.fixed.push_back(L"Old LSODiagnostic hook removed");
    // the DLL
    const std::wstring bin = sg + L"\\Mods\\tech\\ILARTS\\bin", dll = bin + L"\\ILARTS.dll";
    if (LowerW(bin) != LowerW(exeDir)) {  // a development build: the mod's folder gets this build's DLL
        const std::wstring mine = exeDir + L"\\ILARTS.dll";
        if (FileExists(mine) && ReadFileBytes(mine) != ReadFileBytes(dll)) {
            SHCreateDirectoryExW(nullptr, bin.c_str(), nullptr);
            if (dcsRunning) r.notes.push_back(L"ILARTS.dll not updated while DCS runs");
            else if (CopyFileW(mine.c_str(), dll.c_str(), FALSE)) r.fixed.push_back(L"ILARTS.dll updated");
            else r.problems.push_back(L"Could not copy ILARTS.dll");
        }
        const std::wstring vp = bin + L"\\viewer_path.txt";
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::string utf8;
        for (int n = WideCharToMultiByte(CP_UTF8, 0, exe, -1, nullptr, 0, nullptr, nullptr); n > 1;) {
            utf8.resize(static_cast<size_t>(n) - 1);
            WideCharToMultiByte(CP_UTF8, 0, exe, -1, utf8.data(), n, nullptr, nullptr);
            break;
        }
        if (FileExists(bin) && ReadFileBytes(vp) != utf8 && WriteFileBytes(vp, utf8)) r.fixed.push_back(L"DCS starts this ILARTS.exe");
    } else {
        const std::wstring vp = bin + L"\\viewer_path.txt";  // installed: DCS starts the program next to the DLL
        if (FileExists(vp) && DeleteFileW(vp.c_str())) r.fixed.push_back(L"Development viewer path removed");
    }
    if (!FileExists(dll)) r.problems.push_back(L"ILARTS.dll is missing: reinstall DCS ILARTS");
    AddPermissions(r);
    // the virtual cameras
    wchar_t cams[256] = {};
    GetPrivateProfileStringW(L"outputs", L"cameras", L"", cams, 256, app.iniPath.c_str());
    if (cams[0] && VCamApi() && !VCamInstalled()) {
        if (interactive) {
            CameraComponentRun(true);
            r.fixed.push_back(L"Camera component: installing");
        } else {
            r.problems.push_back(L"Virtual cameras are on but the camera component is not installed");
        }
    }
    // DCS
    const std::wstring now = InstalledDcsVersion();
    wchar_t v[64] = {}, ver[64] = {}, st[8] = {};
    GetPrivateProfileStringW(L"dcs", L"version", L"", v, 64, app.iniPath.c_str());
    GetPrivateProfileStringW(L"dcs", L"verified", L"", ver, 64, app.iniPath.c_str());
    GetPrivateProfileStringW(L"dcs", L"state", L"", st, 8, app.iniPath.c_str());
    if (now.empty()) {
        r.notes.push_back(L"DCS's installation was not found in the registry");
    } else if (now == v && _wtoi(st) == 3) {
        r.problems.push_back(L"DCS " + now + L" is not compatible with this ILARTS (made for " + ver + L"): an ILARTS update is needed");
    } else if (now == v && (_wtoi(st) == 1 || _wtoi(st) == 2)) {
        r.notes.push_back(L"DCS " + now + L": compatible");
    } else {
        r.notes.push_back(L"DCS " + now + L": not checked yet - start a mission (ILARTS checks it at the first one)");
    }
    return r;
}

std::wstring RepairText(const RepairResult& r) {
    std::wstring t;
    for (const auto& x : r.problems) t += (t.empty() ? L"" : L"\n") + std::wstring(L"\x2716 ") + x;
    for (const auto& x : r.fixed) t += (t.empty() ? L"" : L"\n") + std::wstring(L"\x2714 ") + x;
    for (const auto& x : r.notes) t += (t.empty() ? L"" : L"\n") + x;
    return t.empty() ? std::wstring(L"Nothing to repair") : t;
}

// SETTINGS > REPAIR: the report for the dialog's banner.
std::wstring RepairNow() { return RepairText(Repair(true)); }
// At start: an installation from a zip (OMM, OvGME, by hand) has no autoexec.cfg permissions until REPAIR runs, and
// that step is easy to miss. If the hook or the permissions are missing, say so and offer to set it up. Cancel
// ("I only join multiplayer servers as a client", which needs no permissions) stops the question about the
// permissions ([install] skip_permissions=1); a missing hook is always reported.
void StartupCheck() {
    const std::wstring cl = GetCommandLineW();
    for (const wchar_t* quiet : {L"--test", L"--wait-pid"})
        if (cl.find(quiet) != std::wstring::npos) return;
    const std::wstring sg = SavedGamesDcs();
    if (!FileExists(sg)) return;  // no DCS here: nothing to set up
    const bool hook = FileExists(sg + L"\\Scripts\\Hooks\\ILARTS-hook.lua");
    const bool perms = AutoexecAllows(ReadFileBytes(AutoexecPath())) || GetPrivateProfileIntW(L"install", L"skip_permissions", 0, app.iniPath.c_str());
    if (hook && perms) return;
    std::wstring t = L"DCS ILARTS is not set up yet:\n\n";
    if (!hook) t += L"\x2022 The hook is missing from Saved Games\\DCS\\Scripts\\Hooks. Without it, DCS does not start ILARTS.\n";
    if (!AutoexecAllows(ReadFileBytes(AutoexecPath())))
        t += L"\x2022 DCS does not yet allow the hook to read the mission (two permissions in Saved Games\\DCS\\Config\\autoexec.cfg). "
             L"Single player and multiplayer hosting need them.\n";
    t += L"\nSet it up now? (The same as SETTINGS > Installation > REPAIR. autoexec.cfg is backed up first.)\n\n"
         L"Yes: set it up now.\nNo: not now (asks again next time).\n"
         L"Cancel: I only join multiplayer servers as a client - do not ask about the permissions again.";
    const int a = MessageBoxW(nullptr, t.c_str(), L"DCS ILARTS - Setup", MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON1);
    if (a == IDCANCEL) WritePrivateProfileStringW(L"install", L"skip_permissions", L"1", app.iniPath.c_str());
    if (a == IDYES) {
        const RepairResult r = Repair(true);
        MessageBoxW(nullptr, (RepairText(r) + (DcsRunning() ? L"\n\nRestart DCS for the changes to apply." : L"")).c_str(),
                    L"DCS ILARTS - Setup", MB_OK | (r.problems.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
}

// SETTINGS > Installation > Permissions: ADD / REMOVE.
std::wstring PermissionsNow(bool add) {
    if (!add) return RepairText(RemovePermissions(true));
    RepairResult r;
    AddPermissions(r);
    if (r.fixed.empty() && r.problems.empty()) r.notes.push_back(L"autoexec.cfg already allows what the hook needs");
    return RepairText(r);
}
