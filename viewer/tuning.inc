// Camera tuning (SETTINGS > Developer > Camera tuning; [developer] tuning = 1). Included by lso_viewer.cpp before
// console.inc.
//
// The TUNE key on a camera's PTZ panel opens a panel that moves the camera the monitor shows (the island camera, or
// the deck position on it) in DCS while it runs: POS and LOOK (the point it looks at) along the carrier's axes, in
// metres from the model origin (x forward, y up, z starboard), and FOV (deg). The values start from where the hook
// has the camera (protocol v9 Header::camPose: what the camera was last 'set' to, by the name of the view it shows).
//
// The panel writes them to the hook's tuning file, Saved Games\DCS\Config\ILARTS-cameras.lua, which the hook reads
// again within 0.5 s and applies at once, to every slot showing that view. ILARTS owns one section of the file:
//
//   return {
//       { name = 'ISLAND', ... },                 <- anything else: kept byte for byte
//       -- ILARTS developer tuning: begin
//       { name = 'HANGAR FWD', pos = { 54.7, 17.2, 4.0 }, look = { -60.0, 10.5, 0.0 }, fov = 50 },
//       -- ILARTS developer tuning: end
//   }
//
// The section is the last thing in the returned table, so its entries win over the user's own for the same name
// (the hook applies the entries in order). Without a section one is added before the table's closing brace (with a
// comma after the entry before it if that has none); without a file a new one is written. Every write is checked
// (it must still be "return { ... }" with balanced braces outside strings and comments), goes to a temporary file
// first and replaces the old one (MoveFileEx), UTF-8 without a BOM (DCS's Lua 5.1 cannot load a file with one).
// Nudges are written at most every 0.3 s. RESET takes the camera's entry out of the section: the hook then puts its
// fields back to its own values (a user entry outside the section, else the hook's CAMERAS table).

std::wstring g_testSavedGames;  // --test-saved-games <dir>: stands in for Saved Games\DCS (SavedGamesDcs, repair.inc)
std::wstring SavedGamesDcs();   // repair.inc
void ConsoleLog(const std::wstring& line);

constexpr double kTuneSteps[] = {0.1, 0.5, 1.0};  // m
constexpr double kTuneFovStep = 1;                  // deg
constexpr double kTuneWriteGap = 0.3;               // s between writes
constexpr double kTuneResetWait = 2.5;              // s: after RESET, until the hook's values are back (or its pose moved)
const char kTuneBegin[] = "-- ILARTS developer tuning: begin";
const char kTuneEnd[] = "-- ILARTS developer tuning: end";

struct TunePose {
    double pos[3] = {}, look[3] = {}, fov = 60;
};
struct TuneState {
    bool on = false;  // [developer] tuning
    int step = 1;     // kTuneSteps index ([developer] tuning_step)
    std::vector<std::pair<std::string, TunePose>> entries;  // the section, in order
    bool loaded = false;
    FILETIME fileTime{};  // the file's last write time when it was last read or written
    double checkedAt = -1;
    bool dirty = false;
    double lastWrite = -1;
    std::wstring alert;  // a write that failed, a file ILARTS cannot add its section to
    struct Reset {
        std::string name;
        double at = 0;
        LONG seq = 0;  // camPose seq at the time
        int slot = -1;
    };
    std::vector<Reset> resets;
} g_tune;

std::wstring TunePath() { return SavedGamesDcs() + L"\\Config\\ILARTS-cameras.lua"; }

// ---- a little Lua: which bytes are code (not in a comment or string)
// mask[i] = 1 for code. Long brackets ([[ ]], [==[ ]==]) for strings and comments, quoted strings with escapes.
std::vector<char> LuaCodeMask(const std::string& s) {
    std::vector<char> m(s.size(), 1);
    auto longOpen = [&](size_t i, int* level) {  // "[==[" at i -> its length, 0 if none
        if (i >= s.size() || s[i] != '[') return size_t(0);
        size_t j = i + 1;
        int l = 0;
        while (j < s.size() && s[j] == '=') ++j, ++l;
        if (j < s.size() && s[j] == '[') {
            *level = l;
            return j + 1 - i;
        }
        return size_t(0);
    };
    auto longClose = [&](size_t from, int level) {  // index after "]==]", s.size() if unterminated
        const std::string close = "]" + std::string(level, '=') + "]";
        const size_t e = s.find(close, from);
        return e == std::string::npos ? s.size() : e + close.size();
    };
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        int level = 0;
        size_t n = 0;
        if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            size_t e;
            if ((n = longOpen(i + 2, &level)) != 0) e = longClose(i + 2 + n, level);
            else {
                e = s.find('\n', i);
                e = e == std::string::npos ? s.size() : e;
            }
            for (size_t k = i; k < e; ++k) m[k] = 0;
            i = e;
        } else if ((n = longOpen(i, &level)) != 0) {
            const size_t e = longClose(i + n, level);
            for (size_t k = i; k < e; ++k) m[k] = 0;
            i = e;
        } else if (c == '\'' || c == '"') {
            size_t k = i + 1;
            while (k < s.size() && s[k] != c && s[k] != '\n') k += s[k] == '\\' ? 2 : 1;
            const size_t e = std::min(s.size(), k + 1);
            for (size_t x = i; x < e; ++x) m[x] = 0;
            i = e;
        } else {
            ++i;
        }
    }
    return m;
}

