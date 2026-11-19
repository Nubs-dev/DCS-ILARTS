// The console interface (docs/HANDOFF-ILARTS.md) of the desktop application. Included by lso_viewer.cpp inside
// its anonymous namespace, before MainProc; the state it keeps (g_con) is declared near the top of that file.
//
// The interface is the web component ui/ilarts-console.js, the same one a web page can host.
// Here it runs in a WebView2 composed OVER the Direct3D picture with DirectComposition ("visual hosting"):
//
//   main window (frameless, no visible child windows)
//     DirectComposition target
//       root visual
//         video visual: the swap chain, window sized; the pictures are drawn where the page's monitors are
//         web visual:   the WebView2, transparent where the page is cut open (the monitor screens)
//
// The page (ui/index.html) reports where its monitors and its title bar are ({cmd:'layout'}); it gets the
// application's state as one JSON object (ConsoleState) whenever that changes, and sends commands
// ({cmd:'source', id:'island'} ...) that call the same functions as the keys. Visual hosting gives the WebView no
// window of its own for the mouse, so the window's mouse messages are passed to it (ConsoleMouse). Keys reach
// the application three ways: the window's own WM_KEYDOWN (message loop), WebView2's accelerator keys (keys
// with Ctrl / Alt, function keys, arrows, Esc) and the page, which passes on the rest ({cmd:'key'}).
// The console is the only interface. Without WebView2 ILARTS says so and offers the download page; if WebView2
// fails while running, ILARTS says so and closes.

constexpr wchar_t kUiHost[] = L"ilarts.example";  // the ui folder, served to the WebView as https://ilarts.example/

// One COM object per WebView2 callback.
template <typename I, typename... A>
struct ComHandler : I {
    std::function<HRESULT(A...)> fn;
    ULONG refs = 1;
    explicit ComHandler(std::function<HRESULT(A...)> f) : fn(std::move(f)) {}
    virtual ~ComHandler() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(I)) {
            *out = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(A... a) override { return fn(a...); }
};

// Registers an event handler (the WebView keeps its own reference).
template <typename I, typename... A, typename Add>
void OnEvent(Add add, std::function<HRESULT(A...)> fn) {
    auto* h = new ComHandler<I, A...>(std::move(fn));
    EventRegistrationToken tok{};
    add(h, &tok);
    h->Release();
}

// ---- small JSON helpers (the page's messages are flat objects written by JSON.stringify)
std::wstring JsonStr(const std::wstring& j, const wchar_t* key) {
    const std::wstring k = std::wstring(L"\"") + key + L"\":\"";
    const size_t a = j.find(k);
    if (a == std::wstring::npos) return {};
    std::wstring out;
    for (size_t i = a + k.size(); i < j.size() && j[i] != L'"'; ++i) {
        if (j[i] != L'\\' || i + 1 >= j.size()) {
            out += j[i];
            continue;
        }
        const wchar_t e = j[++i];
        if (e == L'u' && i + 4 < j.size()) {
            out += static_cast<wchar_t>(wcstoul(j.substr(i + 1, 4).c_str(), nullptr, 16));
            i += 4;
        } else {
            out += e == L'n' ? L'\n' : e == L't' ? L'\t' : e == L'r' ? L'\r' : e;
        }
    }
    return out;
}
bool JsonNum(const std::wstring& j, const wchar_t* key, double* v) {
    const std::wstring k = std::wstring(L"\"") + key + L"\":";
    const size_t a = j.find(k);
    if (a == std::wstring::npos) return false;
    const wchar_t* p = j.c_str() + a + k.size();
    if (wcsncmp(p, L"true", 4) == 0) return *v = 1, true;
    if (wcsncmp(p, L"false", 5) == 0) return *v = 0, true;
    wchar_t* end = nullptr;
    *v = wcstod(p, &end);
    return end != p;
}
std::wstring JsonEsc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') o += L'\\', o += c;
        else if (c == L'\n') o += L"\\n";  // line breaks reach the page (tooltips, the monitors' messages)
        else if (c < 32) o += L' ';
        else o += c;
    }
    return o;
}

// Builds the state object: str / num / flag add "key":value, open / close nest objects and arrays.
struct Json {
    std::wstring j;
    void sep() {
        if (!j.empty() && j.back() != L'{' && j.back() != L'[') j += L',';
    }
    void key(const wchar_t* k) {
        sep();
        if (k) j += std::wstring(L"\"") + k + L"\":";
    }
    void str(const wchar_t* k, const std::wstring& v) { key(k), j += L"\"" + JsonEsc(v) + L"\""; }
    void num(const wchar_t* k, double v) {
        key(k);
        wchar_t b[48];
        swprintf_s(b, L"%.5g", std::isfinite(v) ? v : 0.0);
        j += b;
    }
    void flag(const wchar_t* k, bool v) { key(k), j += v ? L"true" : L"false"; }
    void null(const wchar_t* k) { key(k), j += L"null"; }
    void open(const wchar_t* k, wchar_t c = L'{') { key(k), j += c; }
    void close(wchar_t c = L'}') { j += c; }
};

std::wstring Upper(std::wstring t) {
    for (auto& c : t) c = static_cast<wchar_t>(towupper(c));
    return t;
}

