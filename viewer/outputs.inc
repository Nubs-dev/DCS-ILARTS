// ---------------------------------------------------------------- outputs: Spout and virtual cameras
// (Included by lso_viewer.cpp after the streams' pictures.)
//
// Spout ([outputs] spout = a,plat ...): each source listed is a Spout sender "ILARTS <name>" - its picture drawn at
// the recording size into a texture of its own, handed to Spout on the graphics card every 1/fps s. Receivers (OBS
// with the Spout2 plugin, Resolume ...) take it from there: no read-back, no encode.
//
// Virtual cameras ([outputs] cameras = a,plat ...; Windows 11, the camera component installed): each source listed
// is a webcam "ILARTS <name>" (MFCreateVirtualCamera, for as long as the viewer runs). Windows' camera service loads
// ILARTSCam.dll when an application opens one; it makes the shared block (vcam_shared.h) and asks for a size. While
// it reads, the viewer draws the source at that size, reads it back and writes it into the block - only then.
// "composite" ([outputs] tiles = a,b): up to four sources in one picture, arranged as a composite stream's.
// The component: "ILARTS.exe --install-cameras" (admin) copies ILARTSCam.dll to Program Files\DCS ILARTS and
// registers it; --uninstall-cameras undoes that.

void RecStatus(const std::wstring& text, ULONGLONG ms);
void RestartSelf();

const wchar_t* kOutSources[ilcam::kCams] = {L"a", L"b", L"plat", L"lso1", L"lso2", L"island", L"composite"};
const wchar_t* kOutNames[ilcam::kCams] = {L"Monitor A", L"Monitor B", L"PLAT", L"LSO 1", L"LSO 2", L"Island", L"Composite"};
std::vector<std::wstring> g_outTiles = {L"a", L"b"};  // [outputs] tiles

std::vector<std::wstring> SplitList(const std::wstring& s) {
    std::vector<std::wstring> out;
    for (size_t a = 0; a <= s.size();) {
        size_t e = s.find(L',', a);
        if (e == std::wstring::npos) e = s.size();
        if (e > a) out.push_back(s.substr(a, e - a));
        a = e + 1;
    }
    return out;
}
int OutIndex(const std::wstring& src) {
    for (int i = 0; i < ilcam::kCams; ++i)
        if (src == kOutSources[i]) return i;
    return -1;
}
bool OutComposite(int i) { return !wcscmp(kOutSources[i], L"composite"); }
// The sources output i draws: its own, or the composite's tiles.
std::vector<std::wstring> OutKeys(int i) { return OutComposite(i) ? g_outTiles : std::vector<std::wstring>{kOutSources[i]}; }

// ---- Spout
struct SpoutOut {
    bool want = false;
    spoutout::Sender* sender = nullptr;
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    double next = 0;
    bool failed = false;  // Spout could not start on this device (not retried until the settings change)
};
SpoutOut g_spout[ilcam::kCams];

void SpoutRelease(SpoutOut& o) {
    if (o.sender) spoutout::Destroy(o.sender);
    o.sender = nullptr;
    SafeRelease(o.rtv);
    SafeRelease(o.tex);
}

// Draws and sends every wanted Spout source (once per frame; each at the recording frame rate).
void SpoutCapture() {
    if (!app.dev || !RecEnsureGpu()) return;
    const double now = video::Now(), step = 1.0 / std::max(1, g_engCfg.fps);
    for (int i = 0; i < ilcam::kCams; ++i) {
        SpoutOut& o = g_spout[i];
        if (!o.want || o.failed) continue;
        if (!o.tex) {
            video::Engine::Config size;
            size.w = g_engCfg.w & ~1, size.h = g_engCfg.h & ~1;
            if (OutComposite(i)) size.w = kCompositeW, size.h = kCompositeH;
            const D3D11_TEXTURE2D_DESC td = CanvasDesc(size);
            const std::string name = "ILARTS " + Narrow(kOutNames[i]);
            if (FAILED(app.dev->CreateTexture2D(&td, nullptr, &o.tex)) || FAILED(app.dev->CreateRenderTargetView(o.tex, nullptr, &o.rtv)) ||
                !(o.sender = spoutout::Create(app.dev, name.c_str()))) {
                SpoutRelease(o);
                o.failed = true;
                continue;
            }
        }
        if (now < o.next) continue;
        o.next = now - o.next > step ? now + step : o.next + step;
        const std::vector<ViewState> vs = TileViews(OutKeys(i));
        const float black[4] = {0, 0, 0, 1};
        app.ctx->ClearRenderTargetView(o.rtv, black);
        D3D11_TEXTURE2D_DESC d{};
        o.tex->GetDesc(&d);
        DrawTiles(vs, OutComposite(i), o.rtv, static_cast<int>(d.Width), static_cast<int>(d.Height));
        spoutout::Send(o.sender, o.tex);
    }
}