// "return { ... }" with balanced braces outside comments and strings, nothing after the table but comments and space.
// Gives the index of the table's closing brace.
bool LuaTableFileOk(const std::string& s, size_t* closeAt = nullptr) {
    const std::vector<char> m = LuaCodeMask(s);
    size_t i = 0;
    auto skip = [&] {
        while (i < s.size() && (!m[i] || isspace(static_cast<unsigned char>(s[i])))) ++i;
    };
    skip();
    if (s.compare(i, 6, "return") != 0) return false;
    i += 6;
    skip();
    if (i >= s.size() || s[i] != '{') return false;
    int depth = 0;
    for (; i < s.size(); ++i) {
        if (!m[i]) continue;
        if (s[i] == '{') ++depth;
        else if (s[i] == '}' && --depth == 0) break;
        if (depth < 0) return false;
    }
    if (i >= s.size()) return false;
    if (closeAt) *closeAt = i;
    ++i;
    skip();
    return i >= s.size();
}

// The section's lines: [begin, end) byte offsets of the whole lines from the begin marker to the end marker. 0: no
// section, -1: half a section (a marker without the other) or markers out of order.
int TuneFindSection(const std::string& s, size_t* b, size_t* e) {
    const size_t mb = s.find(kTuneBegin), me = s.find(kTuneEnd);
    if (mb == std::string::npos && me == std::string::npos) return 0;
    if (mb == std::string::npos || me == std::string::npos || me < mb || s.find(kTuneBegin, mb + 1) != std::string::npos ||
        s.find(kTuneEnd, me + 1) != std::string::npos)
        return -1;
    size_t lb = s.rfind('\n', mb);
    lb = lb == std::string::npos ? 0 : lb + 1;
    size_t le = s.find('\n', me);
    le = le == std::string::npos ? s.size() : le + 1;
    *b = lb, *e = le;
    return 1;
}

// One entry as ILARTS writes it: name, pos = { x, y, z }, look = { x, y, z }, fov = n (missing fields stay as given).
bool TuneParseEntry(const std::string& t, std::string* name, TunePose* p) {
    const size_t n = t.find("name");
    if (n == std::string::npos) return false;
    const size_t q = t.find_first_of("'\"", n);
    if (q == std::string::npos) return false;
    const size_t q2 = t.find(t[q], q + 1);
    if (q2 == std::string::npos) return false;
    *name = t.substr(q + 1, q2 - q - 1);
    auto vec = [&](const char* key, double* v) {
        size_t k = t.find(key, q2);
        while (k != std::string::npos && k > 0 && (isalnum(static_cast<unsigned char>(t[k - 1])) || t[k - 1] == '_')) k = t.find(key, k + 1);
        if (k == std::string::npos) return false;
        const size_t lb = t.find('{', k);
        if (lb == std::string::npos) return false;
        double a = 0, b = 0, c = 0;
        if (sscanf_s(t.c_str() + lb + 1, " %lf , %lf , %lf", &a, &b, &c) != 3) return false;
        v[0] = a, v[1] = b, v[2] = c;
        return true;
    };
    vec("pos", p->pos);
    vec("look", p->look);
    const size_t f = t.find("fov", q2);
    if (f != std::string::npos) {
        const size_t eq = t.find('=', f);
        if (eq != std::string::npos) sscanf_s(t.c_str() + eq + 1, " %lf", &p->fov);
    }
    return true;
}