#include "tuning.inc"  // the camera tuning panel (SETTINGS > Developer; development builds only)
std::wstring Hms(double sec) {
    const int t = static_cast<int>(std::max(0.0, sec));
    wchar_t b[24];
    swprintf_s(b, L"%02d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    return b;
}

// ---------------------------------------------------------------- settings dialog (the page's SETTINGS)
//
// Every setting of ILARTS.ini that has no control on the desk, edited in the window. The page gets the table as
// state.settingsForm while the dialog is open and sends back {cmd:'settingsSave', values:{'video.fps': 30 ...}}.
// The file stays the store (and is there for backup and sharing: OPEN FILE, EXPORT, IMPORT).
// type: text, password, number, percent (number shown x100), bool, select, path. restart = read at start only. hi of a text field: its longest.
struct SettingDef {
    const wchar_t *group, *section, *key, *label, *type, *def;
    double lo, hi, step;
    const wchar_t *unit, *options, *hint;
    bool restart;
};
const SettingDef kSettings[] = {
    {L"Recording", L"video", L"folder", L"Folder", L"path", L"", 0, 0, 0, L"", L"", L"Recordings, replays, black box files and stills, in a folder per mission", false},
    {L"Recording", L"video", L"stills_folder", L"Stills folder", L"path", L"", 0, 0, 0, L"", L"", L"Empty: a Stills folder in each mission's folder", false},
    {L"Recording", L"video", L"storage_limit", L"Storage limit", L"bool", L"0", 0, 0, 0, L"", L"",
     L"Deletes the oldest recordings and stills ILARTS made when new ones would take the folder over the size below", false},
    {L"Recording", L"video", L"storage_limit_gb", L"Maximum size", L"number", L"50", 1, 100000, 1, L"GB", L"", L"The recordings folder with its subfolders (and the stills folder)", false},
    // options of a text field: its parameters ("token|what it stands for"), listed under it to insert
    {L"Recording", L"video", L"filename", L"File name", L"text", L"{date} {time} {callsign} Case {case} {grade}", 0, 0, 0, L"",
     L"{date}|Date (YYYY-MM-DD);{time}|Time (HH-MM-SS);{callsign}|Callsign (else the pilot);{case}|Recovery case (I, II, III);"
     L"{grade}|LSO grade and wire (else the result);{result}|TRAP, BOLTER, WAVE-OFF;{wire}|Wire caught;{board}|Board number;"
     L"{type}|Aircraft type;{pilot}|Pilot;{carrier}|Carrier;{pass}|Pass number",
     L"The source is added at the end (PLAT, ISLAND ...)", false},
    {L"Recording", L"video", L"preroll", L"Before the pass", L"number", L"3", 0, 20, 0.5, L"s", L"", L"", false},
    {L"Recording", L"video", L"postroll", L"After the pass", L"number", L"4", 0, 30, 0.5, L"s", L"", L"", false},
    {L"Recording", L"video", L"width", L"Picture width", L"number", L"1024", 320, 3840, 16, L"px", L"", L"", true},
    {L"Recording", L"video", L"height", L"Picture height", L"number", L"768", 240, 2160, 16, L"px", L"", L"", true},
    {L"Recording", L"video", L"fps", L"Frame rate", L"number", L"30", 10, 60, 1, L"fps", L"", L"", true},
    {L"Recording", L"video", L"bitrate_kbps", L"Bit rate", L"number", L"0", 0, 100000, 100, L"kbit/s", L"", L"0 = automatic", true},
    {L"Recording", L"video", L"encoder", L"Encoder", L"select", L"auto", 0, 0, 0, L"", L"auto|Automatic;hardware|Graphics card;software|Software", L"", true},
    {L"Replay", L"video", L"replay_seconds", L"Replay buffer", L"number", L"45", 10, 300, 5, L"s", L"", L"", true},
    {L"Replay", L"video", L"replay_default", L"Without a pass", L"number", L"30", 5, 300, 5, L"s", L"", L"How far back PLAY goes when no pass is in the buffer", false},
    {L"Replay", L"video", L"quality", L"Picture quality", L"number", L"85", 30, 100, 5, L"%", L"", L"", true},
    {L"CCTV look", L"cctv", L"on", L"CCTV look", L"bool", L"1", 0, 0, 0, L"", L"", L"Scan lines, noise, lens distortion and vignette, as on the ship's monitors (key C)", false},
    {L"CCTV look", L"cctv", L"strength", L"Strength", L"percent", L"1", 0, 200, 5, L"%", L"", L"", false},
    {L"CCTV look", L"cctv", L"recordings_only", L"Apply to recordings only", L"bool", L"0", 0, 0, 0, L"", L"", L"", false},
    {L"CCTV look", L"cctv", L"grain", L"Noise", L"number", L"1", 0, 4, 0.1, L"\x00D7", L"", L"", false},
    {L"CCTV look", L"cctv", L"scanlines", L"Scan lines", L"number", L"1", 0, 4, 0.1, L"\x00D7", L"", L"", false},
    {L"CCTV look", L"cctv", L"distortion", L"Lens distortion", L"number", L"1", 0, 4, 0.1, L"\x00D7", L"", L"", false},
    {L"CCTV look", L"cctv", L"vignette", L"Vignette", L"number", L"1", 0, 4, 0.1, L"\x00D7", L"", L"", false},
    {L"Black box", L"blackbox", L"minutes", L"Keeps", L"number", L"5", 1, 30, 1, L"min", L"", L"", false},
    {L"Black box", L"blackbox", L"mishap_before", L"Before a mishap", L"number", L"60", 5, 1800, 5, L"s", L"", L"", false},
    {L"Black box", L"blackbox", L"mishap_after", L"After a mishap", L"number", L"15", 0, 120, 5, L"s", L"", L"", false},
    // streams: what they share, then each target ([stream] = 1, [stream2] ...; options of a multi field: its choices)
    {L"Stream", L"stream", L"autostart", L"Start automatically", L"bool", L"0", 0, 0, 0, L"", L"", L"Once DCS is connected and a picture is ready", false},
    {L"Stream", L"stream", L"bitrate_kbps", L"Bit rate", L"number", L"2500", 300, 20000, 100, L"kbit/s", L"", L"", true},
    {L"Stream", L"stream", L"composite_kbps", L"Composite bit rate", L"number", L"6000", 1000, 30000, 100, L"kbit/s", L"", L"1920 \x00D7 1080", false},
    {L"Stream", L"stream", L"audio", L"Silent audio track", L"bool", L"1", 0, 0, 0, L"", L"", L"Some services refuse video without audio", false},
    {L"Stream 1", L"stream", L"label", L"Label", L"text", L"", 0, 6, 0, L"", L"", L"", false},
    {L"Stream 1", L"stream", L"url", L"Server", L"text", L"", 0, 0, 0, L"", L"", L"rtmp://server/app (plain RTMP)", false},
    {L"Stream 1", L"stream", L"key", L"Stream key", L"password", L"", 0, 0, 0, L"", L"", L"", false},
    {L"Stream 1", L"stream", L"source", L"Source", L"select", L"a", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;composite|Composite;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"", false},
    {L"Stream 1", L"stream", L"tiles", L"Composite", L"multi", L"a,b", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"Up to four, arranged by their number", false},
    {L"Stream 2", L"stream2", L"label", L"Label", L"text", L"", 0, 6, 0, L"", L"", L"", false},
    {L"Stream 2", L"stream2", L"url", L"Server", L"text", L"", 0, 0, 0, L"", L"", L"rtmp://server/app (plain RTMP)", false},
    {L"Stream 2", L"stream2", L"key", L"Stream key", L"password", L"", 0, 0, 0, L"", L"", L"", false},
    {L"Stream 2", L"stream2", L"source", L"Source", L"select", L"a", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;composite|Composite;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"", false},
    {L"Stream 2", L"stream2", L"tiles", L"Composite", L"multi", L"a,b", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"Up to four, arranged by their number", false},
    {L"Stream 3", L"stream3", L"label", L"Label", L"text", L"", 0, 6, 0, L"", L"", L"", false},
    {L"Stream 3", L"stream3", L"url", L"Server", L"text", L"", 0, 0, 0, L"", L"", L"rtmp://server/app (plain RTMP)", false},
    {L"Stream 3", L"stream3", L"key", L"Stream key", L"password", L"", 0, 0, 0, L"", L"", L"", false},
    {L"Stream 3", L"stream3", L"source", L"Source", L"select", L"a", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;composite|Composite;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"", false},
    {L"Stream 3", L"stream3", L"tiles", L"Composite", L"multi", L"a,b", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"Up to four, arranged by their number", false},
    {L"Stream 4", L"stream4", L"label", L"Label", L"text", L"", 0, 6, 0, L"", L"", L"", false},
    {L"Stream 4", L"stream4", L"url", L"Server", L"text", L"", 0, 0, 0, L"", L"", L"rtmp://server/app (plain RTMP)", false},
    {L"Stream 4", L"stream4", L"key", L"Stream key", L"password", L"", 0, 0, 0, L"", L"", L"", false},
    {L"Stream 4", L"stream4", L"source", L"Source", L"select", L"a", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;composite|Composite;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"", false},
    {L"Stream 4", L"stream4", L"tiles", L"Composite", L"multi", L"a,b", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"Up to four, arranged by their number", false},
    // outputs: Spout senders and virtual cameras (outputs.inc); "action": a key (value "id|LABEL")
    {L"Outputs", L"outputs", L"spout", L"Spout", L"multi", L"", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island;composite|Composite", L"For OBS (Spout2 plugin) and other Spout receivers", false},
    {L"Outputs", L"outputs", L"cameras", L"Virtual cameras", L"multi", L"", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island;composite|Composite", L"Webcams for Discord, Teams, OBS ... (Windows 11)", false},
    {L"Outputs", L"outputs", L"tiles", L"Composite", L"multi", L"a,b", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"Up to four, arranged by their number", false},
    {L"Outputs", L"outputs", L"component", L"Camera component", L"action", L"", 0, 0, 0, L"", L"", L"Installs the virtual cameras for every user (administrator)", false},
    // benchmark (bench.inc): armed for the next mission start, switched off again when the run starts
    {L"Benchmark", L"benchmark", L"next_mission", L"Benchmark next mission", L"bool", L"0", 0, 0, 0, L"", L"",
     L"At the next mission start, about 6 minutes: DCS with the hook alone, with PLAT, LSO 1 and LSO 2, then 1 to 5 camera views with recording off and on (nothing is saved). Leave the mission to stop it", false},
    {L"Benchmark", L"benchmark", L"last", L"Last report", L"action", L"", 0, 0, 0, L"", L"", L"", false},
    {L"Benchmark", L"benchmark", L"folder", L"Reports", L"action", L"", 0, 0, 0, L"", L"", L"%LOCALAPPDATA%\\ILARTS\\benchmarks", false},
    // "info": a value shown, not edited or saved
    {L"Installation", L"install", L"version", L"Version", L"info", L"", 0, 0, 0, L"", L"", L"", false},
    {L"Installation", L"install", L"repair", L"Check and repair", L"action", L"", 0, 0, 0, L"", L"", L"After a DCS update or if something stopped working: restores the hook and permissions, and checks the DCS version", false},
    {L"Installation", L"install", L"permissions", L"DCS permissions", L"action", L"", 0, 0, 0, L"", L"", L"The two lines in Saved Games\\DCS\\Config\\autoexec.cfg the hook needs (single player, hosting). REMOVE before you uninstall with OMM or OvGME", false},
    {L"Installation", L"install", L"logs", L"Logs", L"action", L"", 0, 0, 0, L"", L"", L"ILARTS.log, in %LOCALAPPDATA%\\ILARTS", false},
    {L"Experimental", L"experimental", L"flir", L"FLIR view", L"bool", L"0", 0, 0, 0, L"", L"",
     L"A FLIR key on each camera's PTZ panel: DCS's infrared picture for that camera. Experimental, and costly: a camera in FLIR can cut DCS's frame rate considerably", false},
    {L"Developer", L"developer", L"tuning", L"Camera tuning", L"bool", L"0", 0, 0, 0, L"", L"",
     L"A TUNE key on each camera's PTZ panel: moves the camera in DCS and saves its position in Saved Games\\DCS\\Config\\ILARTS-cameras.lua", false},
    {L"Window", L"viewer", L"auto_start", L"Start with DCS", L"bool", L"1", 0, 0, 0, L"", L"", L"Opens ILARTS when a mission starts, if it is not open already", false},
    {L"Window", L"viewer", L"antialias", L"Anti-aliasing", L"bool", L"1", 0, 0, 0, L"", L"", L"", false},
};

std::wstring IniGet(const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    wchar_t b[1024] = {};
    GetPrivateProfileStringW(section, key, def, b, 1024, app.iniPath.c_str());
    return b;
}

bool AutoexecHasOurs();     // repair.inc
std::wstring BenchLastReport();  // bench.inc
std::wstring BenchStatus();
std::wstring BenchLamp(std::wstring* tip);
void BenchLampClick();

// A settings panel's info marker (the "i" beside its name plate): its tooltip.
const wchar_t* SectionTip(const wchar_t* group) {
    if (!wcscmp(group, L"Benchmark"))
        return L"How to run a benchmark\n"
               L"\x00B7 Load Missions\\ILARTS Benchmark.miz, a calm, repeatable scene: parked on deck in the cockpit, not flying.\n"
               L"\x00B7 Keep DCS in front. Do not touch the controls, change the view or pause.\n"
               L"\x00B7 It starts 10 s after the mission runs and takes about 6 minutes.\n"
               L"\x00B7 Compare each stage with Hook only. Runs differ by a few percent.";
    return nullptr;
}

// The user guide: the PDF the packages install (Mods\tech\ILARTS, one folder above bin), a development build's
// (the repository's docs folder, two above build\Release), else the one on GitHub.
constexpr wchar_t kGuideUrl[] = L"https://github.com/Nubs-dev/DCS-ILARTS/blob/main/docs/DCS-ILARTS-Guide.pdf";
std::wstring GuidePath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    for (const wchar_t* rel : {L"..\\DCS-ILARTS-Guide.pdf", L"..\\..\\docs\\DCS-ILARTS-Guide.pdf", L"DCS-ILARTS-Guide.pdf"}) {
        wchar_t full[MAX_PATH];
        if (GetFullPathNameW((dir + rel).c_str(), MAX_PATH, full, nullptr) && GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) return full;
    }
    return {};
}
void OpenGuide() {
    const std::wstring p = GuidePath(), target = p.empty() ? std::wstring(kGuideUrl) : p;
    TestLog(L"guide: " + target);
    if (!g_test.offscreen) ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
bool AutoexecAllowsNow();  // repair.inc
std::wstring SettingValue(const SettingDef& d) {
    if (!wcscmp(d.section, L"install") && !wcscmp(d.key, L"version"))  // 0.0.0: built without -DILARTS_VERSION
        return wcscmp(ILARTS_VERSION_W, L"0.0.0") ? ILARTS_VERSION_W : L"DEVELOPMENT";
    if (!wcscmp(d.section, L"install") && !wcscmp(d.key, L"repair")) return L"repair|REPAIR";
    if (!wcscmp(d.section, L"install") && !wcscmp(d.key, L"logs")) return L"logs|OPEN";
    if (!wcscmp(d.section, L"benchmark") && !wcscmp(d.key, L"last")) return BenchLastReport().empty() ? L"none|\x2014" : L"bench_last|OPEN";
    if (!wcscmp(d.section, L"benchmark") && !wcscmp(d.key, L"folder")) return L"bench_folder|OPEN";
    if (!wcscmp(d.section, L"install") && !wcscmp(d.key, L"permissions"))  // allowed by lines ILARTS did not write: nothing to add or remove
        return AutoexecHasOurs() ? L"perm_remove|REMOVE" : AutoexecAllowsNow() ? L"none|ALLOWED" : L"perm_add|ADD";
    if (!wcscmp(d.section, L"outputs") && !wcscmp(d.key, L"component"))
        return g_vcamSys.installer ? L"busy|\x2026" : !VCamApi() ? L"none|WINDOWS 11" : VCamInstalled() ? L"cam_remove|REMOVE" : L"cam_install|INSTALL";
    if (!wcscmp(d.section, L"video") && !wcscmp(d.key, L"folder")) return VideoFolder();
    if (!wcscmp(d.section, L"video") && !wcscmp(d.key, L"stills_folder")) return CustomStillsFolder();
    return IniGet(d.section, d.key, d.def);
}

// The dialog's table for the page.
void SettingsForm(Json& s) {
    s.open(L"settingsForm");
    s.num(L"rev", g_con.formRev);
    s.str(L"file", app.iniPath);
    s.str(L"message", g_con.formMessage);
    s.flag(L"restart", g_con.formRestart);
    s.open(L"sections", L'[');
    const wchar_t* group = nullptr;
    for (const auto& d : kSettings) {
        if (!wcscmp(d.section, L"developer") && !DevBuild()) continue;  // the Developer settings: development builds only
        if (!group || wcscmp(group, d.group) != 0) {
            if (group) s.close(L']'), s.close();
            group = d.group;
            s.open(nullptr);
            s.str(L"title", d.group);
            if (const wchar_t* tip = SectionTip(d.group)) s.str(L"tip", tip);
            s.open(L"fields", L'[');
        }
        const std::wstring v = SettingValue(d), type = d.type;
        s.open(nullptr);
        s.str(L"key", std::wstring(d.section) + L"." + d.key);
        s.str(L"label", d.label);
        s.str(L"type", type == L"percent" ? L"number" : type);
        if (type == L"bool") s.flag(L"value", _wtof(v.c_str()) > 0.5);
        else if (type == L"number") s.num(L"value", _wtof(v.c_str()));
        else if (type == L"percent") s.num(L"value", std::round(_wtof(v.c_str()) * 100));
        else s.str(L"value", v);
        if (!wcsncmp(d.section, L"stream", 6) && wcscmp(d.group, L"Stream")) s.str(L"default", d.def);  // a stream target's: CLEAR
        if (type == L"number" || type == L"percent") s.num(L"min", d.lo), s.num(L"max", d.hi), s.num(L"step", d.step);
        else if (type == L"text" && d.hi > 0) s.num(L"max", d.hi);  // its length
        if (d.unit[0]) s.str(L"unit", d.unit);
        if (d.hint[0]) s.str(L"hint", d.hint);
        if (d.restart) s.flag(L"restart", true);
        if (d.options[0]) {
            s.open(L"options", L'[');
            std::wstring o = d.options;
            for (size_t a = 0; a < o.size();) {
                size_t e = o.find(L';', a);
                if (e == std::wstring::npos) e = o.size();
                const std::wstring item = o.substr(a, e - a);
                const size_t bar = item.find(L'|');
                s.open(nullptr, L'[');
                s.str(nullptr, item.substr(0, bar));
                s.str(nullptr, bar == std::wstring::npos ? item : item.substr(bar + 1));
                s.close(L']');
                a = e + 1;
            }
            s.close(L']');
        }
        s.close();
    }
    if (group) s.close(L']'), s.close();
    s.open(nullptr);  // keyboard shortcuts (type key: the code; the page names it and records a new one)
    s.str(L"title", L"Keyboard");
    s.open(L"fields", L'[');
    for (int i = 0; i < K_COUNT; ++i) {
        s.open(nullptr);
        s.str(L"key", std::wstring(L"keys.") + kKeyActions[i].id);
        s.str(L"label", kKeyActions[i].label);
        s.str(L"type", L"key");
        s.num(L"value", g_keyCode[i]);
        s.close();
    }
    s.close(L']');
    s.close();
    s.close(L']');
    s.close();
}

// What the program reads from the ini at any time (everything but the restart-only settings).
void ApplyLiveSettings() {
    auto num = [](const wchar_t* sec, const wchar_t* key, double def) {
        const std::wstring v = IniGet(sec, key, L"");
        return v.empty() ? def : _wtof(v.c_str());
    };
    g_cctv.on = num(L"cctv", L"on", 1) > 0.5;  // on unless switched off
    g_cctv.strength = static_cast<float>(std::clamp(num(L"cctv", L"strength", 1), 0.0, 2.0));
    g_cctv.recOnly = num(L"cctv", L"recordings_only", 0) > 0.5;
    g_cctv.grain = static_cast<float>(std::clamp(num(L"cctv", L"grain", 1), 0.0, 4.0));
    g_cctv.scan = static_cast<float>(std::clamp(num(L"cctv", L"scanlines", 1), 0.0, 4.0));
    g_cctv.distort = static_cast<float>(std::clamp(num(L"cctv", L"distortion", 1), 0.0, 4.0));
    g_cctv.vignette = static_cast<float>(std::clamp(num(L"cctv", L"vignette", 1), 0.0, 4.0));
    g_antialias = num(L"viewer", L"antialias", 1) > 0.5;
    g_expFlir = num(L"experimental", L"flir", 0) > 0.5;
    TuneSettings();
    if (!g_expFlir) std::fill(std::begin(g_flirOn), std::end(g_flirOn), false);
    g_bb.minutes = std::clamp(num(L"blackbox", L"minutes", 5), 1.0, 30.0);
    g_bb.before = std::clamp(num(L"blackbox", L"mishap_before", 60), 5.0, g_bb.minutes * 60);
    g_bb.after = std::clamp(num(L"blackbox", L"mishap_after", 15), 0.0, 120.0);
    g_streamAudio = num(L"stream", L"audio", 1) > 0.5;
    g_compositeBitrate = static_cast<int>(std::clamp(num(L"stream", L"composite_kbps", 6000), 1000.0, 30000.0) * 1000);
    g_rec.preroll = std::clamp(num(L"video", L"preroll", 3), 0.0, 20.0);
    g_rec.postroll = std::clamp(num(L"video", L"postroll", 4), 0.0, 30.0);
    if (g_rec.ok) g_rec.replayDefault = std::clamp(num(L"video", L"replay_default", 30), 5.0, g_rec.main.eng.config().ringSeconds);
    g_rec.nameFormat = IniGet(L"video", L"filename", L"{date} {time} {callsign} Case {case} {grade}");
    g_rec.folder.clear();  // VideoFolder() reads it again
    StorageReadSettings();
    LiveApply();
    OutputsApply();
}

// A new process of this program that waits for this one to end (settings that are read at start).
void RestartSelf() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --wait-pid " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        RecStatus(L"Could not restart ILARTS", 8000);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
}

// The settings the page sent: written to the ini, applied. Restart-only settings that changed are reported.
void SettingsSave(const std::wstring& j) {
    std::wstring restartList;
    for (const auto& d : kSettings) {
        const std::wstring k = std::wstring(L"\"") + d.section + L"." + d.key + L"\":";
        const size_t at = j.find(k);
        if (at == std::wstring::npos) continue;
        const std::wstring type = d.type, before = SettingValue(d);
        std::wstring v;
        if (type == L"text" || type == L"password" || type == L"select" || type == L"path" || type == L"multi") {
            v = JsonStr(j.substr(at), (std::wstring(d.section) + L"." + d.key).c_str());
            if (type == L"text" && d.hi > 0 && v.size() > static_cast<size_t>(d.hi)) v.resize(static_cast<size_t>(d.hi));
        } else {
            double n = 0;
            if (!JsonNum(j.substr(at), (std::wstring(d.section) + L"." + d.key).c_str(), &n)) continue;
            if (type == L"bool") v = n > 0.5 ? L"1" : L"0";
            else {
                if (type == L"percent") n /= 100;
                const double lo = type == L"percent" ? d.lo / 100 : d.lo, hi = type == L"percent" ? d.hi / 100 : d.hi;
                if (hi > lo) n = std::clamp(n, lo, hi);
                wchar_t b[48];
                swprintf_s(b, L"%g", n);
                v = b;
            }
        }
        if (!wcscmp(d.key, L"folder") && !wcscmp(d.section, L"video") && v == VideoFolder()) continue;  // unchanged default
        WritePrivateProfileStringW(d.section, d.key, v.c_str(), app.iniPath.c_str());
        const bool textual = type == L"text" || type == L"password" || type == L"select" || type == L"path" || type == L"multi";
        const bool changed = textual ? v != before : std::fabs(_wtof(v.c_str()) - _wtof(before.c_str())) > 1e-9;
        if (d.restart && changed) restartList += (restartList.empty() ? L"" : L", ") + std::wstring(d.label);
    }
    for (int i = 0; i < K_COUNT; ++i) {  // keyboard shortcuts
        const std::wstring k = std::wstring(L"keys.") + kKeyActions[i].id;
        const size_t at = j.find(L"\"" + k + L"\":");
        double n = 0;
        if (at == std::wstring::npos || !JsonNum(j.substr(at), k.c_str(), &n)) continue;
        WritePrivateProfileStringW(L"keys", kKeyActions[i].id, std::to_wstring(static_cast<int>(n)).c_str(), app.iniPath.c_str());
    }
    LoadKeys();
    RegisterGlobalKeys();
    ApplyLiveSettings();
    ++g_con.formRev;  // the dialog re-reads the stored values
    g_con.formRestart = !restartList.empty();
    g_con.formMessage = restartList.empty() ? L"" : L"Saved. Restart ILARTS to apply: " + restartList;
    Flash(L"Settings saved");
    if (restartList.empty()) g_con.settingsOpen = false;  // nothing more to do: close
}

// File dialogs (folder picker, export, import). Empty = cancelled.
std::wstring FileDialog(bool save, bool folder, const wchar_t* title, const wchar_t* defName) {
    IFileDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) return {};
    DWORD opt = 0;
    fd->GetOptions(&opt);
    fd->SetOptions(opt | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    fd->SetTitle(title);
    if (!folder) {
        const COMDLG_FILTERSPEC spec[] = {{L"ILARTS settings (*.ini)", L"*.ini"}, {L"All files", L"*.*"}};
        fd->SetFileTypes(2, spec);
        fd->SetDefaultExtension(L"ini");
        if (defName) fd->SetFileName(defName);
    }
    std::wstring out;
    if (SUCCEEDED(fd->Show(app.hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(fd->GetResult(&it)) && it) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                out = p;
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    fd->Release();
    return out;
}

std::wstring RepairNow();  // repair.inc
void BenchOpenFolder();    // bench.inc
void BenchOpenLast();
std::wstring PermissionsNow(bool add);  // repair.inc
void SettingsAction(const std::wstring& id, const std::wstring& key) {
    if (id == L"openFile") {
        OpenSettingsFile();
    } else if (id == L"browse" && (key == L"video.folder" || key == L"video.stills_folder")) {
        const std::wstring f = FileDialog(false, true, key == L"video.folder" ? L"Folder for recordings" : L"Folder for stills", nullptr);
        if (!f.empty() && g_con.web) {
            Json a;
            a.open(nullptr);
            a.open(L"field");
            a.str(L"key", key);
            a.str(L"value", f);
            a.close();
            a.close();
            g_con.web->PostWebMessageAsJson(a.j.c_str());
        }
    } else if (id == L"repair") {
        g_con.formMessage = RepairNow();
        g_con.formRestart = false;
        ++g_con.formRev;
    } else if (id == L"perm_add" || id == L"perm_remove") {
        g_con.formMessage = PermissionsNow(id == L"perm_add");
        g_con.formRestart = false;
        ++g_con.formRev;
    } else if (id == L"logs") {
        OpenLogsFolder();
    } else if (id == L"bench_folder") {
        BenchOpenFolder();
    } else if (id == L"bench_last") {
        BenchOpenLast();
    } else if (id == L"cam_install" || id == L"cam_remove") {
        CameraComponentRun(id == L"cam_install");
        ++g_con.formRev;  // the key shows it is busy
    } else if (id == L"export") {
        const std::wstring f = FileDialog(true, false, L"Export the settings", L"ILARTS.ini");
        if (!f.empty()) Flash(CopyFileW(app.iniPath.c_str(), f.c_str(), FALSE) ? L"Settings exported" : L"Export failed");
    } else if (id == L"import") {
        const std::wstring f = FileDialog(false, false, L"Import settings (ILARTS restarts)", nullptr);
        if (f.empty()) return;
        CopyFileW(app.iniPath.c_str(), (app.iniPath + L".bak").c_str(), FALSE);  // the old file, just in case
        if (CopyFileW(f.c_str(), app.iniPath.c_str(), FALSE)) RestartSelf();
        else Flash(L"Import failed");
    } else if (id == L"restart") {
        RestartSelf();
    }
}

// ---- REC switches (one per source)
void SetRecArm(int s, bool on) {
    if (s < 0 || s >= FeedCount()) return;
    const std::wstring k = RecKey(s);  // (the deck positions: one switch, record_deck)
    g_recArm[k] = on;
    WritePrivateProfileStringW(L"video", (L"record_" + k).c_str(), on ? L"1" : L"0", app.iniPath.c_str());
    CameraReconcile();  // a fixed camera renders while it is recorded
    RecStatus((IsDeckSource(s) ? std::wstring(L"Deck cameras") : SourceTag(s)) + (on ? L" will be recorded" : L" will not be recorded"));
}

// ---------------------------------------------------------------- WebView2 and DirectComposition

void ConsoleCommand(const std::wstring& j);

// Console events: the test log when there is one, the debugger otherwise.
void ConsoleLog(const std::wstring& line) {
    if (!g_test.log.empty()) TestLog(line);
    OutputDebugStringW((L"ILARTS " + line + L"\n").c_str());
}

bool ConsoleUiFolder(std::wstring* dir) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d = exe;
    d.resize(d.find_last_of(L"\\/"));
    d += L"\\ui";
    if (GetFileAttributesW((d + L"\\index.html").c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    if (dir) *dir = d;
    return true;
}

// True when this PC can show the console: the WebView2 runtime is installed and the ui folder is there.
// --test-no-webview2: as if the runtime were missing (tests the notice).
bool ConsoleAvailable() {
    LPWSTR ver = nullptr;
    const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver);
    const bool ok = SUCCEEDED(hr) && ver && ver[0] && !g_test.noWebView2;
    if (ver) CoTaskMemFree(ver);
    return ok && ConsoleUiFolder(nullptr);
}

void ConsoleFail(const wchar_t* why, HRESULT hr) {
    wchar_t b[160];
    swprintf_s(b, L"console: %s (0x%08lX)", why, static_cast<unsigned long>(hr));
    ConsoleLog(b);
    g_con.failed = true;  // the loop says so and closes ILARTS
}

// The WebView's controller is ready: settings, events, the page.
HRESULT ConsoleControllerReady(ICoreWebView2CompositionController* cc, const std::wstring& ui) {
    g_con.comp = cc;
    cc->AddRef();
    cc->QueryInterface(IID_PPV_ARGS(&g_con.controller));
    if (!g_con.controller || FAILED(g_con.controller->get_CoreWebView2(&g_con.web)) || !g_con.web) {
        ConsoleFail(L"no WebView", E_NOINTERFACE);
        return S_OK;
    }
    cc->put_RootVisualTarget(g_con.webVisual);
    ICoreWebView2Controller2* c2 = nullptr;
    if (SUCCEEDED(g_con.controller->QueryInterface(IID_PPV_ARGS(&c2)))) {
        c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0, 0, 0, 0});  // transparent: the pictures lie beneath
        c2->Release();
    }
    RECT rc;
    GetClientRect(app.hwnd, &rc);
    g_con.controller->put_Bounds(rc);
    g_con.controller->put_IsVisible(g_con.on ? TRUE : FALSE);
    ICoreWebView2Settings* st = nullptr;
    if (SUCCEEDED(g_con.web->get_Settings(&st)) && st) {
        st->put_AreDefaultContextMenusEnabled(FALSE);
        st->put_IsStatusBarEnabled(FALSE);
        st->put_IsZoomControlEnabled(FALSE);
        st->put_AreDevToolsEnabled(app.debug ? TRUE : FALSE);
        ICoreWebView2Settings3* s3 = nullptr;
        if (SUCCEEDED(st->QueryInterface(IID_PPV_ARGS(&s3)))) {
            s3->put_AreBrowserAcceleratorKeysEnabled(app.debug ? TRUE : FALSE);  // no reload, find, print ...
            s3->Release();
        }
        st->Release();
    }
    ICoreWebView2_3* w3 = nullptr;
    if (SUCCEEDED(g_con.web->QueryInterface(IID_PPV_ARGS(&w3)))) {
        w3->SetVirtualHostNameToFolderMapping(kUiHost, ui.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        w3->Release();
    }
    ICoreWebView2* web = g_con.web;
    OnEvent<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
        [web](auto* h, auto* t) { return web->add_WebMessageReceived(h, t); },
        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR json = nullptr;
            if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                const std::wstring j = json;
                CoTaskMemFree(json);
                ConsoleCommand(j);
            }
            return S_OK;
        });
    // the page and nothing else: no navigation away, no new windows
    OnEvent<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
        [web](auto* h, auto* t) { return web->add_NavigationStarting(h, t); },
        [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            const std::wstring prefix = std::wstring(L"https://") + kUiHost + L"/";
            if (!uri || wcsncmp(uri, prefix.c_str(), prefix.size()) != 0) args->put_Cancel(TRUE);
            else g_con.ready = g_con.pageKeys = false;  // (re)loading: wait for the page's 'ready'
            if (uri) CoTaskMemFree(uri);
            return S_OK;
        });
    OnEvent<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(
        [web](auto* h, auto* t) { return web->add_NewWindowRequested(h, t); },
        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            args->put_Handled(TRUE);
            return S_OK;
        });
    OnEvent<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>(
        [web](auto* h, auto* t) { return web->add_ProcessFailed(h, t); },
        [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
            args->get_ProcessFailedKind(&kind);
            if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                ConsoleFail(L"the WebView2 browser process ended", E_FAIL);
            } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                       kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
                ConsoleLog(L"console: the page's process failed, reloading");
                if (g_con.web) g_con.web->Reload();
            }
            return S_OK;
        });
    OnEvent<ICoreWebView2CursorChangedEventHandler, ICoreWebView2CompositionController*, IUnknown*>(
        [cc](auto* h, auto* t) { return cc->add_CursorChanged(h, t); },
        [](ICoreWebView2CompositionController* c, IUnknown*) -> HRESULT {
            HCURSOR cur = nullptr;
            if (SUCCEEDED(c->get_Cursor(&cur)) && cur) {
                g_con.cursor = cur;
                POINT p;
                GetCursorPos(&p);
                if (WindowFromPoint(p) == app.hwnd) SetCursor(cur);
            }
            return S_OK;
        });
    // Keys while the WebView has the keyboard: those WebView2 counts as accelerators come here first.
    ICoreWebView2Controller* ctl = g_con.controller;
    OnEvent<ICoreWebView2AcceleratorKeyPressedEventHandler, ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs*>(
        [ctl](auto* h, auto* t) { return ctl->add_AcceleratorKeyPressed(h, t); },
        [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_KEY_EVENT_KIND kind{};
            UINT vk = 0;
            args->get_KeyEventKind(&kind);
            args->get_VirtualKey(&vk);
            if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN) return S_OK;  // system keys (Alt+F4 ...) as usual
            if (g_con.settingsOpen || g_con.pageKeys) return S_OK;  // a dialog or pop-up open: the page's keys
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return S_OK;
            if (app.debug && (vk == VK_F12 || (vk == 'I' && (GetKeyState(VK_CONTROL) & 0x8000)))) return S_OK;  // DevTools
            COREWEBVIEW2_PHYSICAL_KEY_STATUS ks{};
            args->get_PhysicalKeyStatus(&ks);
            args->put_Handled(TRUE);
            if (KeyNoRepeat(static_cast<int>(vk)) && ks.WasKeyDown) return S_OK;  // no auto-repeat (carrier step, still)
            HandleKey(vk);
            ConsolePush();
            return S_OK;
        });
    const std::wstring url = std::wstring(L"https://") + kUiHost + L"/index.html";
    g_con.web->Navigate(url.c_str());
    g_con.dcomp->Commit();
    ConsoleLog(L"console: WebView2 ready");
    return S_OK;
}