// ---- virtual cameras
typedef HRESULT(WINAPI* CreateVirtualCameraFn)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess, LPCWSTR, LPCWSTR,
                                               const GUID*, ULONG, IMFVirtualCamera**);
struct VCamOut {
    bool want = false;
    IMFVirtualCamera* cam = nullptr;
    HANDLE map = nullptr;
    ilcam::Block* block = nullptr;
    ULONGLONG lastOpen = 0;
    bool reading = false;  // an application takes its frames
    Channel ch;            // its picture at the size asked for, read back
};
VCamOut g_vcam[ilcam::kCams];
struct {
    bool checked = false, api = false;
    CreateVirtualCameraFn create = nullptr;
    HANDLE installer = nullptr;  // --install-cameras / --uninstall-cameras running elevated
    DWORD serverPid = 0;         // the camera service's process when it started
    std::wstring error;          // why the cameras are not there (last attempt)
} g_vcamSys;

// Windows 11's virtual camera API (mfsensorgroup.dll).
bool VCamApi() {
    if (!g_vcamSys.checked) {
        g_vcamSys.checked = true;
        if (HMODULE m = LoadLibraryW(L"mfsensorgroup.dll"))
            g_vcamSys.create = reinterpret_cast<CreateVirtualCameraFn>(GetProcAddress(m, "MFCreateVirtualCamera"));
        g_vcamSys.api = g_vcamSys.create != nullptr;
    }
    return g_vcamSys.api;
}
std::wstring ClsidString(const GUID& g) {
    wchar_t b[64];
    StringFromGUID2(g, b, 64);
    return b;
}
// The component's registered file, if it is registered and there.
std::wstring VCamComponentPath(int cam = 0) {
    const std::wstring key = L"SOFTWARE\\Classes\\CLSID\\" + ClsidString(ilcam::kCamClsid[cam]) + L"\\InprocServer32";
    wchar_t path[MAX_PATH] = {};
    DWORD n = sizeof(path);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &n) != ERROR_SUCCESS) return {};
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES ? std::wstring(path) : std::wstring();
}
bool VCamInstalled() { return !VCamComponentPath().empty(); }

void VCamClose(VCamOut& o) {
    if (o.cam) {
        o.cam->Shutdown();
        o.cam->Release();
        o.cam = nullptr;
    }
    if (o.block) UnmapViewOfFile(o.block);
    if (o.map) CloseHandle(o.map);
    o.block = nullptr;
    o.map = nullptr;
    o.reading = false;
    o.ch.out = nullptr;
    ChannelRelease(o.ch);
}

// Why the cameras are not there - said once (not again every retry).
void VCamError(const std::wstring& e) {
    if (e != g_vcamSys.error) RecStatus(e, 10000);
    g_vcamSys.error = e;
}

// The camera "ILARTS <name>" for this session.
bool VCamCreate(int i) {
    VCamOut& o = g_vcam[i];
    if (o.cam) return true;
    if (!VCamApi()) {
        VCamError(L"Virtual cameras need Windows 11");
        return false;
    }
    if (!VCamInstalled()) {
        VCamError(L"Install the camera component first (SETTINGS)");
        return false;
    }
    if (VCamComponentPath(i).empty()) {  // installed before this camera was added
        VCamError(L"Reinstall the camera component (SETTINGS)");
        return false;
    }
    const std::wstring id = ClsidString(ilcam::kCamClsid[i]);
    HRESULT hr = g_vcamSys.create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser,
                                  ilcam::kCamName[i], id.c_str(), nullptr, 0, &o.cam);
    if (SUCCEEDED(hr)) hr = o.cam->Start(nullptr);
    if (FAILED(hr)) {
        if (o.cam) o.cam->Release();
        o.cam = nullptr;
        wchar_t b[96];
        swprintf_s(b, L"%s: the camera did not start (0x%08lX)", ilcam::kCamName[i], static_cast<unsigned long>(hr));
        VCamError(b);
        return false;
    }
    return true;
}