std::string TuneNum(double v, int decimals) {  // 54.7, -0.25, 50 (at most `decimals` places, no trailing zeros)
    char b[48];
    sprintf_s(b, "%.*f", decimals, v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}
std::string TuneCoord(double v) {  // one decimal at least: 4.0, 54.75
    std::string s = TuneNum(v, 2);
    if (s.find('.') == std::string::npos) s += ".0";
    if (s == "-0.0") s = "0.0";
    return s;
}
std::string TuneEntryLua(const std::string& name, const TunePose& p) {
    return "{ name = '" + name + "', pos = { " + TuneCoord(p.pos[0]) + ", " + TuneCoord(p.pos[1]) + ", " + TuneCoord(p.pos[2]) + " }, look = { " +
           TuneCoord(p.look[0]) + ", " + TuneCoord(p.look[1]) + ", " + TuneCoord(p.look[2]) + " }, fov = " + TuneNum(p.fov, 1) + " },";
}

bool TuneReadFile(std::string* text, FILETIME* ft) {
    text->clear();
    HANDLE h = CreateFileW(TunePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (ft) *ft = FILETIME{};
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;  // none: fine
    }
    if (ft) GetFileTime(h, nullptr, nullptr, ft);
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    bool ok = sz.QuadPart < (4 << 20);
    if (ok) {
        text->resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = text->empty() || (ReadFile(h, text->data(), static_cast<DWORD>(text->size()), &got, nullptr) && got == text->size());
    }
    CloseHandle(h);
    return ok;
}

// The section's entries from the file (at the first use, and when someone else changed the file).
void TuneLoad() {
    std::string s;
    FILETIME ft{};
    g_tune.loaded = true;
    if (!TuneReadFile(&s, &ft)) return;
    g_tune.fileTime = ft;
    g_tune.entries.clear();
    size_t b = 0, e = 0;
    if (TuneFindSection(s, &b, &e) != 1) return;
    const std::string sec = s.substr(b, e - b);
    const std::vector<char> m = LuaCodeMask(sec);
    // each top-level { ... } in the section is an entry
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < sec.size(); ++i) {
        if (!m[i]) continue;
        if (sec[i] == '{' && depth++ == 0) start = i;
        else if (sec[i] == '}' && --depth == 0) {
            std::string name;
            TunePose p;
            if (TuneParseEntry(sec.substr(start, i + 1 - start), &name, &p)) {
                bool found = false;
                for (auto& x : g_tune.entries)
                    if (x.first == name) x.second = p, found = true;
                if (!found) g_tune.entries.push_back({name, p});
            }
        }
    }
}

// The file with the section as it is now in memory; false (and alert) if the file cannot take it.
bool TuneCompose(const std::string& in, std::string* out, std::wstring* why) {
    std::string s = in;
    if (s.size() >= 3 && !memcmp(s.data(), "\xEF\xBB\xBF", 3)) s.erase(0, 3);  // DCS's Lua cannot read a BOM
    const bool crlf = s.find("\r\n") != std::string::npos || s.find('\n') == std::string::npos;
    const std::string nl = crlf ? "\r\n" : "\n";
    std::string body = std::string("    ") + kTuneBegin + nl;
    for (const auto& x : g_tune.entries) body += "    " + TuneEntryLua(x.first, x.second) + nl;
    body += std::string("    ") + kTuneEnd + nl;
    bool blank = true;
    for (char c : s) blank = blank && isspace(static_cast<unsigned char>(c));
    if (blank) {
        *out = "return {" + nl + body + "}" + nl;
        return true;
    }
    size_t b = 0, e = 0;
    const int has = TuneFindSection(s, &b, &e);
    if (has < 0) {
        *why = L"ILARTS-cameras.lua: the tuning section's markers are broken";
        return false;
    }
    if (has == 1) {
        *out = s.substr(0, b) + body + s.substr(e);
    } else {
        size_t close = 0;
        if (!LuaTableFileOk(s, &close)) {
            *why = L"ILARTS-cameras.lua is not \"return { ... }\": nothing written";
            return false;
        }
        // a separator after the last field, if it has none
        const std::vector<char> m = LuaCodeMask(s);
        size_t last = close;
        while (last > 0) {
            --last;
            if (m[last] && !isspace(static_cast<unsigned char>(s[last]))) break;
        }
        std::string head = s.substr(0, close), tail = s.substr(close);
        if (s[last] != ',' && s[last] != ';' && s[last] != '{') head.insert(last + 1, ",");
        // the section on lines of its own before the closing brace
        const size_t ls = head.find_last_not_of(" \t");
        const size_t from = ls == std::string::npos ? 0 : ls + 1;
        std::string indent;  // the closing brace's indentation goes after the section
        if (ls == std::string::npos || head[ls] == '\n') indent = head.substr(from), head.erase(from);
        else head += nl;
        *out = head + body + indent + tail;
    }
    if (!LuaTableFileOk(*out)) {
        *why = L"ILARTS-cameras.lua: the tuning section did not fit: nothing written";
        return false;
    }
    return true;
}