// DirectComposition tree and the WebView (created asynchronously). False = cannot start at all.
bool ConsoleStart() {
    std::wstring ui;
    if (!ConsoleUiFolder(&ui)) return false;
    HRESULT hr = DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&g_con.dcomp));
    if (SUCCEEDED(hr)) hr = g_con.dcomp->CreateTargetForHwnd(app.hwnd, TRUE, &g_con.target);
    if (SUCCEEDED(hr)) hr = g_con.dcomp->CreateVisual(&g_con.root);
    if (SUCCEEDED(hr)) hr = g_con.dcomp->CreateVisual(&g_con.videoVisual);
    if (SUCCEEDED(hr)) hr = g_con.dcomp->CreateVisual(&g_con.webVisual);
    if (SUCCEEDED(hr)) hr = g_con.root->AddVisual(g_con.videoVisual, FALSE, nullptr);
    if (SUCCEEDED(hr)) hr = g_con.root->AddVisual(g_con.webVisual, TRUE, g_con.videoVisual);  // the page in front
    if (FAILED(hr)) {
        ConsoleFail(L"DirectComposition unavailable", hr);
        return false;
    }
    g_con.dcomp->Commit();
    wchar_t local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const std::wstring data = std::wstring(local) + L"\\ILARTS\\WebView2";
    SHCreateDirectoryExW(nullptr, data.c_str(), nullptr);
    auto* envDone = new ComHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
        [ui](HRESULT err, ICoreWebView2Environment* env) -> HRESULT {
            ICoreWebView2Environment3* env3 = nullptr;
            if (FAILED(err) || !env || FAILED(env->QueryInterface(IID_PPV_ARGS(&env3)))) {
                ConsoleFail(L"WebView2 environment", FAILED(err) ? err : E_NOINTERFACE);
                return S_OK;
            }
            auto* ctlDone = new ComHandler<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler, HRESULT,
                                           ICoreWebView2CompositionController*>(
                [ui](HRESULT err2, ICoreWebView2CompositionController* cc) -> HRESULT {
                    if (FAILED(err2) || !cc) {
                        ConsoleFail(L"WebView2 controller", FAILED(err2) ? err2 : E_FAIL);
                        return S_OK;
                    }
                    return ConsoleControllerReady(cc, ui);
                });
            const HRESULT h2 = env3->CreateCoreWebView2CompositionController(app.hwnd, ctlDone);
            if (FAILED(h2)) ConsoleFail(L"WebView2 controller", h2);
            ctlDone->Release();
            env3->Release();
            return S_OK;
        });
    hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), nullptr, envDone);
    envDone->Release();
    if (FAILED(hr)) {
        ConsoleFail(L"WebView2 runtime", hr);
        return false;
    }
    return true;
}

void ConsoleShutdown() {
    if (g_con.controller) g_con.controller->Close();
    SafeRelease(g_con.web);
    SafeRelease(g_con.controller);
    SafeRelease(g_con.comp);
    if (g_con.videoVisual) g_con.videoVisual->SetContent(nullptr);
    SafeRelease(g_con.webVisual);
    SafeRelease(g_con.videoVisual);
    SafeRelease(g_con.root);
    SafeRelease(g_con.target);
    SafeRelease(g_con.dcomp);
    g_con.ready = false;
}

// The window's swap chain while the console is shown: composed under the page.
bool ConsoleCreateSwap(IDXGIFactory2* factory) {
    if (!g_con.videoVisual) return false;
    RECT rc;
    GetClientRect(app.hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = static_cast<UINT>(std::max<LONG>(8, rc.right));
    sd.Height = static_cast<UINT>(std::max<LONG>(8, rc.bottom));
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    const HRESULT hr = factory->CreateSwapChainForComposition(app.dev, &sd, nullptr, &app.swap);
    if (FAILED(hr)) {
        ConsoleLog(L"console: composition swap chain failed");
        return false;
    }
    g_con.videoVisual->SetContent(app.swap);
    g_con.dcomp->Commit();
    return true;
}

// Before the swap chain or the device goes: the visual must let go of it.
void ConsoleDropSwap() {
    SafeRelease(g_con.canvasSrv);
    g_con.canvasFor = nullptr;
    if (g_con.videoVisual && g_con.dcomp) {
        g_con.videoVisual->SetContent(nullptr);
        g_con.dcomp->Commit();
    }
}

// The window changed size: the WebView and the swap chain follow.
void ConsoleResize() {
    RECT rc;
    GetClientRect(app.hwnd, &rc);
    if (g_con.controller) g_con.controller->put_Bounds(rc);
    if (app.swap && rc.right > 0 && rc.bottom > 0) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        app.swap->GetDesc1(&d);
        if (d.Width != static_cast<UINT>(rc.right) || d.Height != static_cast<UINT>(rc.bottom)) {
            SafeRelease(app.rtv);
            app.ctx->OMSetRenderTargets(0, nullptr, nullptr);
            app.swap->ResizeBuffers(0, static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom), DXGI_FORMAT_UNKNOWN, 0);
            CreateBackbufferView();
            // Black until the next frame: while the window is dragged to a new size Windows runs its own message
            // loop, nothing renders, and the monitors would show the last pictures stretched.
            if (app.rtv) {
                const float black[4] = {0, 0, 0, 1};
                app.ctx->ClearRenderTargetView(app.rtv, black);
                app.swap->Present(0, 0);
            }
        }
    }
    if (g_con.dcomp) g_con.dcomp->Commit();
}