// Once per tick: cameras made or removed as the settings say; the shared block of each opened once its camera
// made it; whether an application reads it.
void VCamTick() {
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < ilcam::kCams; ++i) {
        VCamOut& o = g_vcam[i];
        if (!o.want) {
            if (o.cam || o.block) VCamClose(o);
            continue;
        }
        if (!o.cam && (now - o.lastOpen > 5000 || !o.lastOpen)) {
            o.lastOpen = now;
            VCamCreate(i);
        }
        if (o.cam && !o.block && now - o.lastOpen > 1000) {  // made by the camera when an application first opens it
            o.lastOpen = now;
            wchar_t name[64];
            for (bool global : {true, false}) {
                ilcam::BlockName(i, name, 64, global);
                if ((o.map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name)) != nullptr) break;
            }
            if (o.map) {
                o.block = static_cast<ilcam::Block*>(MapViewOfFile(o.map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
                if (!o.block) CloseHandle(o.map), o.map = nullptr;
            }
        }
        const bool reading = o.block && o.block->magic == ilcam::kMagic && now - static_cast<ULONGLONG>(o.block->readTick) < 3000;
        if (reading) {
            const int w = std::clamp<int>(o.block->askW, 64, ilcam::kMaxW) & ~1, h = std::clamp<int>(o.block->askH, 64, ilcam::kMaxH) & ~1;
            const auto& lc = o.ch.live.config();
            if (!o.reading || lc.w != w || lc.h != h) {  // (re)start its picture at that size
                ChannelRelease(o.ch);
                live::Live::Config c;
                c.w = w, c.h = h, c.fps = 30;
                o.ch.live.configure(c);
                o.ch.nextCapture = 0;
                ilcam::Block* b = o.block;
                o.ch.out = [b](const std::vector<uint8_t>& px, int pw, int ph) {
                    if (static_cast<size_t>(pw) * ph * 4 > ilcam::kBlockBytes - sizeof(ilcam::Block) || px.size() < static_cast<size_t>(pw) * ph * 4) return;
                    InterlockedIncrement(&b->seq);  // odd: being written
                    MemoryBarrier();
                    memcpy(b + 1, px.data(), static_cast<size_t>(pw) * ph * 4);
                    b->frameW = pw, b->frameH = ph;
                    b->writeTick = static_cast<LONG64>(GetTickCount64());
                    MemoryBarrier();
                    InterlockedIncrement(&b->seq);
                };
            }
        } else if (o.reading) {  // nobody watches: nothing drawn
            o.ch.out = nullptr;
            ChannelRelease(o.ch);
        }
        o.reading = reading;
    }
}

void VCamCapture() {
    for (int i = 0; i < ilcam::kCams; ++i) {
        VCamOut& o = g_vcam[i];
        if (!o.reading || !o.ch.out) continue;
        const std::vector<ViewState> vs = TileViews(OutKeys(i));
        ChannelCaptureWith(o.ch, true, [&](ID3D11RenderTargetView* rtv, int w, int h) { DrawTiles(vs, OutComposite(i), rtv, w, h); });
    }
}

// The settings: [outputs] spout, cameras.
void OutputsApply() {
    wchar_t b[256] = {};
    GetPrivateProfileStringW(L"outputs", L"spout", L"", b, 256, app.iniPath.c_str());
    const auto sp = SplitList(b);
    GetPrivateProfileStringW(L"outputs", L"cameras", L"", b, 256, app.iniPath.c_str());
    const auto vc = SplitList(b);
    GetPrivateProfileStringW(L"outputs", L"tiles", L"a,b", b, 256, app.iniPath.c_str());
    g_outTiles = SplitList(b);
    if (g_outTiles.size() > static_cast<size_t>(kMaxTiles)) g_outTiles.resize(kMaxTiles);
    if (g_outTiles.empty()) g_outTiles = {L"a", L"b"};
    for (int i = 0; i < ilcam::kCams; ++i) {
        const bool s = std::find(sp.begin(), sp.end(), kOutSources[i]) != sp.end();
        if (!s && g_spout[i].want) SpoutRelease(g_spout[i]);
        g_spout[i].want = s;
        g_spout[i].failed = false;
        g_vcam[i].want = std::find(vc.begin(), vc.end(), kOutSources[i]) != vc.end();
        g_vcam[i].lastOpen = 0;
    }
    g_vcamSys.error.clear();
}

void OutputsCapture() {
    SpoutCapture();
    VCamCapture();
}

void OutputsRelease() {  // the device is gone (or the program ends): GPU objects; the cameras stay
    for (auto& o : g_spout) SpoutRelease(o);
    for (auto& o : g_vcam) ChannelRelease(o.ch);
}
void OutputsShutdown() {
    for (auto& o : g_spout) SpoutRelease(o);
    for (auto& o : g_vcam) VCamClose(o);
}

// A source that an output needs rendered (a Spout sender, or a camera an application reads).
bool OutputWants(int s) {
    if (s < 0) return false;
    for (int i = 0; i < ilcam::kCams; ++i)
        if (g_spout[i].want || (g_vcam[i].want && g_vcam[i].reading))
            for (const auto& k : OutKeys(i))
                if (StreamSource(k) == s) return true;
    return false;
}