bool TuneWrite() {
    std::string in, out;
    FILETIME ft{};
    if (!TuneReadFile(&in, &ft)) {
        g_tune.alert = L"Cannot read ILARTS-cameras.lua";
        return false;
    }
    std::wstring why;
    if (!TuneCompose(in, &out, &why)) {
        g_tune.alert = why;
        return false;
    }
    const std::wstring path = TunePath(), tmp = path + L".ilarts-tmp";
    CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);  // Config (DCS makes it; a test folder may not have it)
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        g_tune.alert = L"Cannot write ILARTS-cameras.lua";
        return false;
    }
    DWORD put = 0;
    const BOOL ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &put, nullptr) && put == out.size() && FlushFileBuffers(h);
    CloseHandle(h);
    // the hook may have the file open for a moment (every 0.5 s): then the next tick tries again
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    std::string check;
    TuneReadFile(&check, &g_tune.fileTime);
    g_tune.alert.clear();
    if (g_test.log.size()) ConsoleLog(L"tuning: wrote " + path + L" (" + std::to_wstring(g_tune.entries.size()) + L" entries)");
    return true;
}

// [developer] tuning and tuning_step (at the start and after SETTINGS > SAVE).
void TuneSettings() {
    g_tune.on = DevBuild() && GetPrivateProfileIntW(L"developer", L"tuning", 0, app.iniPath.c_str()) != 0;  // release builds: never
    g_tune.step = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"developer", L"tuning_step", 1, app.iniPath.c_str())), 0,
                             static_cast<int>(_countof(kTuneSteps)) - 1);
}

// Every loop: a pending write once kTuneWriteGap has passed; the file read again when someone else changed it.
void TuneTick(double now) {
    static bool init = false;
    if (!init) init = true, TuneSettings();
    if (!g_tune.on && !g_tune.dirty) return;  // (switched off: a pending write still goes out)
    if (g_tune.dirty && now - g_tune.lastWrite >= kTuneWriteGap) {
        g_tune.lastWrite = now;
        if (TuneWrite()) g_tune.dirty = false;
        else if (!g_tune.alert.empty()) g_tune.dirty = false;  // not retried until the next change
    }
    if (!g_tune.dirty && now - g_tune.checkedAt >= 1) {
        g_tune.checkedAt = now;
        FILETIME ft{};
        HANDLE h = CreateFileW(TunePath().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) GetFileTime(h, nullptr, nullptr, &ft), CloseHandle(h);
        if (!g_tune.loaded || CompareFileTime(&ft, &g_tune.fileTime) != 0) TuneLoad();
    }
}

TunePose* TuneEntry(const std::string& name) {
    for (auto& x : g_tune.entries)
        if (x.first == name) return &x.second;
    return nullptr;
}

// What the panel shows for logical camera i: the name its tuning entry has, and its pose (false: not known yet).
// waiting: a RESET is under way (the hook has not put its own values back yet).
bool TuneCurrent(int i, std::string* name, TunePose* p, bool* tuned, bool* waiting) {
    *tuned = *waiting = false;
    if (i < 0 || i >= CameraCount()) return false;
    *name = g_lcams[i].name;
    if (const TunePose* e = TuneEntry(*name)) {
        *p = *e;
        *tuned = true;
        return true;
    }
    const int slot = PhysOf(i);
    if (!HeaderValid() || slot < 0 || slot >= lsoshare::kMaxCameras) return false;
    const auto& cp = app.header->camPose[slot];
    if (!cp.valid || std::string(cp.name, strnlen(cp.name, sizeof(cp.name))) != *name) return false;
    for (auto it = g_tune.resets.begin(); it != g_tune.resets.end();) {
        const bool done = video::Now() - it->at >= kTuneResetWait || (it->slot == slot && cp.seq != it->seq);
        if (done) it = g_tune.resets.erase(it);
        else {
            if (it->name == *name) *waiting = true;
            ++it;
        }
    }
    for (int k = 0; k < 3; ++k) p->pos[k] = cp.pos[k], p->look[k] = cp.look[k];
    p->fov = cp.fovDeg;
    return true;
}