// ---------------------------------------------------------------- the pictures

// Source ids of the page: plat, lso1, lso2, then the cameras by name ("island", "catapult").
std::wstring ConsoleSourceId(int source) { return SourceKey(source); }
int ConsoleSourceOf(const std::wstring& id) {
    for (int i = 0; i < FeedCount(); ++i)
        if (ConsoleSourceId(i) == id) return i;
    return -1;
}

// A monitor's screen in the swap chain's pixels; false = the page has none of that name (yet).
bool ConsoleRectOf(const wchar_t* id, ConsoleRect* out) {
    const auto it = g_con.rects.find(id);
    if (it == g_con.rects.end() || it->second.w < 4 || it->second.h < 4) return false;
    const float s = g_con.dpr;
    *out = ConsoleRect{it->second.x * s, it->second.y * s, it->second.w * s, it->second.h * s};
    return true;
}

// A picture of the given shape centred in a monitor's screen.
D3D11_VIEWPORT ConsoleFit(const ConsoleRect& r, float aspect) {
    D3D11_VIEWPORT vp = Fit(r.w, r.h, aspect, 1.0f);
    vp.TopLeftX += r.x;
    vp.TopLeftY += r.y;
    return vp;
}

// A finished texture (replay frame, recording canvas) into a viewport.
void ConsoleCopy(ID3D11ShaderResourceView* srv, const D3D11_VIEWPORT& vp) {
    app.ctx->OMSetRenderTargets(1, &app.rtv, nullptr);
    app.ctx->RSSetViewports(1, &vp);
    app.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    app.ctx->IASetInputLayout(nullptr);
    app.ctx->VSSetShader(app.vs, nullptr, 0);
    app.ctx->PSSetShader(g_rec.psCopy, nullptr, 0);
    app.ctx->PSSetShaderResources(0, 1, &srv);
    app.ctx->PSSetSamplers(0, 1, &app.sampler);
    app.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    app.ctx->PSSetShaderResources(0, 1, &none);
}

// What the replay monitor shows: the replay while one runs, else the picture going into the buffer.
ID3D11ShaderResourceView* ConsoleReplaySource() {
    if (!g_rec.gpu) return nullptr;
    if (g_rec.replay && !g_rec.frames.empty()) return g_rec.replaySrv;
    if (!g_rec.main.gpu || !g_rec.main.canvas || video::Now() - g_rec.main.lastFrame > 1.0) return nullptr;
    if (g_con.canvasFor != g_rec.main.canvas) {
        SafeRelease(g_con.canvasSrv);
        app.dev->CreateShaderResourceView(g_rec.main.canvas, nullptr, &g_con.canvasSrv);
        g_con.canvasFor = g_rec.main.canvas;
    }
    return g_con.canvasSrv;
}

// A preview: the source as DCS makes it - no adjustments, levels or CCTV look. The PLAT and the station
// screens as delivered; a camera through the camera curve at DCS's exposure (its LDR picture is in colour).
ViewState ConsolePreviewView(int source) {
    ViewState pv = ViewFor(source);
    if (!pv.main) return pv;
    if (pv.cam) {
        if (pv.proc == Proc::Off) pv.proc = pv.hdr ? Proc::Pipeline : Proc::Levels;
    } else {
        pv.proc = Proc::Off;
    }
    return pv;
}

// ---- replay feeds: monitor B's picture and each preview's, in replay rings of their own, so an instant replay
// plays every monitor on the wall (A's footage is the main channel's). A preview's ring is half size.
struct ReplayFeed {
    int source = -1;
    bool preview = false;  // a preview's picture (as DCS makes it); else monitor B's
    Channel ch;
    double lastShown = 0;
    std::vector<video::FramePtr> frames;  // the replay's footage of it
    int shown = -1;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
};
std::vector<std::unique_ptr<ReplayFeed>> g_feeds;

ReplayFeed* FindFeed(int source, bool preview) {
    for (auto& f : g_feeds)
        if (f->source == source && f->preview == preview) return f.get();
    return nullptr;
}
bool FeedShown(const ReplayFeed& f) {
    if (!g_con.on) return false;
    if (!f.preview) return g_con.wall == 1 && ConsoleSourceOf(g_con.secondKey) == f.source;
    if (g_con.wall != 2) return false;
    for (const auto& k : g_con.prev)
        if (ConsoleSourceOf(k) == f.source) return true;
    return false;
}
void FeedRelease(ReplayFeed& f) {
    ChannelRelease(f.ch);
    SafeRelease(f.srv);
    SafeRelease(f.tex);
    f.shown = -1;
}
void FeedsRelease() {
    for (auto& f : g_feeds) FeedRelease(*f);
}
void FeedsShutdown() {
    for (auto& f : g_feeds) {
        FeedRelease(*f);
        f->ch.eng.stop();
    }
    g_feeds.clear();
}