// ---- the camera component (elevated: ILARTS.exe --install-cameras / --uninstall-cameras)
std::wstring ProgramFilesDir() {
    wchar_t pf[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\DCS ILARTS", pf, MAX_PATH);
    return pf;
}
// Windows' camera service, stopped and started again (it starts on demand too); waits up to 10 s for the stop.
void RestartFrameServer() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    if (SC_HANDLE s = OpenServiceW(scm, L"FrameServer", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS st{};
        if (ControlService(s, SERVICE_CONTROL_STOP, &st))
            for (int i = 0; i < 100 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(100);
        StartServiceW(s, 0, nullptr);
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
}

// Returns the process exit code: 0 done, else a Win32 error / HRESULT.
int CameraComponentInstall(bool install) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    typedef HRESULT(STDAPICALLTYPE * RegFn)();
    const std::wstring dir = ProgramFilesDir(), dst = dir + L"\\ILARTSCam.dll";
    if (!install) {
        if (HMODULE m = LoadLibraryW(dst.c_str())) {
            if (auto f = reinterpret_cast<RegFn>(GetProcAddress(m, "DllUnregisterServer"))) f();
            FreeLibrary(m);
        }
        if (!DeleteFileW(dst.c_str())) MoveFileExW(dst.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);  // in use by the camera service
        RemoveDirectoryW(dir.c_str());
        return 0;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring src = exe;
    src = src.substr(0, src.find_last_of(L"\\/") + 1) + L"ILARTSCam.dll";
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) return ERROR_FILE_NOT_FOUND;
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {  // the old one in use: move it aside (gone at the next restart)
        const std::wstring old = dst + L"." + std::to_wstring(GetTickCount64()) + L".old";
        if (!MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return static_cast<int>(GetLastError());
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) return static_cast<int>(GetLastError());
        RestartFrameServer();  // it keeps running the old one until then
    }
    {  // a folder for the component's log that the camera service may write to
        wchar_t pd[MAX_PATH];
        ExpandEnvironmentStringsW(L"%ProgramData%\\DCS ILARTS", pd, MAX_PATH);
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GRGWGX;;;LS)(A;OICI;GRGWGX;;;BU)",
                                                                 SDDL_REVISION_1, &sd, nullptr))
            sa.lpSecurityDescriptor = sd;
        CreateDirectoryW(pd, &sa);
        if (sd) LocalFree(sd);
    }
    HMODULE m = LoadLibraryW(dst.c_str());
    if (!m) return static_cast<int>(GetLastError());
    auto f = reinterpret_cast<RegFn>(GetProcAddress(m, "DllRegisterServer"));
    const HRESULT hr = f ? f() : E_FAIL;
    FreeLibrary(m);
    return FAILED(hr) ? static_cast<int>(hr) : 0;
}

// The camera service's process (0: not running).
DWORD FrameServerPid() {
    DWORD pid = 0;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 0;
    if (SC_HANDLE s = OpenServiceW(scm, L"FrameServer", SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS_PROCESS st{};
        DWORD n = 0;
        if (QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&st), sizeof(st), &n)) pid = st.dwProcessId;
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return pid;
}

// Runs this program elevated to install or remove the component (Windows asks the user).
void CameraComponentRun(bool install) {
    if (g_vcamSys.installer) return;
    g_vcamSys.serverPid = FrameServerPid();
    for (auto& o : g_vcam) VCamClose(o);  // made again when it is done (a reinstall restarts the camera service)
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW si{sizeof(si)};
    si.fMask = SEE_MASK_NOCLOSEPROCESS;
    si.hwnd = app.hwnd;
    si.lpVerb = L"runas";
    si.lpFile = exe;
    si.lpParameters = install ? L"--install-cameras" : L"--uninstall-cameras";
    si.nShow = SW_HIDE;
    if (!ShellExecuteExW(&si) || !si.hProcess) {
        RecStatus(install ? L"Camera component not installed" : L"Camera component not removed", 8000);
        return;
    }
    g_vcamSys.installer = si.hProcess;
}
// Done: says so, and the cameras are made again - by a new process of this program if the camera service was
// restarted meanwhile (this one can no longer make cameras: MFCreateVirtualCamera fails until it ends).
bool CameraComponentPoll() {
    if (!g_vcamSys.installer || WaitForSingleObject(g_vcamSys.installer, 0) != WAIT_OBJECT_0) return false;
    DWORD code = 1;
    GetExitCodeProcess(g_vcamSys.installer, &code);
    CloseHandle(g_vcamSys.installer);
    g_vcamSys.installer = nullptr;
    const bool now = VCamInstalled();
    if (!code && now && g_vcamSys.serverPid && FrameServerPid() != g_vcamSys.serverPid) {
        RestartSelf();
        return true;
    }
    wchar_t b[96];
    if (code) swprintf_s(b, L"Camera component: failed (0x%08lX)", code);
    RecStatus(code ? std::wstring(b) : now ? L"Camera component installed" : L"Camera component removed", 8000);
    for (auto& o : g_vcam) o.lastOpen = 0;
    g_vcamSys.error.clear();
    ++g_con.formRev;  // the settings dialog's key: INSTALL / REMOVE
    return true;
}
