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
// Without WebView2 ILARTS says so and offers the download page; if WebView2 fails while running, ILARTS says so
// and closes. (The classic sidebar is only for the development tests: --ui classic.)

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
// type: text, password, number, percent (number shown x100), bool, select, path. restart = read at start only.
struct SettingDef {
    const wchar_t *group, *section, *key, *label, *type, *def;
    double lo, hi, step;
    const wchar_t *unit, *options, *hint;
    bool restart;
};
const SettingDef kSettings[] = {
    {L"Recording", L"video", L"folder", L"Folder", L"path", L"", 0, 0, 0, L"", L"", L"Recordings, replays and black box files", false},
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
    {L"CCTV look", L"cctv", L"on", L"CCTV look", L"bool", L"0", 0, 0, 0, L"", L"", L"Scan lines, noise, lens distortion and vignette, as on the ship's monitors (key C)", false},
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
    {L"Stream", L"stream", L"bitrate_kbps", L"Bit rate", L"number", L"2500", 300, 20000, 100, L"kbit/s", L"", L"", true},
    {L"Stream", L"stream", L"composite_kbps", L"Composite bit rate", L"number", L"6000", 1000, 30000, 100, L"kbit/s", L"", L"1920 \x00D7 1080", false},
    {L"Stream", L"stream", L"audio", L"Silent audio track", L"bool", L"1", 0, 0, 0, L"", L"", L"Some services refuse video without audio", false},
    {L"Stream 1", L"stream", L"url", L"Server", L"text", L"", 0, 0, 0, L"", L"", L"rtmp://server/app (plain RTMP)", false},
    {L"Stream 1", L"stream", L"key", L"Stream key", L"password", L"", 0, 0, 0, L"", L"", L"", false},
    {L"Stream 1", L"stream", L"source", L"Source", L"select", L"a", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;composite|Composite;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"", false},
    {L"Stream 1", L"stream", L"tiles", L"Composite", L"multi", L"a,b", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"Up to four, arranged by their number", false},
    {L"Stream 2", L"stream2", L"url", L"Server", L"text", L"", 0, 0, 0, L"", L"", L"rtmp://server/app (plain RTMP)", false},
    {L"Stream 2", L"stream2", L"key", L"Stream key", L"password", L"", 0, 0, 0, L"", L"", L"", false},
    {L"Stream 2", L"stream2", L"source", L"Source", L"select", L"a", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;composite|Composite;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"", false},
    {L"Stream 2", L"stream2", L"tiles", L"Composite", L"multi", L"a,b", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"Up to four, arranged by their number", false},
    {L"Stream 3", L"stream3", L"url", L"Server", L"text", L"", 0, 0, 0, L"", L"", L"rtmp://server/app (plain RTMP)", false},
    {L"Stream 3", L"stream3", L"key", L"Stream key", L"password", L"", 0, 0, 0, L"", L"", L"", false},
    {L"Stream 3", L"stream3", L"source", L"Source", L"select", L"a", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;composite|Composite;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"", false},
    {L"Stream 3", L"stream3", L"tiles", L"Composite", L"multi", L"a,b", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"Up to four, arranged by their number", false},
    {L"Stream 4", L"stream4", L"url", L"Server", L"text", L"", 0, 0, 0, L"", L"", L"rtmp://server/app (plain RTMP)", false},
    {L"Stream 4", L"stream4", L"key", L"Stream key", L"password", L"", 0, 0, 0, L"", L"", L"", false},
    {L"Stream 4", L"stream4", L"source", L"Source", L"select", L"a", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;composite|Composite;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"", false},
    {L"Stream 4", L"stream4", L"tiles", L"Composite", L"multi", L"a,b", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"Up to four, arranged by their number", false},
    // outputs: Spout senders and virtual cameras (outputs.inc); "action": a key (value "id|LABEL")
    {L"Outputs", L"outputs", L"spout", L"Spout", L"multi", L"", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"For OBS (Spout2 plugin) and other Spout receivers", false},
    {L"Outputs", L"outputs", L"cameras", L"Virtual cameras", L"multi", L"", 0, 0, 0, L"", L"a|Monitor A;b|Monitor B;plat|PLAT;lso1|LSO 1;lso2|LSO 2;island|Island", L"Webcams for Discord, Teams, OBS ... (Windows 11)", false},
    {L"Outputs", L"outputs", L"component", L"Camera component", L"action", L"", 0, 0, 0, L"", L"", L"Installs the virtual cameras for every user (administrator)", false},
    {L"Installation", L"install", L"repair", L"Check and repair", L"action", L"", 0, 0, 0, L"", L"", L"After a DCS update or if something stopped working: restores the hook and permissions, and checks the DCS version", false},
    {L"Installation", L"install", L"permissions", L"DCS permissions", L"action", L"", 0, 0, 0, L"", L"", L"The two lines in Saved Games\\DCS\\Config\\autoexec.cfg the hook needs (single player, hosting). REMOVE before you uninstall with OMM or OvGME", false},
    {L"Experimental", L"experimental", L"flir", L"FLIR view", L"bool", L"0", 0, 0, 0, L"", L"",
     L"A FLIR key on each camera's PTZ panel: DCS's infrared picture for that camera. Experimental, and costly: a camera in FLIR can cut DCS's frame rate considerably", false},
    {L"Window", L"viewer", L"auto_start", L"Start with DCS", L"bool", L"1", 0, 0, 0, L"", L"", L"Opens ILARTS when a mission starts, if it is not open already", false},
    {L"Window", L"viewer", L"antialias", L"Anti-aliasing", L"bool", L"1", 0, 0, 0, L"", L"", L"", false},
};