// Every loop: a feed for each picture on monitor B or a preview. One not on screen for a whole ring is dropped
// (never during a replay).
void FeedsTick(double now) {
    if (!g_rec.ok) return;
    auto want = [&](int s, bool preview) {
        if (s < 0 || s >= FeedCount()) return;
        ReplayFeed* f = FindFeed(s, preview);
        if (!f) {
            auto n = std::make_unique<ReplayFeed>();
            n->source = n->ch.source = s;
            n->preview = preview;
            video::Engine::Config c = g_engCfg;
            if (preview) c.w /= 2, c.h /= 2;
            n->ch.ok = n->ch.eng.start(c);
            f = n.get();
            g_feeds.push_back(std::move(n));
        }
        f->lastShown = now;
    };
    if (g_con.on && g_con.wall == 1) want(ConsoleSourceOf(g_con.secondKey), false);
    if (g_con.on && g_con.wall == 2)
        for (const auto& k : g_con.prev) want(ConsoleSourceOf(k), true);
    if (g_rec.replay) return;
    for (size_t i = 0; i < g_feeds.size();) {
        if (now - g_feeds[i]->lastShown <= g_engCfg.ringSeconds) {
            ++i;
            continue;
        }
        FeedRelease(*g_feeds[i]);
        g_feeds[i]->ch.eng.stop();
        g_feeds.erase(g_feeds.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// Each rendered frame: the feeds on screen, drawn as shown (a preview without adjustments) into their rings.
void FeedsCapture() {
    for (auto& f : g_feeds) {
        if (!f->ch.ok) continue;
        const bool on = FeedShown(*f);
        const ViewState v = !on ? ViewState{} : f->preview ? ConsolePreviewView(f->source) : ViewFor(f->source);
        ChannelCaptureWith(f->ch, v.main != nullptr, [&](ID3D11RenderTargetView* rtv, int w, int h) {
            g_neutral = f->preview;
            DrawFeed(v, rtv, Fit(static_cast<float>(w), static_cast<float>(h), ViewAspect(v), 1.0f));
            g_neutral = false;
        });
    }
}

// A replay takes every feed's footage of the same time as A's.
void FeedsReplayStart(double t0, double t1) {
    for (auto& f : g_feeds) {
        f->frames = f->ch.ok ? f->ch.eng.range(t0, t1) : std::vector<video::FramePtr>{};
        f->shown = -1;
    }
}
void FeedsReplayStop() {
    for (auto& f : g_feeds) {
        f->frames.clear();
        f->frames.shrink_to_fit();
        f->shown = -1;
    }
}

// Saving a replay: a file of each other picture on the wall too ("<base> ISLAND"), one per source not on A
// (monitor B's before a preview's, which is half size).
void FeedsReplaySave(const std::wstring& base) {
    std::vector<int> done{ProgramSource()};
    for (const bool preview : {false, true})
        for (auto& f : g_feeds) {
            if (f->preview != preview || f->frames.size() < 2 || !FeedShown(*f)) continue;
            if (std::find(done.begin(), done.end(), f->source) != done.end()) continue;
            done.push_back(f->source);
            f->ch.eng.saveClip(f->frames, UniqueVideoPath(base + L" " + SourceTag(f->source)));
        }
}
void FeedsEach(const std::function<void(Channel&)>& fn) {
    for (auto& f : g_feeds) fn(f->ch);
}

// The replay feed of what a monitor shows now (preview: a preview's), if the replay has footage of it.
ReplayFeed* FeedInReplay(int source, bool preview) {
    if (!g_rec.replay || g_rec.frames.empty()) return nullptr;
    ReplayFeed* f = FindFeed(source, preview);
    return f && !f->frames.empty() ? f : nullptr;
}

// A feed's picture at the replay's position (A's clock) in a monitor's screen; black where it has none then.
void FeedDrawReplay(ReplayFeed& f, const ConsoleRect& r) {
    const auto& c = f.ch.eng.config();
    if (!f.tex) {
        D3D11_TEXTURE2D_DESC td = CanvasDesc(c);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(app.dev->CreateTexture2D(&td, nullptr, &f.tex)) || FAILED(app.dev->CreateShaderResourceView(f.tex, nullptr, &f.srv))) {
            SafeRelease(f.tex);
            return;
        }
        f.shown = -1;
    }
    const double t = g_rec.frames.front()->t + g_rec.pos;
    const auto it = std::upper_bound(f.frames.begin(), f.frames.end(), t + 1e-6, [](double x, const video::FramePtr& p) { return x < p->t; });
    if (it == f.frames.begin()) return;
    const int idx = static_cast<int>(it - f.frames.begin()) - 1;
    if (t - f.frames[idx]->t > 1.0) return;  // not on screen then
    if (idx != f.shown) {
        int w = 0, h = 0;
        if (g_rec.jpeg.decode(f.frames[idx]->jpeg, &g_rec.decoded, &w, &h) && w == c.w && h == c.h)
            app.ctx->UpdateSubresource(f.tex, 0, nullptr, g_rec.decoded.data(), static_cast<UINT>(w) * 4, 0);
        f.shown = idx;
    }
    ConsoleCopy(f.srv, ConsoleFit(r, static_cast<float>(c.w) / static_cast<float>(c.h)));
}

void ConsoleRender() {
    if (!app.dev || !app.swap || !app.rtv) return;
    const float black[4] = {0, 0, 0, 1};
    ViewState v = CurrentView();  // first: building the station screen may use the device context
    const int s2 = ConsoleSecond();
    const ViewState v2 = s2 >= 0 ? ViewFor(s2) : ViewState{};  // monitor B, with its own picture's levels
    app.ctx->OMSetRenderTargets(1, &app.rtv, nullptr);
    app.ctx->ClearRenderTargetView(app.rtv, black);
    ConsoleRect r;
    D3D11_VIEWPORT pvp{};
    bool drew = false;
    if (ConsoleRectOf(L"program", &r)) {
        if (g_rec.replay) {
            drew = RecDrawReplay(r.w, r.h, &pvp, r.x, r.y);
        } else if (v.main) {
            pvp = ConsoleFit(r, ViewAspect(v));
            DrawFeed(v, app.rtv, pvp);
            drew = true;
            ++app.presentedWithImage;
        }
    }
    RecCapture(v);  // recordings, black box, stream (also during a replay)
    app.ctx->OMSetRenderTargets(1, &app.rtv, nullptr);
    if (drew) RecDrawBadges(pvp, false);  // the replay's progress bar (the page shows the rest)
    if (ConsoleRectOf(L"second", &r)) {
        if (ReplayFeed* f = FeedInReplay(s2, false)) FeedDrawReplay(*f, r);
        else if (v2.main) DrawFeed(v2, app.rtv, ConsoleFit(r, ViewAspect(v2)));
    }
    for (int k = 0; k < 4; ++k) {
        const int i = ConsoleSourceOf(g_con.prev[k]);
        if (i < 0 || !ConsoleRectOf((L"p" + std::to_wstring(k)).c_str(), &r)) continue;
        if (ReplayFeed* f = FeedInReplay(i, true)) {
            FeedDrawReplay(*f, r);
            continue;
        }
        const ViewState pv = ConsolePreviewView(i);
        if (!pv.main) continue;
        g_neutral = true;
        DrawFeed(pv, app.rtv, ConsoleFit(r, ViewAspect(pv)));
        g_neutral = false;
    }
    if (ConsoleRectOf(L"replay", &r)) {
        if (ID3D11ShaderResourceView* srv = ConsoleReplaySource()) {
            const auto& c = g_rec.main.eng.config();
            ConsoleCopy(srv, ConsoleFit(r, static_cast<float>(c.w) / static_cast<float>(c.h)));
        }
    }
    if (!g_shotPath.empty()) {
        SaveBackbuffer(g_shotPath);
        g_shotPath.clear();
    }
    app.swap->Present(1, 0);
}

// ---------------------------------------------------------------- state for the page

// The videos folder's contents (subfolders too): the MP4s in it and the bytes of every file.
struct FolderStats {
    int captures = 0;
    unsigned long long bytes = 0;
};
void ScanFolder(const std::wstring& dir, FolderStats& st, int depth) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 8 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                ScanFolder(dir + L"\\" + fd.cFileName, st, depth + 1);
            continue;
        }
        st.bytes += (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        const size_t n = wcslen(fd.cFileName);
        if (n > 4 && !_wcsicmp(fd.cFileName + n - 4, L".mp4")) ++st.captures;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Rescanned every 2 s on a worker thread: a big folder or a slow drive never holds up the window.
struct {
    std::future<FolderStats> job;
    FolderStats last;
    bool have = false;
    ULONGLONG at = 0;
} g_folderStats;
const FolderStats* VideoFolderStats() {
    auto& f = g_folderStats;
    if (f.job.valid() && f.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        f.last = f.job.get();
        f.have = true;
    }
    if (!f.job.valid() && GetTickCount64() - f.at >= 2000) {
        f.at = GetTickCount64();
        f.job = std::async(std::launch::async, [dir = VideoFolder()] {
            FolderStats st;
            ScanFolder(dir, st, 0);
            return st;
        });
    }
    return f.have ? &f.last : nullptr;
}

// "12 CAPTURES · 3.4 GB"
std::wstring FolderStatsText(const FolderStats& st) {
    wchar_t b[64];
    const double mb = st.bytes / 1e6;
    if (mb >= 1000) swprintf_s(b, L"%d CAPTURE%s \x00B7 %.1f GB", st.captures, st.captures == 1 ? L"" : L"S", mb / 1000);
    else swprintf_s(b, L"%d CAPTURE%s \x00B7 %.0f MB", st.captures, st.captures == 1 ? L"" : L"S", mb);
    return b;
}

// Frames per second a source delivers (the viewer's own station screens: the PLAT's).
float ConsoleSourceFps(int source) {
    const ViewState x = ViewFor(source);
    if (!x.main) return 0;
    if (x.synth || x.synthCue) return x.lso ? x.lso->fps : 0;
    return x.main->fps;
}

std::wstring ConsoleResText(const std::wstring& source) {  // "768x768  102 fps" -> "768×768 · 102 FPS"
    std::wstring t;
    for (wchar_t c : source) t += c == L'x' ? L'\x00D7' : static_cast<wchar_t>(towupper(c));
    for (size_t at; (at = t.find(L"  ")) != std::wstring::npos;) t.replace(at, 2, L" \x00B7 ");
    return t;
}

// ---- the monitor wall
int ConsoleSecond() {
    if (!g_con.on || g_con.wall != 1) return -1;
    return ConsoleSourceOf(g_con.secondKey);
}

// The 1 + 4 previews: their sources (default: the PLAT, LSO and fixed cameras not on A, the rest empty).
void ConsoleSavePreviews() {
    std::wstring t;
    for (int k = 0; k < 4; ++k) t += (k ? L"," : L"") + g_con.prev[k];
    WritePrivateProfileStringW(L"viewer", L"console_previews", t.c_str(), app.iniPath.c_str());
}
void ConsoleDefaultPreviews() {
    if (g_con.prevInit) return;
    g_con.prevInit = true;
    int k = 0;
    for (int s = 0; s < FeedCount() && k < 4; ++s)
        if (s != app.selected && !IsDeckSource(s)) g_con.prev[k++] = SourceKey(s);
    for (; k < 4; ++k) g_con.prev[k].clear();
    ConsoleSavePreviews();
}
// Monitor B or a preview on screen shows source s (A: app.selected).
bool ConsoleShows(int s) {
    if (!g_con.on || s < 0) return false;
    if (g_con.wall == 1 && ConsoleSourceOf(g_con.secondKey) == s) return true;
    if (g_con.wall == 2)
        for (const auto& k : g_con.prev)
            if (ConsoleSourceOf(k) == s) return true;
    return false;
}
// A camera switched off: off monitor B and the previews (A: SetCamera's own SelectFeed(0)).
void ConsoleDropSource(int s) {
    if (s < 0) return;
    const std::wstring key = SourceKey(s);
    bool prevChanged = false;
    for (auto& k : g_con.prev)
        if (k == key) k.clear(), prevChanged = true;
    if (prevChanged) ConsoleSavePreviews();
    if (g_con.secondKey == key) {
        g_con.secondKey.clear();
        WritePrivateProfileStringW(L"viewer", L"console_second", L"", app.iniPath.c_str());
    }
}
void ConsoleNoteDeck(int s) {  // the deck position last put on a monitor (the DECK key brings it back)
    if (!IsDeckSource(s)) return;
    g_con.lastDeck = SourceKey(s);
    WritePrivateProfileStringW(L"viewer", L"console_deck", g_con.lastDeck.c_str(), app.iniPath.c_str());
}
// The DECK key on a monitor showing `cur`: the position it shows, else the last one if no other monitor shows it,
// else the first one no monitor shows.
int ConsoleDeckFor(int cur) {
    if (IsDeckSource(cur)) return cur;
    const int last = ConsoleSourceOf(g_con.lastDeck);
    if (IsDeckSource(last) && !SourceShown(last)) return last;
    const int n = CameraCount() - g_firstPos;
    for (int k = 0; k < n; ++k)
        if (!SourceShown(PositionSource(k))) return PositionSource(k);
    return n > 0 ? PositionSource(0) : -1;
}

// Monitor A shows no source (its lit source key pressed again); any source key turns it on again (SelectFeed).
void ConsoleProgramOff() {
    if (g_con.progOff) return;
    if (g_rec.replay) ReplayStop();
    g_con.progOff = true;
    ResetAuto();
    CameraReconcile();  // a deck position only it showed gives its render slot back
    Changed();
}

// Monitor B shows source s (a camera is switched on to be shown; -1 = nothing).
void ConsoleSetSecond(int s) {
    if (s >= FeedCount()) return;
    g_con.secondKey = s >= 0 ? SourceKey(s) : L"";
    WritePrivateProfileStringW(L"viewer", L"console_second", g_con.secondKey.c_str(), app.iniPath.c_str());
    if (s >= kFeeds && !((g_cam.want >> (s - kFeeds)) & 1)) SetCamera(s - kFeeds, true);
    ConsoleNoteDeck(s);
    CameraReconcile();
    ResetAuto();
    ConsolePush();
}

void ConsoleSetWall(int w) {
    g_con.wall = std::clamp(w, 0, 2);
    WritePrivateProfileStringW(L"viewer", L"console_layout", g_con.wall == 0 ? L"1" : g_con.wall == 2 ? L"q" : L"2", app.iniPath.c_str());
    if (g_con.wall == 1) {  // monitor B must show something that exists, and a camera on it must render
        int s2 = ConsoleSourceOf(g_con.secondKey);
        if (s2 < 0) s2 = app.selected == 1 ? 0 : 1;
        ConsoleSetSecond(s2);
    }
    if (g_con.wall == 2) ConsoleDefaultPreviews();
    CameraReconcile();  // deck positions now off screen give their render slots back; those now on screen get one
    ConsolePush();
}

// ---- deck cycle: a monitor (A or B) showing a deck position steps through the positions ticked in its CYCLE
// pop-up, one every `seconds` ([viewer] cycle_a / cycle_b = 0 / 1, cycle_a_views = source keys, cycle_a_seconds).
// Seamless: when the time is up the next position is wanted first (CycleWants: it takes a free render slot and is
// asked for its view), the monitor switches once that slot delivers frames of it (kCycleFrames after DCS reports it
// at that position), and only then is the previous one let go (CameraReconcile gives its slot back unless something
// else shows it). No free slot, or no picture within kCycleWait: the monitor switches in place (the slot moves).
// It runs only while the monitor is on the wall and shows a deck position, and not during a replay. Picking a
// position by hand (◀ ▶, the list, another monitor) starts the interval again from there; holding the PTZ stick on
// the monitor's camera holds it on that position (the interval starts again on release).
constexpr double kCycleMinSeconds = 2, kCycleMaxSeconds = 3600, kCycleWait = 4;
constexpr LONG64 kCycleFrames = 3;
struct DeckCycle {
    bool loaded = false, on = false;
    std::vector<std::wstring> keys;  // the positions ticked (source keys), in no particular order
    double seconds = 10;
    int at = -1;            // the position the monitor showed at the last tick
    double nextAt = 0;      // when the next step is due
    int pending = -1;       // the position being brought up (a source), -1 = none
    double pendingSince = 0;
    LONG64 pendingBase = -1;  // its target's frame counter when DCS first showed it at that position
    double heldUntil = 0;   // the PTZ stick is held on this monitor's camera
};
DeckCycle g_cycle[2];
const wchar_t* CycleIniKey(int u) { return u ? L"cycle_b" : L"cycle_a"; }
DeckCycle& Cycle(int u) {
    DeckCycle& c = g_cycle[u ? 1 : 0];
    if (c.loaded) return c;
    c.loaded = true;
    const std::wstring k = CycleIniKey(u);
    c.on = GetPrivateProfileIntW(L"viewer", k.c_str(), 0, app.iniPath.c_str()) != 0;
    const std::wstring sel = IniGet(L"viewer", (k + L"_views").c_str(), L"");
    for (size_t a = 0; a <= sel.size();) {
        size_t e = sel.find(L',', a);
        if (e == std::wstring::npos) e = sel.size();
        if (e > a) c.keys.push_back(sel.substr(a, e - a));
        a = e + 1;
    }
    const double s = _wtof(IniGet(L"viewer", (k + L"_seconds").c_str(), L"10").c_str());
    c.seconds = s > 0 ? std::clamp(s, kCycleMinSeconds, kCycleMaxSeconds) : 10;
    return c;
}
void CycleSave(int u) {
    const DeckCycle& c = Cycle(u);
    const std::wstring k = CycleIniKey(u);
    std::wstring sel;
    for (const auto& x : c.keys) sel += (sel.empty() ? L"" : L",") + x;
    wchar_t sec[32];
    swprintf_s(sec, L"%g", c.seconds);
    WritePrivateProfileStringW(L"viewer", k.c_str(), c.on ? L"1" : L"0", app.iniPath.c_str());
    WritePrivateProfileStringW(L"viewer", (k + L"_views").c_str(), sel.c_str(), app.iniPath.c_str());
    WritePrivateProfileStringW(L"viewer", (k + L"_seconds").c_str(), sec, app.iniPath.c_str());
}
bool CycleTicked(const DeckCycle& c, int s) { return std::find(c.keys.begin(), c.keys.end(), SourceKey(s)) != c.keys.end(); }
int CycleTickedCount(const DeckCycle& c) {
    int n = 0;
    for (int k = 0; k < CameraCount() - g_firstPos; ++k) n += CycleTicked(c, PositionSource(k)) ? 1 : 0;
    return n;
}
// A position a cycle is bringing up: it renders already (CameraNeeded).
bool CycleWants(int s) {
    for (const auto& c : g_cycle)
        if (c.pending >= 0 && c.pending == s) return true;
    return false;
}
int CycleMonSource(int u) { return u ? ConsoleSecond() : ProgramSource(); }
// The next ticked position after cur (in the positions' order), not shown elsewhere; -1 = none.
int CycleNext(int u, int cur) {
    const DeckCycle& c = Cycle(u);
    const int n = CameraCount() - g_firstPos, at = IsDeckSource(cur) ? g_lcams[cur - kFeeds].pos : -1;
    for (int step = 1; step <= n; ++step) {
        const int s = PositionSource(((at + step) % n + n) % n);
        if (s == cur || !CycleTicked(c, s) || SourceShown(s) || CycleWants(s)) continue;
        return s;
    }
    return -1;
}
std::wstring CycleSlotName(int s) {
    const int p = IsDeckSource(s) ? PhysOf(s - kFeeds) : -1;
    return p >= 0 ? Utf8W(PhysName(p)) : std::wstring(L"none");
}
void CycleCancel(DeckCycle& c) {
    if (c.pending < 0) return;
    c.pending = -1;
    CameraReconcile();  // its slot goes back
}
// The monitor shows the position brought up; the one before lets its slot go.
void CycleSwitch(int u, DeckCycle& c, int to, double now, const wchar_t* how) {
    const int from = CycleMonSource(u);
    if (u) {
        ConsoleSetSecond(to);
    } else {
        SelectFeed(to);
        ConsoleNoteDeck(to);
    }
    c.pending = -1;
    CameraReconcile();
    c.at = to;
    c.nextAt = now + c.seconds;
    ConsoleLog(std::wstring(L"cycle ") + (u ? L"B" : L"A") + L": " + SourceTag(to) + L" shown (" + how + L", slot " + CycleSlotName(to) +
               L"); " + SourceTag(from) + L" slot after release: " + CycleSlotName(from));
    ConsolePush();
}
void CycleTick(double now) {
    for (int u = 0; u < 2; ++u) {
        DeckCycle& c = Cycle(u);
        const int cur = CycleMonSource(u);
        const bool run = g_con.on && c.on && IsDeckSource(cur) && (u == 0 || g_con.wall == 1) && !g_rec.replay && CamLive() &&
                         CycleTickedCount(c) > 0;
        if (!run) {
            CycleCancel(c);
            c.at = -1;
            continue;
        }
        if (cur != c.at || now < c.heldUntil) {  // a new position (by hand or a cycle step), or the stick is held: wait from now
            CycleCancel(c);
            c.at = cur;
            c.nextAt = now + c.seconds;
            continue;
        }
        if (c.pending < 0) {
            if (now < c.nextAt) continue;
            const int next = CycleNext(u, cur);
            if (next < 0) {
                c.nextAt = now + c.seconds;
                continue;
            }
            c.pending = next;
            c.pendingSince = now;
            c.pendingBase = -1;
            CameraReconcile();  // it takes a free slot, if there is one
            if (PhysOf(next - kFeeds) < 0) {  // none free: the monitor's own slot moves
                CycleSwitch(u, c, next, now, L"in place, no free slot");
                continue;
            }
            ConsoleLog(std::wstring(L"cycle ") + (u ? L"B" : L"A") + L": " + SourceTag(cur) + L" -> " + SourceTag(next) + L": slot " +
                       CycleSlotName(next) + L" asked for it");
            continue;
        }
        const int next = c.pending;
        if (!CycleTicked(c, next) || SourceShown(next)) {  // unticked meanwhile, or another monitor took it
            CycleCancel(c);
            c.nextAt = now;
            continue;
        }
        if (PhysOf(next - kFeeds) < 0) {  // its slot went to a monitor that needed it
            CycleSwitch(u, c, next, now, L"in place, slot lost");
            continue;
        }
        const ViewState v = ViewFor(next);  // only once DCS renders that slot at this position
        if (v.main && c.pendingBase < 0) c.pendingBase = v.main->lastFrame;
        if (v.main && c.pendingBase >= 0 && v.main->lastFrame >= c.pendingBase + kCycleFrames) {
            wchar_t how[64];
            swprintf_s(how, L"ready after %.2f s", now - c.pendingSince);
            CycleSwitch(u, c, next, now, how);
        } else if (now - c.pendingSince > kCycleWait) {
            CycleSwitch(u, c, next, now, L"no picture in time");
        }
    }
}
// The PTZ stick moves the camera on monitor u: the cycle holds there.
void CycleHold(int s, double now) {
    for (int u = 0; u < 2; ++u)
        if (s >= 0 && CycleMonSource(u) == s) Cycle(u).heldUntil = now + 0.5;
}

// The source a monitor's controls act on: {"mon":1} = monitor B, else monitor A (the program).
int ConsoleMonSource(const std::wstring& j) {
    double m = 0;
    JsonNum(j, L"mon", &m);
    return m > 0.5 ? ConsoleSecond() : ProgramSource();
}

// The levels of settings group g, and saving them after a change (the program's group goes through Changed()).
Settings& GroupRef(int g) { return g == g_curGroup ? app.set : g_groupSet[g]; }
void GroupChanged(int g) {
    if (g == g_curGroup) {
        Changed();
        return;
    }
    SaveGroupSettings(g, g_groupSet[g]);
    ConsolePush();
}

static const wchar_t* kShortName[kFeeds] = {L"PLAT", L"LSO 1", L"LSO 2"};
static const wchar_t* kPlateName[kFeeds] = {L"PLAT CAMERA", L"LSO CAMERA 1", L"LSO CAMERA 2"};
std::wstring ConsoleTag(int s) { return s < kFeeds ? std::wstring(kShortName[std::max(0, s)]) : CameraTag(s - kFeeds); }
std::wstring ConsoleButtonName(int s) {  // a key's label (CATAPULT -> CAT); a deck position (no key of its own): its name
    const std::wstring t = ConsoleTag(s);
    return t.size() > 6 && !IsDeckSource(s) ? t.substr(0, 3) : t;
}
std::wstring ConsolePlate(int s) { return s < kFeeds ? std::wstring(kPlateName[s]) : SourceTag(s) + L" CAMERA"; }  // a view's name

float ConsoleSourceFps(int source);
// One monitor of the wall: what it shows, its message, and the levels and PTZ of that picture (no levels for LSO 2;
// exposure only in the Pipeline mode).
void ConsoleMonitor(Json& s, int src, bool program) {
    const bool valid = src >= 0 && src < FeedCount(), compatBad = HeaderValid() && app.header->compat.state == 3;
    const ViewState v = valid ? ViewFor(src) : ViewState{};
    const bool camFeed = valid && src >= kFeeds, picture = v.main != nullptr,
               replay = program ? g_rec.replay : valid && FeedInReplay(src, false) != nullptr;
    s.open(nullptr);
    s.str(L"id", valid ? ConsoleSourceId(src) : L"");
    s.str(L"name", valid ? ConsoleTag(src) : L"");
    s.str(L"plate", valid ? ConsolePlate(src) : L"NO SOURCE");
    if (picture) {
        const Target* t = (v.synth || v.synthCue) ? v.lso : v.main;
        wchar_t b[64];
        swprintf_s(b, L"%u\x00D7%u \x00B7 %.0f FPS", v.main->w, v.main->h, t ? t->fps : 0.0f);
        s.str(L"res", b);
    }
    s.flag(L"signal", picture || replay);
    s.flag(L"burnIns", camFeed && !replay);  // the PLAT, the station screens and recordings carry their own
    s.flag(L"replay", replay);
    if (camFeed) s.str(L"burnName", SourceTag(src));
    s.flag(L"rec", valid && RecArmed(src));
    // a deck position: all the deck positions (the view selector) and which one this is
    if (camFeed && CameraViewCount(src - kFeeds) > 0) {
        const int i = src - kFeeds;
        const int k = CameraViewIndex(i, CameraViewName(i));
        s.open(L"views", L'[');
        for (int n = 0; n < CameraViewCount(i); ++n) s.str(nullptr, CameraViewNameAt(i, n));
        s.close(L']');
        s.open(L"hangar", L'[');  // the views on the hangar deck (the lists group them apart)
        for (int n = 0; n < CameraViewCount(i); ++n)
            if (CameraViewHangarAt(i, n)) s.num(nullptr, n);
        s.close(L']');
        s.num(L"view", k);
        const DeckCycle& c = Cycle(program ? 0 : 1);  // its deck cycle: on, the positions ticked, the interval
        s.open(L"cycle");
        s.flag(L"on", c.on);
        s.num(L"seconds", c.seconds);
        s.open(L"sel", L'[');
        for (int n = 0; n < CameraViewCount(i); ++n)
            if (CycleTicked(c, PositionSource(n))) s.num(nullptr, n);
        s.close(L']');
        s.close();
    }
    {
        std::wstring time, ship;
        if (HeaderValid() && app.header->sym.valid)
            time = Ascii(app.header->sym.date, sizeof(app.header->sym.date)) + L"  " + Ascii(app.header->sym.time, sizeof(app.header->sym.time));
        if (HeaderValid() && app.header->selected >= 0 && app.header->selected < CarrierCount()) {
            ship = Ascii(app.header->carriers[app.header->selected].type, sizeof(app.header->carriers[0].type));
            for (auto& c : ship)
                if (c == L'_') c = L'-';
        }
        s.str(L"time", time);
        s.str(L"ship", ship);
    }
    std::wstring msg;
    if (compatBad) {
        const auto& c = app.header->compat;
        msg = L"DCS " + Ascii(c.dcsVersion, sizeof(c.dcsVersion)) + L" IS NOT COMPATIBLE\nWITH THIS VERSION OF ILARTS\n\nROLL DCS BACK TO " +
              Ascii(c.verifiedVersion, sizeof(c.verifiedVersion)) + L"\nOR WAIT FOR AN UPDATE OF THE MOD";
    } else if (!HeaderValid()) {
        msg = L"WAITING FOR DCS\x2026";
    } else if (!valid) {
        msg = L"NO SOURCE";
    } else if (camFeed && !((g_cam.want >> (src - kFeeds)) & 1)) {
        msg = L"CAMERA OFF";
    } else if (!picture) {
        msg = camFeed ? L"CAMERA STARTING\x2026" : L"NO SIGNAL";
    }
    s.str(L"message", msg);
    // levels
    if (valid && (src <= 1 || camFeed)) {
        const int g = GroupOf(src);
        const Settings& S = SetFor(g);
        const bool lev = S.enabled && v.proc != Proc::Off, pipe = lev && v.proc == Proc::Pipeline;
        const Analysis& an = app.an[v.an];
        s.open(L"picture");
        s.str(L"group", camFeed ? CameraTag(src - kFeeds) : std::wstring(L"PLAT \x00B7 LSO 1"));  // whose levels the knobs set
        s.flag(L"auto", S.autoLevels);
        s.flag(L"cctv", g_cctv.on);
        if (S.autoLevels && lev) {
            wchar_t b[48];
            swprintf_s(b, L"%.3f \x2013 %.3f", an.autoBlack, an.autoWhite);
            s.str(L"autoRange", b);
        }
        s.num(L"exp", S.stops);
        s.num(L"bri", S.brightness);
        s.num(L"con", S.contrast);
        s.num(L"gam", S.gamma);
        s.num(L"str", g_cctv.strength * 100);
        s.open(L"unavailable");
        s.flag(L"reset", false);
        s.flag(L"auto", !lev), s.flag(L"exp", !pipe), s.flag(L"bri", !lev), s.flag(L"con", !lev), s.flag(L"gam", !lev);
        s.close();
        s.close();
    } else {
        s.null(L"picture");
    }
    // PTZ: the camera this monitor shows
    if (camFeed) {  // (DCS not running: a remembered camera, at rest)
        const int i = src - kFeeds, p = PhysOf(i);  // the render slot (a deck position's changes with the pool)
        const lsoshare::Header::PtzState ps = HeaderValid() && p >= 0 ? app.header->ptzState[p] : lsoshare::Header::PtzState{0, 0, 1, 0};
        const std::wstring tag = CameraTag(i);
        s.open(L"ptz");
        s.str(L"id", SourceKey(src));
        s.str(L"name", tag);
        s.flag(L"live", picture);
        s.flag(L"manual", ps.manual != 0);
        const CamTrack& tr = TrackOf(i);
        s.flag(L"track", !IsHangar(i));  // false: no AUTO / TARGET (a hangar position)
        s.flag(L"armed", tr.armed);
        s.flag(L"auto", tr.armed && ps.manual == 0);  // the joystick suspends it until AUTO or REST
        s.num(L"target", tr.target);
        s.open(L"targets", L'[');
        for (int t = kTrackFirst; t <= kTrackLast; ++t) {
            s.open(nullptr, L'[');
            s.num(nullptr, t);
            s.str(nullptr, TrackTargetName(t));
            s.close(L']');
        }
        s.close(L']');
        s.num(L"pan", ps.panDeg);
        s.num(L"tilt", ps.tiltDeg);
        s.num(L"zoom", ps.zoom > 0.05f ? ps.zoom : 1.0f);
        s.flag(L"flirAvail", g_expFlir);  // [experimental] flir: the FLIR key is shown
        s.flag(L"flir", g_expFlir && i < 16 && g_flirOn[i]);
        TuneJson(s, i);  // [developer] tuning: the TUNE panel
        s.close();
    } else {
        s.null(L"ptz");
    }
    s.close();
}

std::wstring ConsoleState() {
    Json s;
    s.open(nullptr);
    const ViewState v = CurrentView();
    const bool compatBad = HeaderValid() && app.header->compat.state == 3;
    const std::wstring progName = ConsoleTag(std::min(app.selected, FeedCount() - 1));
    const bool picture = v.main != nullptr;
    const std::wstring res = picture ? ConsoleResText(app.source) : L"";
    s.flag(L"nativeLook", true);
    s.flag(L"windowButtons", true);
    s.flag(L"settings", true);
    const std::wstring bench = BenchStatus();  // a benchmark under way
    s.str(L"status", !bench.empty() ? bench : compatBad ? L"DCS NOT COMPATIBLE" : !HeaderValid() ? L"WAITING FOR DCS" : ProgramSource() < 0 ? L"NO SOURCE" : progName + L" \x00B7 " + (picture ? res : L"NO SIGNAL"));
    {  // the title bar's benchmark lamp: armed, running, complete, error (bench.inc)
        std::wstring tip;
        const std::wstring lamp = BenchLamp(&tip);
        if (lamp.empty()) {
            s.null(L"bench");
        } else {
            s.open(L"bench");
            s.str(L"state", lamp);
            s.str(L"tip", tip);
            s.close();
        }
    }
    if (GetTickCount64() < g_flashUntil) s.str(L"flash", Upper(g_flash));
    // the knobs' ranges are the application's (contrast and gamma sweep logarithmically, as the sliders did)
    s.open(L"knobs");
    auto knob = [&](const wchar_t* k, double lo, double hi, double step, bool lg) {
        s.open(k);
        s.num(L"min", lo), s.num(L"max", hi), s.num(L"step", step), s.flag(L"log", lg);
        s.close();
    };
    knob(L"exp", -4, 8, 0.05, false);
    knob(L"bri", -0.5, 0.5, 0.01, false);
    knob(L"con", 0.25, 16, 0.01, true);
    knob(L"gam", 0.3, 4, 0.01, true);
    knob(L"str", 0, 200, 5, false);
    s.close();
    // the monitor wall: A (the program) and B
    s.str(L"layout", g_con.wall == 0 ? L"1" : g_con.wall == 2 ? L"q" : L"2");
    s.open(L"monitors", L'[');
    ConsoleMonitor(s, ProgramSource(), true);
    ConsoleMonitor(s, ConsoleSourceOf(g_con.secondKey), false);
    s.close(L']');
    // sources (buttons and previews)
    s.open(L"sources", L'[');
    for (int i = 0; i < FeedCount(); ++i) {
        s.open(nullptr);
        s.str(L"id", ConsoleSourceId(i));
        s.str(L"name", ConsoleButtonName(i));
        const int keySlot = std::min(i, kFeeds + g_firstPos);  // the source keys: a key each, then one for DECK
        s.str(L"key", keySlot < 9 ? std::to_wstring(keySlot + 1) : L"");
        if (IsDeckSource(i)) s.str(L"group", L"deck");  // one DECK key; the view selector picks the position
        if (IsDeckSource(i)) s.flag(L"hangar", IsHangar(i - kFeeds));  // a hangar deck position
        s.str(L"plate", ConsolePlate(i));
        s.flag(L"signal", ViewFor(i).main != nullptr);
        s.flag(L"selected", i == ProgramSource());
        s.flag(L"rec", RecArmed(i));  // the VTR records this source (its REC switch)
        s.num(L"fps", std::round(ConsoleSourceFps(i)));
        s.close();
    }
    s.close(L']');
    ConsoleDefaultPreviews();
    s.open(L"previews", L'[');
    for (const auto& k : g_con.prev) s.str(nullptr, ConsoleSourceOf(k) >= 0 ? k : L"");
    s.close(L']');
    s.open(L"replay");
    s.flag(L"signal", ConsoleReplaySource() != nullptr);
    s.close();
    // ships
    s.open(L"ships", L'[');
    for (int i = 0; i < CarrierCount(); ++i) {
        const auto& c = app.header->carriers[i];
        std::wstring hull = Ascii(c.type, sizeof(c.type));
        if (hull == L"Stennis") hull = L"CVN_74";  // the older Stennis model
        for (auto& ch : hull)
            if (ch == L'_') ch = L'-';
        // the ship's name for each hull the PLAT can sit on (the hook's carrier types)
        static const std::pair<const wchar_t*, const wchar_t*> kShipNames[] = {
            {L"CVN-71", L"ROOSEVELT"}, {L"CVN-72", L"LINCOLN"}, {L"CVN-73", L"WASHINGTON"}, {L"CVN-74", L"STENNIS"}, {L"CVN-75", L"TRUMAN"}};
        std::wstring name;
        for (const auto& [h, n] : kShipNames)
            if (hull == h) name = n;
        s.open(nullptr);
        s.str(L"id", std::to_wstring(i));
        s.str(L"unit", Upper(Ascii(c.name, sizeof(c.name))));
        s.str(L"hull", hull);
        if (!name.empty()) s.str(L"name", name);
        s.flag(L"on", app.header->selected == i);
        s.close();
    }
    s.close(L']');
    // presets
    s.open(L"presets", L'[');
    for (int i = 0; i < kPresets; ++i) {
        s.open(nullptr);
        s.str(L"name", PresetName(i));
        s.str(L"key", L"F" + std::to_wstring(5 + i));
        s.flag(L"saved", g_presets[i].saved);
        s.flag(L"fixed", i == kNightPreset);
        s.flag(L"active", PresetActive(i));
        s.close();
    }
    s.close(L']');
    // cameras
    s.open(L"cameras", L'[');
    for (int i = 0; i < CameraCount(); ++i) {
        const bool want = ((g_cam.want >> i) & 1) != 0, live = ViewFor(kFeeds + i).main != nullptr;
        if (IsPosition(i) && !want) continue;  // a deck position has a rocker only while it renders
        s.open(nullptr);
        s.str(L"id", std::to_wstring(i));
        s.str(L"name", CameraName(i));
        s.flag(L"on", want);
        s.flag(L"live", live);
        s.flag(L"starting", want && !live);
        s.close();
    }
    s.close(L']');
    if (g_con.settingsOpen) SettingsForm(s);
    // VTR
    const double now = video::Now();
    s.open(L"vtr");
    s.flag(L"ok", g_rec.ok);
    std::wstring l1, l2, counter = L"00:00:00", replayTime, speed;
    if (g_rec.replay && !g_rec.frames.empty()) {
        const double dur = g_rec.frames.back()->t - g_rec.frames.front()->t;
        replayTime = L"\x2212" + Hms(dur - g_rec.pos).substr(3);
        speed = SpeedLabel(g_rec.speed);
        l1 = L"REPLAY \x00B7 " + Upper(g_rec.replayLabel) + L" \x00B7 " + replayTime + L" \x00B7 " + speed;
        if (g_rec.paused) l1 += g_rec.pos >= dur ? L" \x00B7 END" : L" \x00B7 PAUSED";
        counter = Hms(g_rec.pos);
    } else if (Recording()) {
        l1 = L"RECORDING \x00B7 " + progName;
        for (const auto& r : g_rec.recs) {
            if (r.manual) {
                l1 += L" \x00B7 MANUAL";
                continue;
            }
            l1 += L" \x00B7 PASS " + std::to_wstring(r.pass);
            for (const auto& p : g_rec.passes)
                if (p.id == r.pass && !p.board.empty()) l1 += L" (" + Utf8W(p.board) + L")";
        }
        counter = Hms(now - OldestRecordingStart());
    } else {
        l1 = !g_rec.ok ? L"VIDEO UNAVAILABLE ON THIS PC" : g_rec.autoMode ? L"AUTO \x00B7 WAITING FOR AN APPROACH" : L"MANUAL \x00B7 STANDBY";
    }
    if (GetTickCount64() < g_rec.statusUntil) {
        l2 = Upper(g_rec.status);
    } else if (g_rec.replay) {
        l2 = L"SPACE PAUSE \x00B7 \x2190\x2192 SEEK \x00B7 \x2191\x2193 SPEED \x00B7 ESC LIVE";
    } else if (const RecState::PassInfo* p = LastPass()) {
        l2 = (p->active ? L"PASS IN PROGRESS \x00B7 " : L"LAST PASS \x00B7 ") + Upper(PassLabel(*p)) + (p->active ? L"" : L" \x00B7 " + std::wstring(ResultName(p->result)));
    } else if (g_rec.ok) {
        wchar_t b[64];
        swprintf_s(b, L"BUFFER \x00B7 KEEPS THE LAST %.0f S", g_rec.main.eng.config().ringSeconds);
        l2 = b;
    }
    s.str(L"counter", counter);
    s.str(L"line1", l1);
    s.str(L"line2", l2);
    s.str(L"mode", g_rec.autoMode ? L"auto" : L"manual");
    s.flag(L"composite", g_rec.composite);
    s.str(L"autoFilter", kAutoFilterKey[g_rec.autoFilter]);
    s.flag(L"noAcft", OwnAircraft() == 2);  // you have no aircraft: only ALL ACFT can record
    s.flag(L"rec", Recording());
    s.flag(L"play", g_rec.replay);
    s.str(L"replayTime", replayTime);
    s.str(L"speed", speed);
    s.flag(L"canSave", g_rec.replay);
    s.flag(L"canStill", g_rec.ok && StillReady());
    if (const FolderStats* fs = VideoFolderStats()) s.str(L"folderInfo", FolderStatsText(*fs));
    s.close();
    // black box and stream
    s.open(L"bb");
    s.flag(L"on", g_bb.on);
    {
        std::wstring t;
        bool alert = false;
        if ((g_bb.on || StreamsOn()) && !g_rec.main.live.encoderOk()) {
            t = L"NO H.264 ENCODER ON THIS PC";
            alert = true;
        } else if (!g_bb.pending.empty()) {
            t = L"MISHAP \x00B7 SAVING THE BLACK BOX IN " + std::to_wstring(std::max(0, static_cast<int>(g_bb.pending.front().at - now))) + L" S";
            alert = true;
        } else if (g_bb.on) {
            double sec = 0;
            size_t bytes = 0;
            g_rec.main.live.ringState(&sec, &bytes);
            wchar_t b[96];
            swprintf_s(b, L"KEEPS THE LAST %.0f MIN \x00B7 %s \x00B7 %.0f MB", g_bb.minutes, Clock(sec).c_str(), bytes / 1e6);
            t = b;
        } else {
            t = L"BLACK BOX OFF";
        }
        s.str(L"text", t);
        s.flag(L"alert", alert);
    }
    s.close();
    // the streams: all of them (the rocker: on while any is), and each target (set = it has a server)
    s.open(L"stream");
    s.flag(L"on", StreamsOn());
    {
        auto word = [](const StreamTarget& t) {
            const auto st = t.pub.state();
            return !t.on ? L"off" : st == rtmp::Publisher::Live ? L"live" : st == rtmp::Publisher::Retrying ? L"retrying" : L"connecting";
        };
        int live = 0, on = 0;
        bool retry = false;
        std::wstring text;
        for (int n = 0; n < kStreams; ++n) {
            const auto& t = g_streams[n];
            if (!t.on) continue;
            ++on;
            const auto st = t.pub.state();
            live += st == rtmp::Publisher::Live;
            retry = retry || st == rtmp::Publisher::Retrying;
            wchar_t b[160];
            std::wstring name = StreamIni(n, L"label");
            if (name.empty()) name = L"Stream " + std::to_wstring(n + 1);
            if (st == rtmp::Publisher::Live) swprintf_s(b, L"%s: live, %.1f Mbit/s", name.c_str(), StreamRate(n));
            else swprintf_s(b, L"%s: %s", name.c_str(), Utf8W(t.pub.text()).c_str());
            text += (text.empty() ? L"" : L"\n") + std::wstring(b);
        }
        s.str(L"state", !on ? L"off" : retry ? L"retrying" : live == on ? L"live" : L"connecting");
        if (on) s.str(L"text", text);  // the LCD's tooltip
        s.open(L"targets", L'[');
        for (int n = 0; n < kStreams; ++n) {
            const auto& t = g_streams[n];
            s.open(nullptr);
            s.flag(L"set", StreamConfigured(n));
            if (const std::wstring l = StreamIni(n, L"label"); !l.empty()) s.str(L"label", l);
            s.str(L"state", word(t));
            if (t.on) {  // the LCD: its rate while live, else its state in a word or two (CONNECTING, NO SERVER ...)
                if (t.pub.state() == rtmp::Publisher::Live) s.num(L"rate", std::round(StreamRate(n) * 10) / 10);
                s.str(L"msg", Utf8W(t.pub.shortText()));
            }
            s.close();
        }
        s.close(L']');
    }
    s.close();
    s.close();
    return s.j;
}

// The state to the page when it changed (force: always).
void ConsolePush(bool force) {
    if (!g_con.on || !g_con.web || !g_con.ready) return;
    const std::wstring j = ConsoleState();
    if (!force && j == g_con.lastState) return;
    g_con.lastState = j;
    g_con.web->PostWebMessageAsJson(j.c_str());
}

// ---------------------------------------------------------------- commands from the page

void ConsoleCommand(const std::wstring& j) {
    const std::wstring cmd = JsonStr(j, L"cmd");
    double n = 0;
    if (g_test.logCommands) ConsoleLog(L"page: " + j);
    if (cmd == L"ready") {  // the page has (re)loaded
        g_con.ready = true;
        ConsolePush(true);
        return;
    }
    if (cmd == L"layout") {  // "rects":{"program":[x,y,w,h],...},"dpr":1
        g_con.rects.clear();
        size_t at = j.find(L"\"rects\":{");
        if (at != std::wstring::npos) {
            at += 9;
            while (at < j.size() && j[at] == L'"') {
                const size_t e = j.find(L'"', at + 1);
                const size_t lb = e == std::wstring::npos ? e : j.find(L'[', e);
                if (lb == std::wstring::npos) break;
                ConsoleRect r;
                swscanf_s(j.c_str() + lb + 1, L"%f,%f,%f,%f", &r.x, &r.y, &r.w, &r.h);
                g_con.rects[j.substr(at + 1, e - at - 1)] = r;
                at = j.find(L']', lb);
                if (at == std::wstring::npos) break;
                if (++at < j.size() && j[at] == L',') ++at;
            }
        }
        if (JsonNum(j, L"dpr", &n) && n > 0.2 && n < 8) g_con.dpr = static_cast<float>(n);
        return;
    }
    if (cmd == L"key") {
        double rep = 0;
        JsonNum(j, L"repeat", &rep);
        if (JsonNum(j, L"vk", &n) && !(KeyNoRepeat(static_cast<int>(n)) && rep > 0.5)) HandleKey(static_cast<WPARAM>(n));
    } else if (cmd == L"source") {  // {id, mon, toggle}: id "deck" = the DECK key; toggle: the key shown again = monitor off
        double mon = 0, toggle = 0;
        JsonNum(j, L"mon", &mon);
        JsonNum(j, L"toggle", &toggle);
        const int cur = mon > 0.5 ? ConsoleSourceOf(g_con.secondKey) : ProgramSource();
        const std::wstring id = JsonStr(j, L"id");
        const int s = id == L"deck" ? ConsoleDeckFor(cur) : ConsoleSourceOf(id);
        if (s >= 0 && s == cur && toggle > 0.5) {  // pressed again: the monitor shows no source
            if (mon > 0.5) ConsoleSetSecond(-1);
            else ConsoleProgramOff();
        } else if (s >= 0 && mon > 0.5) {
            ConsoleSetSecond(s);
        } else if (s >= 0) {
            if (g_rec.replay) ReplayStop();  // the program monitor shows the source picked
            SelectFeed(s);
            ConsoleNoteDeck(s);
            CameraReconcile();
        }
    } else if (cmd == L"view") {  // a monitor's deck position: {mon, dir: -1 / 1} (skips those on other monitors) or {mon, index}
        double mon = 0, idx = 0, dir = 0;
        JsonNum(j, L"mon", &mon);
        const int cur = mon > 0.5 ? ConsoleSourceOf(g_con.secondKey) : ProgramSource();
        if (!IsDeckSource(cur)) return;
        const int count = CameraViewCount(cur - kFeeds), at = g_lcams[cur - kFeeds].pos;
        int k = at;
        if (JsonNum(j, L"index", &idx)) {
            k = static_cast<int>(idx);
        } else {
            JsonNum(j, L"dir", &dir);
            for (int step = 1; step <= count; ++step) {
                k = ((at + (dir < 0 ? -step : step)) % count + count) % count;
                if (!SourceShown(PositionSource(k))) break;
            }
        }
        if (k < 0 || k >= count || k == at) return;
        const int s = PositionSource(k);
        if (mon > 0.5) {
            ConsoleSetSecond(s);
        } else {
            if (g_rec.replay) ReplayStop();
            SelectFeed(s);
            ConsoleNoteDeck(s);
            CameraReconcile();
        }
    } else if (cmd == L"cycle") {  // a monitor's deck cycle: {mon, on} on / off; {mon, sel: "0,3,5"} the positions ticked;
                                   // {mon, seconds} the interval
        double mon = 0, on = 0, sec = 0;
        JsonNum(j, L"mon", &mon);
        const int u = mon > 0.5 ? 1 : 0;
        DeckCycle& c = Cycle(u);
        if (JsonNum(j, L"on", &on)) {
            c.on = on > 0.5;
            if (!c.on) CycleCancel(c);
            c.at = -1;  // the interval starts now
        }
        if (j.find(L"\"sel\"") != std::wstring::npos) {
            const std::wstring sel = JsonStr(j, L"sel");
            std::vector<std::wstring> keys;
            for (size_t a = 0; a <= sel.size();) {
                size_t e = sel.find(L',', a);
                if (e == std::wstring::npos) e = sel.size();
                const int k = e > a ? _wtoi(sel.substr(a, e - a).c_str()) : -1;
                if (k >= 0 && k < CameraCount() - g_firstPos) keys.push_back(SourceKey(PositionSource(k)));
                a = e + 1;
            }
            // positions not in the list now (DCS offers fewer) stay ticked for when they are back
            for (const auto& x : c.keys)
                if (!IsDeckSource(ConsoleSourceOf(x))) keys.push_back(x);
            c.keys = keys;
        }
        if (JsonNum(j, L"seconds", &sec) && sec > 0) {
            c.seconds = std::clamp(sec, kCycleMinSeconds, kCycleMaxSeconds);
            c.at = -1;
        }
        CycleSave(u);
    } else if (cmd == L"preview") {  // {slot, id}: a preview's source ("" = none, "deck" = a deck position)
        if (!JsonNum(j, L"slot", &n)) return;
        const int k = std::clamp(static_cast<int>(n), 0, 3);
        const std::wstring id = JsonStr(j, L"id");
        const int s = id == L"deck" ? ConsoleDeckFor(-1) : ConsoleSourceOf(id);
        g_con.prev[k] = s >= 0 ? SourceKey(s) : L"";
        ConsoleSavePreviews();
        ConsoleNoteDeck(s);
        CameraReconcile();
        ConsolePush();
    } else if (cmd == L"previewTake") {  // {slot}: the preview's source onto monitor A, A's into the preview
        if (!JsonNum(j, L"slot", &n)) return;
        const int k = std::clamp(static_cast<int>(n), 0, 3), s = ConsoleSourceOf(g_con.prev[k]);
        if (s < 0) return;
        const int a = ProgramSource();
        g_con.prev[k] = a >= 0 ? SourceKey(a) : L"";
        ConsoleSavePreviews();
        if (g_rec.replay) ReplayStop();
        SelectFeed(s);
        CameraReconcile();
        ConsolePush();
    } else if (cmd == L"wall") {
        const std::wstring m = JsonStr(j, L"mode");
        ConsoleSetWall(m == L"1" ? 0 : m == L"q" ? 2 : 1);
    } else if (cmd == L"swap") {  // monitors A and B change pictures
        const int a = ProgramSource(), b = ConsoleSecond();
        if (g_con.wall == 1 && b != a) {  // either may show no source
            if (b >= 0) {
                if (g_rec.replay) ReplayStop();
                SelectFeed(b);
            } else {
                ConsoleProgramOff();
            }
            ConsoleSetSecond(a);
        }
    } else if (cmd == L"ship") {
        ChooseCarrier(_wtoi(JsonStr(j, L"id").c_str()));
    } else if (cmd == L"camera") {
        const int i = _wtoi(JsonStr(j, L"id").c_str());
        SetCamera(i, !((g_cam.want >> i) & 1));
    } else if (cmd == L"auto") {
        const int src = ConsoleMonSource(j);
        if (src < 0) return;
        Settings& t = GroupRef(GroupOf(src));
        t.autoLevels = !t.autoLevels;
        ResetAuto();
        GroupChanged(GroupOf(src));
    } else if (cmd == L"symb") {
        app.set.symbology = !app.set.symbology;
        Changed();
    } else if (cmd == L"reset") {
        const int src = ConsoleMonSource(j);
        if (src < 0) return;
        Settings& t = GroupRef(GroupOf(src));
        Settings def;
        def.autoLevels = t.autoLevels, def.symbology = t.symbology, def.proc = t.proc, def.enabled = t.enabled;
        t = def;
        GroupChanged(GroupOf(src));
    } else if (cmd == L"cctv") {
        g_cctv.on = !g_cctv.on;
        Changed();
    } else if (cmd == L"recOnly") {
        g_cctv.recOnly = !g_cctv.recOnly;
        Changed();
    } else if (cmd == L"knob" && JsonNum(j, L"value", &n)) {
        const std::wstring k = JsonStr(j, L"key");
        const float x = static_cast<float>(n);
        if (k == L"str") {
            g_cctv.strength = std::clamp(x / 100.0f, 0.0f, 2.0f);
            Changed();
            return;
        }
        const int src = ConsoleMonSource(j);
        if (src < 0) return;
        Settings& t = GroupRef(GroupOf(src));
        if (k == L"exp") t.stops = std::clamp(x, -4.0f, 8.0f);
        else if (k == L"bri") t.brightness = std::clamp(x, -0.5f, 0.5f);
        else if (k == L"con") t.contrast = std::clamp(x, 0.25f, 16.0f);
        else if (k == L"gam") t.gamma = std::clamp(x, 0.3f, 4.0f);
        GroupChanged(GroupOf(src));
    } else if (cmd == L"preset" && JsonNum(j, L"index", &n)) {
        double save = 0;
        JsonNum(j, L"save", &save);
        const int i = std::clamp(static_cast<int>(n), 0, kPresets - 1);
        if (save > 0.5) StorePreset(i);
        else LoadPreset(i);
    } else if (cmd == L"presetClear" && JsonNum(j, L"index", &n)) {  // the page's right-click menu (and rename)
        ClearPreset(std::clamp(static_cast<int>(n), 0, kPresets - 1));
    } else if (cmd == L"keys" && JsonNum(j, L"page", &n)) {  // a pop-up or dialog in the page: it takes the keys
        g_con.pageKeys = n > 0.5;
        if (g_con.pageKeys && g_con.controller) g_con.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    } else if (cmd == L"rename" && JsonNum(j, L"index", &n)) {
        RenamePreset(std::clamp(static_cast<int>(n), 1, kPresets - 1), JsonStr(j, L"name").substr(0, 10));
    } else if (cmd == L"mode" && JsonNum(j, L"auto", &n)) {
        SetAutoMode(n > 0.5);
    } else if (cmd == L"comp") {
        SetRecComposite(!g_rec.composite);
    } else if (cmd == L"autoFilter") {  // {value: 'own' | 'section' | 'flight' | 'all'}
        SetAutoFilter(AutoFilterFromKey(JsonStr(j, L"value").c_str()));
    } else if (cmd == L"rec") {
        ToggleRecording();
    } else if (cmd == L"play") {
        ToggleReplay();
    } else if (cmd == L"stop") {
        if (g_rec.replay) ReplayStop();
        else RecStopManual();
    } else if (cmd == L"save") {
        ReplaySave();
    } else if (cmd == L"still") {
        StillTake();
    } else if (cmd == L"folder") {
        OpenVideoFolder();
    } else if (cmd == L"bb") {
        SetBlackBox(!g_bb.on);
    } else if (cmd == L"bbSave") {
        BlackBoxSaveNow();
    } else if (cmd == L"stream" && JsonNum(j, L"on", &n)) {  // {on}: all; {n, on}: stream n (0..3)
        double t = 0;
        if (JsonNum(j, L"n", &t)) SetStreamTarget(static_cast<int>(t), n > 0.5);
        else SetStream(n > 0.5);
        ConsolePush();
    } else if (cmd == L"settings") {  // the settings dialog (the file itself: OPEN FILE in it)
        g_con.settingsOpen = true;
        if (g_con.controller) g_con.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);  // its fields take the keyboard
        ++g_con.formRev;
        g_con.formMessage.clear();
        g_con.formRestart = false;
    } else if (cmd == L"guide") {  // the title bar's "?"
        OpenGuide();
    } else if (cmd == L"benchLamp") {  // the benchmark lamp after a run: its report, and the lamp goes out
        BenchLampClick();
        ConsolePush();
    } else if (cmd == L"settingsClose") {
        g_con.settingsOpen = false;
    } else if (cmd == L"settingsSave") {
        SettingsSave(j);
    } else if (cmd == L"settingsAction") {
        SettingsAction(JsonStr(j, L"id"), JsonStr(j, L"key"));
    } else if (cmd == L"recArm") {  // id "deck": the deck positions' switch
        const std::wstring id = JsonStr(j, L"id");
        const int s = id == L"deck" ? (CameraCount() > g_firstPos ? PositionSource(0) : -1) : ConsoleSourceOf(id);
        if (s >= 0) SetRecArm(s, !RecArmed(s));
    } else if (cmd == L"shipStep" && JsonNum(j, L"dir", &n)) {
        StepCarrier(n < 0 ? -1 : 1);
    } else if (cmd == L"ptzTarget") {  // {cam, target}: what AUTO follows
        const int s = ConsoleSourceOf(JsonStr(j, L"cam"));
        if (s < kFeeds || IsHangar(s - kFeeds) || !JsonNum(j, L"target", &n) || n < kTrackFirst || n > kTrackLast) return;
        TrackOf(s - kFeeds).target = static_cast<int>(n);
        SaveTrack(s - kFeeds);
        ConsolePush();
    } else if (cmd == L"tune" || cmd == L"tuneStep" || cmd == L"tuneReset" || cmd == L"tuneCopy") {  // tuning.inc
        TuneCommand(cmd, j, cmd == L"tuneStep" ? -1 : ConsoleSourceOf(JsonStr(j, L"cam")));
    } else if (cmd == L"ptzFlir") {  // experimental: this camera's picture in DCS's infrared (FLIR) or normal
        const int s = ConsoleSourceOf(JsonStr(j, L"cam"));
        if (!g_expFlir || s < kFeeds || s - kFeeds >= 16) return;
        g_flirOn[s - kFeeds] = !g_flirOn[s - kFeeds];
        RecStatus(CameraTag(s - kFeeds) + (g_flirOn[s - kFeeds] ? L": FLIR" : L": normal picture"), 3000);
    } else if (cmd == L"ptz" || cmd == L"ptzReset" || cmd == L"ptzRest" || cmd == L"ptzAuto") {
        // the joystick (rates for the DLL, stamped: stale after 0.3 s); REST = back to the rest view and stay there;
        // AUTO = back to the rest view and follow the target (ptzReset: the rest view, AUTO as it was)
        const int s = ConsoleSourceOf(JsonStr(j, L"cam"));
        if (cmd == L"ptzAuto" && s >= kFeeds && IsHangar(s - kFeeds)) return;  // a hangar position never tracks
        if (s >= kFeeds && (cmd == L"ptzRest" || cmd == L"ptzAuto")) {
            TrackOf(s - kFeeds).armed = cmd == L"ptzAuto";
            SaveTrack(s - kFeeds);
        }
        const int slot = s >= kFeeds ? PhysOf(s - kFeeds) : -1;  // its render slot
        if (slot < 0 || !app.headerW) return;
        auto& c = app.headerW->ptz[slot];
        double p = 0, t = 0, z = 0;
        if (cmd == L"ptz") JsonNum(j, L"pan", &p), JsonNum(j, L"tilt", &t), JsonNum(j, L"zoom", &z);
        if (cmd == L"ptz" && (p != 0 || t != 0 || z != 0)) CycleHold(s, video::Now());  // a cycle on its monitor stays
        c.pan = static_cast<float>(std::clamp(p, -1.0, 1.0));
        c.tilt = static_cast<float>(std::clamp(t, -1.0, 1.0));
        c.zoom = static_cast<float>(std::clamp(z, -1.0, 1.0));
        MemoryBarrier();
        c.tick = static_cast<LONG64>(GetTickCount64());
        if (cmd != L"ptz") InterlockedIncrement(&c.resetSeq);
        if (cmd == L"ptz") return;  // 10 a second while held: the state follows with the regular push
    } else if (cmd == L"drag") {  // fallback (the title bar is normally the window's caption: ConsoleHitTest)
        if (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) {
            if (g_con.comp) g_con.comp->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, POINT{0, 0});
            ReleaseCapture();
            SendMessageW(app.hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return;
    } else if (cmd == L"win") {
        const std::wstring what = JsonStr(j, L"what");
        if (what == L"min") ShowWindow(app.hwnd, SW_MINIMIZE);
        else if (what == L"max") ShowWindow(app.hwnd, IsZoomed(app.hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        else if (what == L"close") PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
        return;
    }
    ConsolePush();
}

// ---------------------------------------------------------------- window: mouse, hit testing, placement

// Mouse messages of the window go to the page. True = handled.
bool ConsoleMouse(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!g_con.on || !g_con.comp) return false;
    POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
    UINT32 data = 0;
    if (m == WM_MOUSEWHEEL || m == WM_MOUSEHWHEEL) {
        ScreenToClient(h, &p);  // wheel positions are in screen coordinates
        data = static_cast<UINT32>(static_cast<int>(GET_WHEEL_DELTA_WPARAM(w)));
    } else if (m == WM_XBUTTONDOWN || m == WM_XBUTTONUP || m == WM_XBUTTONDBLCLK) {
        data = GET_XBUTTON_WPARAM(w);
    }
    switch (m) {
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
            SetCapture(h);  // drags (the knobs) keep working outside the window
            // The keyboard goes to the page only once WebView2 has the focus (visual hosting does not take it on a
            // click by itself): typing in the settings dialog. The page passes the shortcuts on.
            if (g_con.controller) g_con.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            break;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
            if (!(GET_KEYSTATE_WPARAM(w) & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2))) ReleaseCapture();
            break;
        case WM_MOUSEMOVE:
            if (!g_con.tracking) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                g_con.tracking = true;
            }
            break;
        case WM_MOUSELEAVE:
            g_con.tracking = false;
            break;
        default: break;
    }
    const auto vk = static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(m == WM_MOUSELEAVE ? 0 : GET_KEYSTATE_WPARAM(w));
    g_con.comp->SendMouseInput(static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(m), vk, data, p);
    return true;
}

// Window edges resize; the page's title bar (not its buttons) moves the window like a caption; the rest is the page.
LRESULT ConsoleHitTest(POINT screen) {
    const LRESULT edge = FrameHitTest(screen);
    if (edge != HTCAPTION && !IsZoomed(app.hwnd)) return edge;
    POINT c = screen;
    ScreenToClient(app.hwnd, &c);
    const float x = c.x / g_con.dpr, y = c.y / g_con.dpr;
    auto in = [&](const wchar_t* id) {
        const auto it = g_con.rects.find(id);
        if (it == g_con.rects.end()) return false;
        const ConsoleRect& r = it->second;
        return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    };
    if (g_con.rects.find(L"titlebar") == g_con.rects.end()) return y < 40 ? HTCAPTION : HTCLIENT;  // page still loading
    return in(L"titlebar") && !in(L"winbtns") ? HTCAPTION : HTCLIENT;
}

// The console window's placement ([console] x, y, w, h, max).
void ConsoleSavePlacement() {
    if (IsIconic(app.hwnd) || g_test.offscreen) return;
    const bool zoomed = IsZoomed(app.hwnd) != 0;
    WritePrivateProfileStringW(L"console", L"max", zoomed ? L"1" : L"0", app.iniPath.c_str());
    if (zoomed) return;
    RECT wr;
    GetWindowRect(app.hwnd, &wr);
    for (auto [k, v] : {std::pair<const wchar_t*, LONG>{L"x", wr.left}, {L"y", wr.top}, {L"w", wr.right - wr.left}, {L"h", wr.bottom - wr.top}})
        WritePrivateProfileStringW(L"console", k, std::to_wstring(v).c_str(), app.iniPath.c_str());
}

// First run: 1600 x 1000 (at most 92 % of the screen), centred.
void ConsoleRestorePlacement(bool* maximise) {
    const int x = GetPrivateProfileIntW(L"console", L"x", INT_MIN, app.iniPath.c_str());
    const int y = GetPrivateProfileIntW(L"console", L"y", INT_MIN, app.iniPath.c_str());
    int w = GetPrivateProfileIntW(L"console", L"w", 0, app.iniPath.c_str());
    int h = GetPrivateProfileIntW(L"console", L"h", 0, app.iniPath.c_str());
    if (maximise) *maximise = GetPrivateProfileIntW(L"console", L"max", 0, app.iniPath.c_str()) != 0;
    POINT probe{x == INT_MIN ? 0 : x + 40, y == INT_MIN ? 0 : y + 20};
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(probe, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& wa = mi.rcWork;
    const int ww = wa.right - wa.left, wh = wa.bottom - wa.top;
    const UINT dpi = GetDpiForWindow(app.hwnd) ? GetDpiForWindow(app.hwnd) : 96;
    if (w <= 0 || h <= 0) {
        w = std::min(MulDiv(1600, static_cast<int>(dpi), 96), ww * 92 / 100);
        h = std::min(MulDiv(1000, static_cast<int>(dpi), 96), wh * 92 / 100);
    }
    w = std::clamp(w, std::min(MulDiv(kConsoleMinW, static_cast<int>(dpi), 96), ww), ww);
    h = std::clamp(h, std::min(MulDiv(kConsoleMinH, static_cast<int>(dpi), 96), wh), wh);
    int left = x == INT_MIN ? wa.left + (ww - w) / 2 : std::clamp(x, static_cast<int>(wa.left), static_cast<int>(wa.right) - w);
    int top = y == INT_MIN ? wa.top + (wh - h) / 2 : std::clamp(y, static_cast<int>(wa.top), static_cast<int>(wa.bottom) - h);
    if (g_test.offscreen) left = -w - 4000, top = 50;  // tests: rendered, but nowhere on the screen
    SetWindowPos(app.hwnd, nullptr, left, top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Starts the console, once, before the window is shown: the DirectComposition tree, the WebView (created
// asynchronously) and the window's placement. False: it cannot start (ILARTS says so and closes).
bool ConsoleActivate() {
    if (!ConsoleStart()) return false;
    g_con.on = true;
    g_con.target->SetRoot(g_con.root);
    g_con.dcomp->Commit();
    // the console has no ADJ switch (removed 2026-11-14): every picture's adjustments are on
    app.set.enabled = true;
    for (auto& gs : g_groupSet) gs.enabled = true;
    ConsoleRestorePlacement(nullptr);  // maximised: wWinMain shows it so
    ConsoleResize();
    return true;
}

// ---------------------------------------------------------------- tests

// --test-script file: lines "seconds<TAB>javascript" run in the page, "seconds<TAB>@vk" press a key, or
// "seconds<TAB>!name" to save the page
// (<log>.name.png, through WebView2), the pictures (<log>.name.bmp, the back buffer), the window
// (<log>.name.window.bmp), the monitor rectangles and the state (<log>.name.txt).
void ConsoleTestShot(const std::wstring& name) {
    const std::wstring base = g_test.log + L"." + name;
    g_shotPath = base + L".bmp";
    SaveWindowCapture(base + L".window.bmp");
    FILE* f = nullptr;
    if (_wfopen_s(&f, (base + L".txt").c_str(), L"w, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"dpr %.3f\n", g_con.dpr);
        for (const auto& [id, r] : g_con.rects) fwprintf(f, L"rect %s %.2f %.2f %.2f %.2f\n", id.c_str(), r.x, r.y, r.w, r.h);
        fwprintf(f, L"state %s\n", ConsoleState().c_str());
        fclose(f);
    }
    if (!g_con.web) return;
    IStream* st = nullptr;
    if (FAILED(SHCreateStreamOnFileEx((base + L".png").c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &st))) return;
    auto* done = new ComHandler<ICoreWebView2CapturePreviewCompletedHandler, HRESULT>([st](HRESULT hr) -> HRESULT {
        st->Release();
        ConsoleLog(SUCCEEDED(hr) ? L"page captured" : L"page capture failed");
        return S_OK;
    });
    if (FAILED(g_con.web->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, st, done))) st->Release();
    done->Release();
}

void ConsoleTestTick(double t) {
    while (g_test.scriptAt < g_test.script.size() && t >= g_test.script[g_test.scriptAt].first) {
        const std::wstring line = g_test.script[g_test.scriptAt++].second;
        ConsoleLog(L"script: " + line);
        if (!line.empty() && line[0] == L'!') {
            ConsoleTestShot(line.substr(1));
        } else if (!line.empty() && line[0] == L'@') {  // "@113" = the key with virtual-key code 113 (F2)
            HandleKey(static_cast<WPARAM>(_wtoi(line.c_str() + 1)));
        } else if (!line.empty() && (line[0] == L'^' || line[0] == L'?')) {
            // "^x,y": a left click posted to the window at client pixel x, y (through ConsoleMouse to the page);
            // "?x,y": log what the window's hit test says there
            int x = 0, y = 0;
            swscanf_s(line.c_str() + 1, L"%d,%d", &x, &y);
            if (line[0] == L'^') {
                PostMessageW(app.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
                PostMessageW(app.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
                PostMessageW(app.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            } else {
                POINT p{x, y};
                ClientToScreen(app.hwnd, &p);
                ConsoleLog(L"hit test " + line.substr(1) + L" = " + std::to_wstring(SendMessageW(app.hwnd, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y))));
            }
        } else if (g_con.web) {
            auto* done = new ComHandler<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>([](HRESULT hr, LPCWSTR result) -> HRESULT {
                ConsoleLog(std::wstring(L"script result ") + (SUCCEEDED(hr) ? L"" : L"(failed) ") + (result ? result : L""));
                return S_OK;
            });
            g_con.web->ExecuteScript(line.c_str(), done);
            done->Release();
        }
    }
}