// The panel's state for the camera monitor shows (logical camera i), inside its "ptz" object.
void TuneJson(Json& s, int i) {
    if (!g_tune.on) return;
    if (!g_tune.loaded) TuneLoad();
    std::string name;
    TunePose p;
    bool tuned = false, waiting = false;
    const bool known = TuneCurrent(i, &name, &p, &tuned, &waiting);
    s.open(L"tune");
    s.str(L"name", Ascii(name.c_str(), name.size()));
    s.flag(L"known", known && !waiting);
    s.flag(L"tuned", tuned);
    s.num(L"step", kTuneSteps[g_tune.step]);
    s.open(L"steps", L'[');
    for (double v : kTuneSteps) s.num(nullptr, v);
    s.close(L']');
    s.num(L"fovStep", kTuneFovStep);
    if (known) {
        s.open(L"pos", L'[');
        for (double v : p.pos) s.num(nullptr, std::round(v * 100) / 100);
        s.close(L']');
        s.open(L"look", L'[');
        for (double v : p.look) s.num(nullptr, std::round(v * 100) / 100);
        s.close(L']');
        s.num(L"fov", std::round(p.fov * 10) / 10);
    }
    if (!g_tune.alert.empty()) s.str(L"alert", g_tune.alert);
    s.close();
}

void CopyText(const std::wstring& t) {
    if (!OpenClipboard(app.hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (t.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* d = GlobalLock(g)) {
            memcpy(d, t.c_str(), bytes);
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        } else {
            GlobalFree(g);
        }
    }
    CloseClipboard();
}

// The page's tuning commands: tune {cam, field: pos | look | fov, axis, dir}, tuneStep {index}, tuneReset {cam},
// tuneCopy {cam}. cam = the source key of the camera on the monitor.
void TuneCommand(const std::wstring& cmd, const std::wstring& j, int src) {
    if (!g_tune.on) return;
    double n = 0;
    if (cmd == L"tuneStep") {
        if (JsonNum(j, L"index", &n) && n >= 0 && n < static_cast<int>(_countof(kTuneSteps))) {
            g_tune.step = static_cast<int>(n);
            WritePrivateProfileStringW(L"developer", L"tuning_step", std::to_wstring(g_tune.step).c_str(), app.iniPath.c_str());
        }
        return;
    }
    if (src < kFeeds) return;
    const int i = src - kFeeds;
    std::string name;
    TunePose p;
    bool tuned = false, waiting = false;
    const bool known = TuneCurrent(i, &name, &p, &tuned, &waiting);
    if (cmd == L"tuneCopy") {
        if (!known) return;
        const std::string line = TuneEntryLua(name, p);
        CopyText(Ascii(line.c_str(), line.size()));
        RecStatus(L"Copied: " + Ascii(line.c_str(), line.size()), 4000);
        if (g_test.log.size()) ConsoleLog(L"tuning: copied " + Ascii(line.c_str(), line.size()));
        return;
    }
    if (cmd == L"tuneReset") {
        if (!tuned) return;
        for (auto it = g_tune.entries.begin(); it != g_tune.entries.end(); ++it)
            if (it->first == name) {
                g_tune.entries.erase(it);
                break;
            }
        const int slot = PhysOf(i);
        TuneState::Reset r;
        r.name = name, r.at = video::Now(), r.slot = slot;
        r.seq = HeaderValid() && slot >= 0 && slot < lsoshare::kMaxCameras ? app.header->camPose[slot].seq : 0;
        g_tune.resets.push_back(r);
        g_tune.dirty = true;
        return;
    }
    if (cmd != L"tune" || !known || waiting) return;
    const std::wstring field = JsonStr(j, L"field");
    double axis = 0, dir = 0;
    JsonNum(j, L"axis", &axis);
    if (!JsonNum(j, L"dir", &dir) || dir == 0) return;
    const double sgn = dir < 0 ? -1 : 1;
    const int a = std::clamp(static_cast<int>(axis), 0, 2);
    auto snap = [](double v) { return std::round(v * 100) / 100; };
    if (field == L"pos") p.pos[a] = snap(p.pos[a] + sgn * kTuneSteps[g_tune.step]);
    else if (field == L"look") p.look[a] = snap(p.look[a] + sgn * kTuneSteps[g_tune.step]);
    else if (field == L"fov") p.fov = std::clamp(std::round(p.fov + sgn * kTuneFovStep), 2.0, 150.0);
    else return;
    if (TunePose* e = TuneEntry(name)) *e = p;
    else g_tune.entries.push_back({name, p});
    g_tune.dirty = true;
}