std::wstring IniGet(const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    wchar_t b[1024] = {};
    GetPrivateProfileStringW(section, key, def, b, 1024, app.iniPath.c_str());
    return b;
}

bool AutoexecHasBlock();  // repair.inc
std::wstring SettingValue(const SettingDef& d) {
    if (!wcscmp(d.section, L"install") && !wcscmp(d.key, L"repair")) return L"repair|REPAIR";
    if (!wcscmp(d.section, L"install") && !wcscmp(d.key, L"permissions")) return AutoexecHasBlock() ? L"perm_remove|REMOVE" : L"perm_add|ADD";
    if (!wcscmp(d.section, L"outputs") && !wcscmp(d.key, L"component"))
        return g_vcamSys.installer ? L"busy|\x2026" : !VCamApi() ? L"none|WINDOWS 11" : VCamInstalled() ? L"cam_remove|REMOVE" : L"cam_install|INSTALL";
    if (!wcscmp(d.section, L"video") && !wcscmp(d.key, L"folder")) return VideoFolder();
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
        if (!group || wcscmp(group, d.group) != 0) {
            if (group) s.close(L']'), s.close();
            group = d.group;
            s.open(nullptr);
            s.str(L"title", d.group);
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
        if (type == L"number" || type == L"percent") s.num(L"min", d.lo), s.num(L"max", d.hi), s.num(L"step", d.step);
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
    g_cctv.on = num(L"cctv", L"on", 0) > 0.5;
    g_cctv.strength = static_cast<float>(std::clamp(num(L"cctv", L"strength", 1), 0.0, 2.0));
    g_cctv.recOnly = num(L"cctv", L"recordings_only", 0) > 0.5;
    g_cctv.grain = static_cast<float>(std::clamp(num(L"cctv", L"grain", 1), 0.0, 4.0));
    g_cctv.scan = static_cast<float>(std::clamp(num(L"cctv", L"scanlines", 1), 0.0, 4.0));
    g_cctv.distort = static_cast<float>(std::clamp(num(L"cctv", L"distortion", 1), 0.0, 4.0));
    g_cctv.vignette = static_cast<float>(std::clamp(num(L"cctv", L"vignette", 1), 0.0, 4.0));
    g_antialias = num(L"viewer", L"antialias", 1) > 0.5;
    g_expFlir = num(L"experimental", L"flir", 0) > 0.5;
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
std::wstring PermissionsNow(bool add);  // repair.inc
void SettingsAction(const std::wstring& id, const std::wstring& key) {
    if (id == L"openFile") {
        OpenSettingsFile();
    } else if (id == L"browse" && key == L"video.folder") {
        const std::wstring f = FileDialog(false, true, L"Folder for recordings", nullptr);
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
bool ConsoleAvailable() {
    LPWSTR ver = nullptr;
    const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver);
    const bool ok = SUCCEEDED(hr) && ver && ver[0];
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
            else g_con.ready = false;  // (re)loading: wait for the page's 'ready'
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
            if (g_con.settingsOpen) return S_OK;  // typing in the settings dialog: the page's keys
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return S_OK;
            if (app.debug && (vk == VK_F12 || (vk == 'I' && (GetKeyState(VK_CONTROL) & 0x8000)))) return S_OK;  // DevTools
            COREWEBVIEW2_PHYSICAL_KEY_STATUS ks{};
            args->get_PhysicalKeyStatus(&ks);
            args->put_Handled(TRUE);
            if (vk == 'N' && ks.WasKeyDown) return S_OK;  // no auto-repeat for the carrier step
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
    if (v2.main && ConsoleRectOf(L"second", &r)) DrawFeed(v2, app.rtv, ConsoleFit(r, ViewAspect(v2)));
    for (int k = 0; k < 4; ++k) {
        const int i = ConsoleSourceOf(g_con.prev[k]);
        if (i < 0 || !ConsoleRectOf((L"p" + std::to_wstring(k)).c_str(), &r)) continue;
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
// One monitor of the wall: what it shows, its message, and the levels and PTZ of that picture (the levels follow
// the classic sidebar's rules: none for LSO 2; exposure only in the Pipeline mode).
void ConsoleMonitor(Json& s, int src, bool program) {
    const bool valid = src >= 0 && src < FeedCount(), compatBad = HeaderValid() && app.header->compat.state == 3;
    const ViewState v = valid ? ViewFor(src) : ViewState{};
    const bool camFeed = valid && src >= kFeeds, picture = v.main != nullptr, replay = program && g_rec.replay;
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
    if (camFeed) s.str(L"burnName", SourceTag(src));
    s.flag(L"rec", valid && RecArmed(src));
    // a deck position: all the deck positions (the view selector) and which one this is
    if (camFeed && CameraViewCount(src - kFeeds) > 0) {
        const int i = src - kFeeds;
        const int k = CameraViewIndex(i, CameraViewName(i));
        s.open(L"views", L'[');
        for (int n = 0; n < CameraViewCount(i); ++n) s.str(nullptr, CameraViewNameAt(i, n));
        s.close(L']');
        s.num(L"view", k);
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
    s.str(L"status", compatBad ? L"DCS NOT COMPATIBLE" : !HeaderValid() ? L"WAITING FOR DCS" : ProgramSource() < 0 ? L"NO SOURCE" : progName + L" \x00B7 " + (picture ? res : L"NO SIGNAL"));
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
        s.str(L"key", i < 9 ? std::to_wstring(i + 1) : L"");
        if (IsDeckSource(i)) s.str(L"group", L"deck");  // one DECK key; the view selector picks the position
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
        for (auto& ch : hull)
            if (ch == L'_') ch = L'-';
        s.open(nullptr);
        s.str(L"id", std::to_wstring(i));
        s.str(L"unit", Upper(Ascii(c.name, sizeof(c.name))));
        s.str(L"hull", hull);
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
    std::wstring l1, l2, counter = L"00:00:00", replayTime;
    if (g_rec.replay && !g_rec.frames.empty()) {
        const double dur = g_rec.frames.back()->t - g_rec.frames.front()->t;
        replayTime = L"\x2212" + Hms(dur - g_rec.pos).substr(3);
        l1 = L"REPLAY \x00B7 " + Upper(g_rec.replayLabel) + L" \x00B7 " + replayTime;
        if (g_rec.speed != 1.0) l1 += g_rec.speed == 0.5 ? L" \x00B7 \x00BD SPEED" : L" \x00B7 \x00BC SPEED";
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
    s.flag(L"rec", Recording());
    s.flag(L"play", g_rec.replay);
    s.str(L"replayTime", replayTime);
    s.flag(L"canSave", g_rec.replay);
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
            wchar_t b[96];
            if (st == rtmp::Publisher::Live) swprintf_s(b, L"%d \x00B7 %.1f MBIT/S", n + 1, StreamRate(n));
            else swprintf_s(b, L"%d \x00B7 %s", n + 1, Upper(Utf8W(t.pub.text())).c_str());
            text += (text.empty() ? L"" : L"\n") + std::wstring(b);
        }
        s.str(L"state", !on ? L"off" : retry ? L"retrying" : live == on ? L"live" : L"connecting");
        if (on) s.str(L"text", text);
        s.open(L"targets", L'[');
        for (int n = 0; n < kStreams; ++n) {
            s.open(nullptr);
            s.flag(L"set", StreamConfigured(n));
            s.str(L"state", word(g_streams[n]));
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

// Right-click on a preset: save / rename / clear (Night is fixed). The new name is asked for by the page.
void ConsolePresetMenu(int slot) {
    HMENU m = CreatePopupMenu();
    if (slot == kNightPreset) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Night is a fixed preset");
    } else {
        AppendMenuW(m, MF_STRING, 1, (L"Save the current look to " + PresetName(slot)).c_str());
        AppendMenuW(m, MF_STRING, 2, L"Rename\x2026");
        AppendMenuW(m, MF_STRING | (g_presets[slot].saved ? 0 : MF_GRAYED), 3, L"Clear");
    }
    POINT p;
    GetCursorPos(&p);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, p.x, p.y, 0, app.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) {
        StorePreset(slot);
    } else if (cmd == 2 && g_con.web) {
        Json a;
        a.open(nullptr);
        a.str(L"ask", L"rename");
        a.num(L"index", slot);
        a.str(L"name", g_presets[slot].name);
        a.close();
        g_con.web->PostWebMessageAsJson(a.j.c_str());
    } else if (cmd == 3) {
        ClearPreset(slot);
    }
}

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
        if (JsonNum(j, L"vk", &n) && !(static_cast<int>(n) == 'N' && rep > 0.5)) HandleKey(static_cast<WPARAM>(n));
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
    } else if (cmd == L"presetMenu" && JsonNum(j, L"index", &n)) {
        ConsolePresetMenu(std::clamp(static_cast<int>(n), 0, kPresets - 1));
    } else if (cmd == L"rename" && JsonNum(j, L"index", &n)) {
        RenamePreset(std::clamp(static_cast<int>(n), 1, kPresets - 1), JsonStr(j, L"name").substr(0, 10));
    } else if (cmd == L"mode" && JsonNum(j, L"auto", &n)) {
        SetAutoMode(n > 0.5);
    } else if (cmd == L"rec") {
        ToggleRecording();
    } else if (cmd == L"play") {
        ToggleReplay();
    } else if (cmd == L"stop") {
        if (g_rec.replay) ReplayStop();
        else RecStopManual();
    } else if (cmd == L"save") {
        ReplaySave();
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
        if (s < kFeeds || !JsonNum(j, L"target", &n) || n < kTrackFirst || n > kTrackLast) return;
        TrackOf(s - kFeeds).target = static_cast<int>(n);
        SaveTrack(s - kFeeds);
        ConsolePush();
    } else if (cmd == L"ptzFlir") {  // experimental: this camera's picture in DCS's infrared (FLIR) or normal
        const int s = ConsoleSourceOf(JsonStr(j, L"cam"));
        if (!g_expFlir || s < kFeeds || s - kFeeds >= 16) return;
        g_flirOn[s - kFeeds] = !g_flirOn[s - kFeeds];
        RecStatus(CameraTag(s - kFeeds) + (g_flirOn[s - kFeeds] ? L": FLIR" : L": normal picture"), 3000);
    } else if (cmd == L"ptz" || cmd == L"ptzReset" || cmd == L"ptzRest" || cmd == L"ptzAuto") {
        // the joystick (rates for the DLL, stamped: stale after 0.3 s); REST = back to the rest view and stay there;
        // AUTO = back to the rest view and follow the target (ptzReset: the rest view, AUTO as it was)
        const int s = ConsoleSourceOf(JsonStr(j, L"cam"));
        if (s >= kFeeds && (cmd == L"ptzRest" || cmd == L"ptzAuto")) {
            TrackOf(s - kFeeds).armed = cmd == L"ptzAuto";
            SaveTrack(s - kFeeds);
        }
        const int slot = s >= kFeeds ? PhysOf(s - kFeeds) : -1;  // its render slot
        if (slot < 0 || !app.headerW) return;
        auto& c = app.headerW->ptz[slot];
        double p = 0, t = 0, z = 0;
        if (cmd == L"ptz") JsonNum(j, L"pan", &p), JsonNum(j, L"tilt", &t), JsonNum(j, L"zoom", &z);
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

// The console window's own placement ([console] x, y, w, h, max); the classic window keeps [window].
void ConsoleSavePlacement() {
    if (IsIconic(app.hwnd) || !app.snapshotPath.empty() || g_uiSelftest || g_test.offscreen) return;
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

// Console on / off (at start; the development tests use the classic window). The window's swap chain changes kind, so the
// device is rebuilt (Update does it on the next loop, as after a driver reset).
void ConsoleActivate(bool on) {
    if (on == g_con.on) return;
    if (on && !g_con.dcomp && !ConsoleStart()) return;  // cannot start: stays classic
    if (app.hwnd && IsWindowVisible(app.hwnd)) {
        if (g_con.on) ConsoleSavePlacement();
        else SaveWindowPlacement();
    }
    g_con.on = on;
    if (g_con.target) g_con.target->SetRoot(on ? g_con.root : nullptr);
    if (g_con.controller) g_con.controller->put_IsVisible(on ? TRUE : FALSE);
    if (g_con.dcomp) g_con.dcomp->Commit();
    if (app.dev) CloseDevice();
    g_con.rects.clear();
    g_con.lastState.clear();
    if (on) {
        // the console has no ADJ switch (removed 2026-11-14): every picture's adjustments are on in it
        if (!app.set.enabled) {
            app.set.enabled = true;
            Changed();
        }
        for (auto& gs : g_groupSet) gs.enabled = true;
        ShowWindow(app.video, SW_HIDE);
        ShowWindow(app.sidebar, SW_HIDE);
        bool max = false;
        if (IsZoomed(app.hwnd)) ShowWindow(app.hwnd, SW_RESTORE);
        ConsoleRestorePlacement(&max);
        if (max && IsWindowVisible(app.hwnd)) ShowWindow(app.hwnd, SW_MAXIMIZE);
        if (g_con.ready) {
            ConsolePush(true);
            if (g_con.web) g_con.web->ExecuteScript(L"document.querySelector('ilarts-console')._rectSig='';document.querySelector('ilarts-console')._layout()", nullptr);
        }
    } else {
        ShowWindow(app.video, SW_SHOW);
        if (IsZoomed(app.hwnd)) ShowWindow(app.hwnd, SW_RESTORE);
        RestoreWindowPlacement();
        ApplyAspect(true);
        if (g_test.offscreen) SetWindowPos(app.hwnd, nullptr, -6000, 50, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    LayoutChildren();
    UpdateTitle();
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
