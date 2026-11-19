// ---------------------------------------------------------------- benchmark (SETTINGS > Benchmark)
//
// Armed by the switch ([benchmark] next_mission), a run starts at the next mission start and measures what ILARTS
// costs DCS, stage by stage: DCS's own frame times, taken on its render thread by the DLL (src/bench.cpp) and read here
// from the block in src/bench_protocol.h. The hook holds ILARTS back for the first stage (the hook alone); this file
// releases it and runs the rest: the native exports (PLAT, LSO 1, LSO 2), then 1 to 5 camera views with recording off
// and on (recorded into a null sink: encoded like a file, nothing written). While a run is under way it decides which
// cameras render (BenchWantsCamera) and which sources record (BenchRecArmed); nothing of the user's is changed, so
// when it ends everything goes back by itself. The report: %LOCALAPPDATA%\ILARTS\benchmarks, one HTML page and one
// JSON file per run. Included by lso_viewer.cpp after console.inc.

namespace bench {

// ---- statistics (pure; unit-tested by --test-bench-report)

// Nearest-rank percentile of sorted values (p in 0..100): the smallest value with at least p % of the values at or
// below it.
double Percentile(const std::vector<float>& sorted, double p) {
    if (sorted.empty()) return 0;
    const double rank = std::ceil(p / 100.0 * static_cast<double>(sorted.size()));
    const size_t i = static_cast<size_t>(std::clamp(rank, 1.0, static_cast<double>(sorted.size()))) - 1;
    return sorted[i];
}

// One stage's frames. Average FPS = frames / their total time (not the mean of per-frame FPS). The lows are the frame
// rates at the 99th and 99.9th percentile frame time (99 % / 99.9 % of the frames were faster), as PresentMon-based
// tools (FrameView, CapFrameX "P1 / P0.1") report them.
struct Stats {
    int frames = 0;
    double seconds = 0, avgMs = 0, avgFps = 0, p99Ms = 0, p999Ms = 0, low1Fps = 0, low01Fps = 0, maxMs = 0;
};
Stats Compute(std::vector<float> ms) {
    Stats s;
    ms.erase(std::remove_if(ms.begin(), ms.end(), [](float v) { return !(v > 0) || !std::isfinite(v); }), ms.end());
    if (ms.empty()) return s;
    double sum = 0;
    for (float v : ms) sum += v;
    std::sort(ms.begin(), ms.end());
    s.frames = static_cast<int>(ms.size());
    s.seconds = sum / 1000.0;
    s.avgMs = sum / s.frames;
    s.avgFps = 1000.0 / s.avgMs;
    s.p99Ms = Percentile(ms, 99.0);
    s.p999Ms = Percentile(ms, 99.9);
    s.low1Fps = 1000.0 / s.p99Ms;
    s.low01Fps = 1000.0 / s.p999Ms;
    s.maxMs = ms.back();
    return s;
}

struct Stage {
    std::wstring name;  // "Hook only", "PLAT, LSO 1, LSO 2", "2 views", "2 views + recording"
    int cams = -1;      // -1 = the hook alone (nothing shared), 0 = the native exports, n = n camera views
    bool rec = false;   // every source shown recorded (null sink)
    // result
    bool measured = false, skipped = false, paused = false;  // paused: the simulation was paused during part of it
    std::wstring note;     // why skipped / what fell short
    std::wstring sources;  // cameras rendered ("ISLAND, BOW BUBBLE"), recorded ("PLAT, ISLAND")
    Stats st;
    double cpu = -1;       // ILARTS (this process): % of the machine's CPU time
    long long encoded = 0; // frames the recording encoded (all sources)
};

struct Report {
    std::vector<std::pair<std::wstring, std::wstring>> info;  // header lines (label, value)
    std::vector<Stage> stages;
    std::wstring when;    // "2026-11-19 21:04:10"
    std::wstring result;  // "" complete, else why it stopped
    double measureS = 20;
};

std::vector<Stage> Plan() {
    std::vector<Stage> s;
    s.push_back({L"Hook only", -1, false});
    s.push_back({L"PLAT, LSO 1, LSO 2", 0, false});
    for (int n = 1; n <= 5; ++n) {
        const std::wstring v = std::to_wstring(n) + (n == 1 ? L" view" : L" views");
        s.push_back({v, n, false});
        s.push_back({v + L" + recording", n, true});
    }
    return s;
}

// ---- the report

std::wstring Html(const std::wstring& t) {
    std::wstring o;
    for (wchar_t c : t) {
        if (c == L'&') o += L"&amp;";
        else if (c == L'<') o += L"&lt;";
        else if (c == L'>') o += L"&gt;";
        else if (c == L'"') o += L"&quot;";
        else o += c;
    }
    return o;
}
std::wstring Num(double v, int decimals) {
    wchar_t b[48];
    swprintf_s(b, L"%.*f", decimals, std::isfinite(v) ? v : 0.0);
    return b;
}
std::wstring Signed(double v, int decimals) {  // "+2.3", "−15" (a real minus sign), "±0"
    const double r = std::round(v * std::pow(10.0, decimals)) / std::pow(10.0, decimals);
    if (r == 0) return L"\x00B1" + Num(0, decimals);
    return (r > 0 ? L"+" : L"\x2212") + Num(std::fabs(r), decimals);
}
int FpsDecimals(double fps) { return fps < 100 ? 1 : 0; }

// "−15 fps · +2.3 ms · −13%": a value against the baseline's, as fps (higher is better).
std::wstring Delta(double fps, double base) {
    if (!(fps > 0) || !(base > 0)) return L"";
    const double ms = 1000.0 / fps - 1000.0 / base, pct = (fps - base) / base * 100;
    return Signed(fps - base, std::fabs(fps - base) < 9.95 ? 1 : 0) + L" fps \x00B7 " + Signed(ms, 1) + L" ms \x00B7 " + Signed(pct, 0) + L"%";
}

std::wstring ReportHtml(const Report& r) {
    const Stage* base = nullptr;
    for (const auto& s : r.stages)
        if (s.measured && s.cams < 0) base = &s;
    double maxFps = 1;
    for (const auto& s : r.stages)
        if (s.measured) maxFps = std::max(maxFps, s.st.avgFps);
    std::wstring h;
    h += L"<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n";
    h += L"<title>ILARTS benchmark " + Html(r.when) + L"</title>\n<style>\n"
         L":root{--bg:#0F1214;--panel:#15181B;--line:#2E343A;--text:#C9D0D6;--dim:#7D8690;--hi:#E6EDF3;--good:#8EE089;--bad:#E0524E;--warn:#E7B04F;--bar:#5E9A5A;--bar2:#3A4148}\n"
         L"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:13px/1.5 'IBM Plex Mono',Consolas,'Cascadia Mono',monospace}\n"
         L"main{max-width:1180px;margin:0 auto;padding:24px 16px 40px}\n"
         L"h1{font-size:15px;letter-spacing:.14em;color:var(--hi);margin:0 0 4px;text-transform:uppercase}\n"
         L".sub{color:var(--dim);margin:0 0 18px}.stop{color:var(--warn);margin:0 0 18px}\n"
         L"dl{display:grid;grid-template-columns:max-content 1fr;gap:2px 18px;margin:0 0 18px;padding:12px 14px;background:var(--panel);border:1px solid var(--line);border-radius:4px}\n"
         L"dt{color:var(--dim)}dd{margin:0;color:var(--hi);overflow-wrap:anywhere}\n"
         L"p.read{color:var(--dim);max-width:900px;margin:0 0 18px}\n"
         L".wrap{overflow-x:auto;border:1px solid var(--line);border-radius:4px;background:var(--panel)}\n"
         L"table{border-collapse:collapse;width:100%;min-width:860px}\n"
         L"th,td{padding:7px 10px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top;white-space:nowrap}\n"
         L"th{color:var(--dim);font-weight:600;font-size:11px;letter-spacing:.12em;text-transform:uppercase}\n"
         L"tr:last-child td{border-bottom:0}td.n{text-align:right}\n"
         L".v{color:var(--hi);font-size:14px}.u{color:var(--dim)}.d{display:block;font-size:11px}.d.c{color:var(--bad)}.d.g{color:var(--good)}.d.z{color:var(--dim)}\n"
         L".tag{display:inline-block;margin-left:6px;padding:0 5px;border:1px solid var(--warn);border-radius:2px;color:var(--warn);font-size:10px;letter-spacing:.1em}\n"
         L".src{display:block;color:var(--dim);font-size:11px;white-space:normal;max-width:240px}\n"
         L".bars{width:220px}.bar{height:7px;margin:3px 0;background:var(--bar2);border-radius:1px}.bar i{display:block;height:100%;background:var(--bar);border-radius:1px}\n"
         L".bar.l1 i{background:#7D8690}.bar.l01 i{background:#4A525A}\n"
         L".key{color:var(--dim);font-size:11px;margin:8px 2px 0}.key b{display:inline-block;width:18px;height:7px;margin:0 6px 0 14px;vertical-align:middle}\n"
         L"</style></head><body><main>\n";
    h += L"<h1>ILARTS benchmark</h1>\n<p class=\"sub\">" + Html(r.when) + L"</p>\n";
    if (!r.result.empty()) h += L"<p class=\"stop\">Stopped: " + Html(r.result) + L"</p>\n";
    h += L"<dl>\n";
    for (const auto& kv : r.info) h += L"<dt>" + Html(kv.first) + L"</dt><dd>" + Html(kv.second.empty() ? L"\x2014" : kv.second) + L"</dd>\n";
    h += L"</dl>\n";
    h += L"<p class=\"read\">How to read this: each row ran DCS for " + Num(r.measureS, 0) +
         L" seconds after a settling period, with the frame times DCS's render thread took. Avg is the frame rate over "
         L"the row (frames &divide; time). 1% low and 0.1% low are the frame rates at the 99th and 99.9th percentile frame "
         L"time: 99% (99.9%) of the frames were faster. The line under each number compares it with the first row, DCS with "
         L"only the ILARTS hook loaded: fewer fps, more milliseconds per frame and the change in percent are what that row "
         L"costs. The scene in a running mission keeps changing, so a difference of a few percent can be noise.</p>\n";
    h += L"<div class=\"wrap\"><table>\n<thead><tr><th>Stage</th><th>Avg</th><th>1% low</th><th>0.1% low</th><th>ILARTS CPU</th><th></th></tr></thead><tbody>\n";
    auto cell = [&](double fps, double baseFps, bool isBase) {
        std::wstring c = L"<td><span class=\"v\">" + Num(fps, FpsDecimals(fps)) + L"</span> <span class=\"u\">fps \x00B7 " +
                         Num(1000.0 / fps, 1) + L" ms</span>";
        if (!isBase && baseFps > 0) {
            const double pct = (fps - baseFps) / baseFps * 100;
            const wchar_t* cls = std::fabs(pct) < 0.5 ? L"z" : pct < 0 ? L"c" : L"g";
            c += std::wstring(L"<span class=\"d ") + cls + L"\">" + Delta(fps, baseFps) + L"</span>";
        }
        return c + L"</td>";
    };
    for (const auto& s : r.stages) {
        h += L"<tr><td>" + Html(s.name);
        if (s.paused) h += L"<span class=\"tag\">PAUSED</span>";
        if (!s.sources.empty()) h += L"<span class=\"src\">" + Html(s.sources) + L"</span>";
        if (!s.note.empty()) h += L"<span class=\"src\" style=\"color:var(--warn)\">" + Html(s.note) + L"</span>";
        h += L"</td>";
        if (!s.measured) {
            h += L"<td colspan=\"5\" class=\"u\">" + std::wstring(s.skipped ? L"skipped" : L"not run") + L"</td></tr>\n";
            continue;
        }
        const bool isBase = &s == base;
        h += cell(s.st.avgFps, base ? base->st.avgFps : 0, isBase);
        h += cell(s.st.low1Fps, base ? base->st.low1Fps : 0, isBase);
        h += cell(s.st.low01Fps, base ? base->st.low01Fps : 0, isBase);
        h += L"<td class=\"n\">" + (s.cpu >= 0 ? Num(s.cpu, 1) + L"%" : std::wstring(L"\x2014")) + L"</td>";
        auto bar = [&](const wchar_t* cls, double v) {
            return std::wstring(L"<div class=\"bar ") + cls + L"\"><i style=\"width:" + Num(std::clamp(v / maxFps * 100, 0.0, 100.0), 1) + L"%\"></i></div>";
        };
        h += L"<td class=\"bars\">" + bar(L"avg", s.st.avgFps) + bar(L"l1", s.st.low1Fps) + bar(L"l01", s.st.low01Fps) + L"</td></tr>\n";
    }
    h += L"</tbody></table></div>\n";
    h += L"<p class=\"key\"><b style=\"background:#5E9A5A\"></b>Avg<b style=\"background:#7D8690\"></b>1% low<b style=\"background:#4A525A\"></b>0.1% low</p>\n";
    h += L"</main></body></html>\n";
    return h;
}

std::wstring JsonStr(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') o += L'\\', o += c;
        else if (c < 0x20) {
            wchar_t b[8];
            swprintf_s(b, L"\\u%04x", static_cast<unsigned>(c));
            o += b;
        } else o += c;
    }
    return o + L"\"";
}
std::wstring ReportJson(const Report& r) {
    std::wstring j = L"{\n  \"when\": " + JsonStr(r.when) + L",\n  \"stopped\": " + JsonStr(r.result) + L",\n  \"measureSeconds\": " +
                     Num(r.measureS, 1) + L",\n  \"definitions\": " +
                     JsonStr(L"avgFps = frames / total frame time; low1Fps / low01Fps = 1000 / the 99th / 99.9th percentile frame time "
                             L"(nearest rank); cpu = ILARTS's share of the machine's CPU time") +
                     L",\n  \"info\": {";
    for (size_t i = 0; i < r.info.size(); ++i) j += (i ? L", " : L"") + JsonStr(r.info[i].first) + L": " + JsonStr(r.info[i].second);
    j += L"},\n  \"stages\": [\n";
    for (size_t i = 0; i < r.stages.size(); ++i) {
        const Stage& s = r.stages[i];
        j += L"    {\"name\": " + JsonStr(s.name) + L", \"cameras\": " + std::to_wstring(std::max(0, s.cams)) + L", \"shared\": " +
             (s.cams >= 0 ? L"true" : L"false") + L", \"recording\": " + (s.rec ? L"true" : L"false") + L", \"measured\": " +
             (s.measured ? L"true" : L"false") + L", \"paused\": " + (s.paused ? L"true" : L"false") + L", \"sources\": " +
             JsonStr(s.sources) + L", \"note\": " + JsonStr(s.note);
        if (s.measured)
            j += L", \"frames\": " + std::to_wstring(s.st.frames) + L", \"seconds\": " + Num(s.st.seconds, 3) + L", \"avgFps\": " +
                 Num(s.st.avgFps, 2) + L", \"avgMs\": " + Num(s.st.avgMs, 3) + L", \"p99Ms\": " + Num(s.st.p99Ms, 3) +
                 L", \"p999Ms\": " + Num(s.st.p999Ms, 3) + L", \"low1Fps\": " + Num(s.st.low1Fps, 2) + L", \"low01Fps\": " +
                 Num(s.st.low01Fps, 2) + L", \"maxMs\": " + Num(s.st.maxMs, 3) + L", \"ilartsCpuPercent\": " + Num(s.cpu, 2) +
                 L", \"encodedFrames\": " + std::to_wstring(s.encoded);
        j += i + 1 < r.stages.size() ? L"},\n" : L"}\n";
    }
    return j + L"  ]\n}\n";
}

bool WriteUtf8(const std::wstring& path, const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string u(static_cast<size_t>(std::max(0, n)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), u.data(), n, nullptr, nullptr);
    const std::wstring part = path + L".part";
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(f, u.data(), static_cast<DWORD>(u.size()), &w, nullptr) && w == u.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(part.c_str());
        return false;
    }
    return true;
}

// Writes "ILARTS benchmark <date> <time>.html" and ".json" into dir (made if missing). The HTML's path, "" if it failed.
std::wstring WriteReport(const Report& r, const std::wstring& dir, const std::wstring& stamp) {
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const std::wstring base = dir + L"\\ILARTS benchmark " + stamp;
    if (!WriteUtf8(base + L".html", ReportHtml(r))) return {};
    WriteUtf8(base + L".json", ReportJson(r));
    return base + L".html";
}

}  // namespace bench

// ---- the run

std::wstring g_testBenchReport;  // --test-bench-report <folder>

std::wstring BenchFolder() { return LogsFolder() + L"\\benchmarks"; }

struct BenchRun {
    HANDLE map = nullptr;
    lsoshare::Bench* b = nullptr;
    ULONGLONG lastTry = 0;
    LONG doneSession = 0;  // the last session this viewer ran or closed (never a second run in it)
    bool active = false;
    LONG session = 0, whoSeq = 0;
    bench::Report rep;
    std::wstring stamp;   // file names
    size_t cur = 0;
    bool measuring = false;
    double phaseT = 0;    // when the stage (settling) or its measurement started (video::Now)
    double readyT = 0;    // when its pictures were there (settling counts from then); 0 = not yet
    LONG64 read = 0;      // frame times consumed
    LONG64 lastFrames = -1;
    double framesT = 0;   // when the frame count last moved
    std::vector<float> samples;
    double cpuT0 = 0;
    // what the run decides while it is under way
    uint32_t camSet = 0;  // logical cameras rendered
    bool recOn = false;
    std::vector<Channel*> recCh;  // channels recording into the null sink
    size_t recStage = 0;          // the stage their encoded frames belong to
    int recPending = 0;           // recordings ended whose encoder has not reported its frames yet
    bool reportDue = false;       // the run is over; the report waits for recPending (or reportBy)
    double reportBy = 0;
    bool compositeWas = false;
    bool headerSeen = false;
    bool hw = false;              // the recording encoder was the GPU's
    bool noCams = false;          // a stage found no cameras: the later ones do not wait for them again
    // from the share while it is there (the report is written after the mission may have ended)
    std::wstring dcs, mission, carrier;
    LUID luid{};
    // timing (--test-bench-seconds shortens everything for tests)
    double measureS = 20, scale = 1;
    std::wstring lastReport;
    // before stage 1: the mission has to run (unpaused, the player in place) for kBenchStartS first
    bool waiting = false;
    double warmT = 0;     // since when it has run (0 = it does not now)
    double noAcT = 0;     // since when it has run without the player in an aircraft (0 = in one, or not looked yet)
    // DCS paused (or the hook stopped stepping): the clocks stop and no frame counts (BenchHold)
    bool halt = false;
    double resumeT = -1e9;  // when the last pause ended
    double lastT = 0;       // the last BenchTick (video::Now)
    struct Mark {
        double t, elapsed;  // when, and the measurement's clock then
        size_t n;           // samples then
    };
    std::vector<Mark> marks;  // the last few seconds of the measurement (taken back when a pause is seen)
    double cpuWallT = 0;      // ILARTS CPU: wall time from here
    // the title-bar lamp (BenchLamp): the last run's result until it is clicked or ILARTS closes
    int result = 0;  // 0 none, 1 complete, 2 stopped
    std::wstring resultWhy, resultReport;
    int armedWas = -1;
} g_bench;

constexpr double kBenchStartS = 10;      // before stage 1: the mission running, unpaused, with the player in place
constexpr double kBenchResumeS = 3;      // after a pause: before the clocks run again
constexpr double kBenchRollbackS = 4;    // when a pause is seen, the measurement's last seconds are taken back
constexpr LONG64 kBenchHookStaleMs = 2500;  // the hook steps once a second: longer without one = DCS is not running

bool BenchActive() { return g_bench.active; }
bool BenchReportDue() { return g_bench.reportDue; }
bool BenchWantsCamera(int i) { return i >= 0 && i < 32 && ((g_bench.camSet >> i) & 1); }
bool BenchRecArmed(int s) {
    if (!g_bench.recOn) return false;
    return s == 0 || (s >= kFeeds && BenchWantsCamera(s - kFeeds));  // the PLAT and every camera view shown
}

void BenchLog(const std::wstring& line) { TestLog(L"bench: " + line); }

void BenchStageText(const std::wstring& t) {
    if (!g_bench.b) return;
    std::string a;
    for (wchar_t c : t) a += c < 128 ? static_cast<char>(c) : '?';
    strncpy_s(g_bench.b->stage, a.c_str(), _TRUNCATE);  // the hook logs it to ILARTS.log
}

// The stage as the hook logs it: "4/12 1 view + recording", "starting", with " (paused)".
std::wstring BenchStageName() {
    if (!g_bench.active || g_bench.cur >= g_bench.rep.stages.size()) return {};
    std::wstring t = g_bench.waiting ? std::wstring(L"starting")
                                     : std::to_wstring(g_bench.cur + 1) + L"/" + std::to_wstring(g_bench.rep.stages.size()) + L" " +
                                           g_bench.rep.stages[g_bench.cur].name;
    return t + (g_bench.halt ? L" (paused)" : L"");
}

void BenchCommand(LONG cmd) {
    if (!g_bench.b) return;
    g_bench.b->command = cmd;
    MemoryBarrier();
    InterlockedExchange(&g_bench.b->commandSession, g_bench.session);
}

bool BenchOpen() {
    if (g_bench.b) return true;
    const ULONGLONG now = GetTickCount64();
    if (now - g_bench.lastTry < 1000) return false;
    g_bench.lastTry = now;
    wchar_t name[96];
    lsoshare::BenchMappingName(name, _countof(name));
    HANDLE m = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (!m) return false;
    auto* b = static_cast<lsoshare::Bench*>(MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(lsoshare::Bench)));
    if (!b || b->magic != lsoshare::kBenchMagic || b->version != lsoshare::kBenchVersion) {
        if (b) UnmapViewOfFile(b);
        CloseHandle(m);
        return false;
    }
    g_bench.map = m;
    g_bench.b = b;
    return true;
}

double ProcessCpuSeconds() {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER a{}, b{};
    a.LowPart = k.dwLowDateTime, a.HighPart = k.dwHighDateTime;
    b.LowPart = u.dwLowDateTime, b.HighPart = u.dwHighDateTime;
    return static_cast<double>(a.QuadPart + b.QuadPart) / 1e7;
}

// ---- report header

std::wstring BenchInfo(const char* key) {  // a line of the hook's info ("mission", "aircraft" ...), UTF-8
    if (!g_bench.b) return {};
    char buf[sizeof(g_bench.b->info) + 1] = {};
    memcpy(buf, g_bench.b->info, sizeof(g_bench.b->info));
    const std::string all(buf), k = std::string(key) + "=";
    for (size_t p = 0; p < all.size();) {
        size_t e = all.find('\n', p);
        if (e == std::string::npos) e = all.size();
        if (all.compare(p, k.size(), k) == 0) return Utf8W(all.substr(p + k.size(), e - p - k.size()));
        p = e + 1;
    }
    return {};
}

std::wstring AdapterName(LUID luid) {
    IDXGIFactory1* f = nullptr;
    std::wstring out;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return out;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; out.empty() && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
            out = d.Description;
            wchar_t mem[48];
            swprintf_s(mem, L" (%.0f GB)", d.DedicatedVideoMemory / 1073741824.0);
            if (d.DedicatedVideoMemory >= 512ull << 20) out += mem;
        }
        a->Release();
    }
    f->Release();
    return out;
}

std::wstring CpuName() {
    wchar_t v[128] = {};
    DWORD n = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ,
                     nullptr, v, &n) != ERROR_SUCCESS)
        return {};
    std::wstring s = v;
    while (!s.empty() && s.back() == L' ') s.pop_back();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return s + L", " + std::to_wstring(si.dwNumberOfProcessors) + L" threads";
}

// DCS's window: the largest visible top-level window of its process (its client area: the picture DCS renders).
std::wstring DcsResolution(DWORD pid) {
    struct Find {
        DWORD pid;
        LONG w = 0, h = 0;
    } f{pid};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* f = reinterpret_cast<Find*>(l);
            DWORD p = 0;
            GetWindowThreadProcessId(h, &p);
            RECT rc;
            if (p == f->pid && IsWindowVisible(h) && GetClientRect(h, &rc) && (rc.right * rc.bottom) > f->w * f->h) f->w = rc.right, f->h = rc.bottom;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.w > 0 ? std::to_wstring(f.w) + L" \x00D7 " + std::to_wstring(f.h) : std::wstring();
}

// What the report takes from the share, while it is there.
void BenchNoteHeader() {
    if (!HeaderValid()) return;
    g_bench.dcs = Ascii(app.header->compat.dcsVersion, sizeof(app.header->compat.dcsVersion));
    g_bench.mission = Utf8W(std::string(app.header->missionName, strnlen(app.header->missionName, sizeof(app.header->missionName))));
    g_bench.luid = app.header->adapterLuid;
    if (app.header->selected >= 0 && app.header->selected < static_cast<int32_t>(std::min<uint32_t>(app.header->carrierCount, 8))) {
        const auto& c = app.header->carriers[app.header->selected];
        g_bench.carrier = Utf8W(std::string(c.name, strnlen(c.name, sizeof(c.name)))) + L" (" + Ascii(c.type, sizeof(c.type)) + L")";
    }
}

void BenchHeader() {
    auto& info = g_bench.rep.info;
    info.clear();
    std::wstring dcs = BenchInfo("dcs");
    if (dcs.empty()) dcs = g_bench.dcs;
    std::wstring mission = BenchInfo("mission");
    if (mission.empty()) mission = g_bench.mission;
    const std::wstring carrier = g_bench.carrier;
    const LUID luid = g_bench.luid;
    std::wstring rec = std::to_wstring(g_engCfg.w) + L" \x00D7 " + std::to_wstring(g_engCfg.h) + L", " + std::to_wstring(g_engCfg.fps) +
                       L" fps, " + bench::Num(g_engCfg.bitrate / 1e6, 1) + L" Mbit/s";
    if (!g_bench.recCh.empty() || g_bench.hw) rec += g_bench.hw ? L", graphics card encoder" : L", software encoder";
    int streams = 0, spout = 0, vcams = 0;
    for (const auto& t : g_streams) streams += t.on;
    for (const auto& o : g_spout) spout += o.want;
    for (const auto& o : g_vcam) vcams += o.want;
    const std::wstring ver = wcscmp(ILARTS_VERSION_W, L"0.0.0") ? std::wstring(ILARTS_VERSION_W) : std::wstring(L"development build");
    info.push_back({L"ILARTS", ver});
    info.push_back({L"DCS", dcs});
    info.push_back({L"Graphics card", AdapterName(luid)});
    info.push_back({L"Processor", CpuName()});
    info.push_back({L"DCS window", DcsResolution(g_bench.b ? g_bench.b->producerPid : 0)});
    info.push_back({L"Mission", mission});
    info.push_back({L"Carrier", carrier});
    std::wstring who = BenchInfo("aircraft");
    const std::wstring role = BenchInfo("role");
    info.push_back({L"Player", (who.empty() ? std::wstring(L"no aircraft") : who) + (role.empty() ? L"" : L", " + role)});
    info.push_back({L"Recording", rec});
    info.push_back({L"Also running", std::wstring(L"black box ") + (g_bb.on ? L"on" : L"off") + L", streams " + std::to_wstring(streams) +
                                         L", Spout senders " + std::to_wstring(spout) + L", virtual cameras " + std::to_wstring(vcams) +
                                         L", CCTV look " + (g_cctv.on ? L"on" : L"off")});
}

// ---- stages

// The cameras a stage with n views renders: the fixed cameras first (ISLAND), then the flight deck positions, then
// the hangar ones - each in the list's order. 0 when there are fewer than n.
uint32_t BenchCamSet(int n, std::wstring* names) {
    std::vector<int> order;
    for (int i = 0; i < CameraCount(); ++i)
        if (!IsPosition(i)) order.push_back(i);
    for (int i = 0; i < CameraCount(); ++i)
        if (IsPosition(i) && !IsHangar(i)) order.push_back(i);
    for (int i = 0; i < CameraCount(); ++i)
        if (IsHangar(i)) order.push_back(i);
    // a deck position needs a free render slot: at most as many positions as the deck camera has slots
    int fixed = 0, positions = 0;
    uint32_t set = 0;
    names->clear();
    for (int i : order) {
        if (fixed + positions >= n || i >= 32) break;
        if (IsPosition(i)) {
            if (positions >= static_cast<int>(g_pool.size())) continue;
            ++positions;
        } else {
            ++fixed;
        }
        set |= 1u << i;
        *names += (names->empty() ? L"" : L", ") + CameraTag(i);
    }
    return fixed + positions == n ? set : 0;
}

void BenchRecEnd() {
    for (Channel* c : g_bench.recCh) {
        g_bench.hw = g_bench.hw || c->eng.hardwareEncoder();
        c->eng.endRecording(kBenchRecKey, L"-");
        ++g_bench.recPending;  // its frame count arrives later, from the encoder (BenchEncoded)
    }
    if (!g_bench.recCh.empty()) BenchLog(L"recording stopped (" + std::to_wstring(g_bench.recCh.size()) + L" sources, nothing saved)");
    g_bench.recCh.clear();
}

// The engine's report of a benchmark recording ("<frames> frames, <bytes> bytes"): counted to its stage.
void BenchEncoded(const video::Event& e) {
    const long long frames = _wtoi64(e.text.c_str());
    if (g_bench.recStage < g_bench.rep.stages.size()) g_bench.rep.stages[g_bench.recStage].encoded += frames;
    if (g_bench.recPending > 0) --g_bench.recPending;
    BenchLog(L"null sink: " + e.text);
}

// A stage with views: its cameras, once DCS has listed them (CamLive); with recording, the PLAT and each view record.
bool BenchPick(bench::Stage& s) {
    if (s.cams <= 0 || !CamLive()) return false;
    std::wstring names;
    g_bench.camSet = BenchCamSet(s.cams, &names);
    if (!g_bench.camSet) return false;
    s.sources = names;
    if (s.rec) {
        g_bench.recOn = true;
        s.sources = L"PLAT, " + names;
    }
    return true;
}

void BenchEnter(size_t i) {
    g_bench.waiting = false;
    g_bench.cur = i;
    g_bench.measuring = false;
    g_bench.phaseT = video::Now();
    g_bench.readyT = 0;
    g_bench.samples.clear();
    if (i >= g_bench.rep.stages.size()) return;
    bench::Stage& s = g_bench.rep.stages[i];
    BenchRecEnd();
    g_bench.recOn = false;
    if (s.cams == 0) BenchCommand(lsoshare::kBenchCmdRelease);  // the hook starts ILARTS
    g_bench.camSet = 0;
    BenchPick(s);  // the views (else once DCS has listed its cameras: BenchSettle)
    CameraReconcile();
    wchar_t b[64];
    swprintf_s(b, L"%zu/%zu ", i + 1, g_bench.rep.stages.size());
    BenchStageText(b + s.name);
    BenchLog(L"stage " + std::wstring(b) + s.name);
}

void BenchFinish(const std::wstring& why, bool waitForEncoders = true);

// Before stage 1 (and again when the player changes during it): wait until the mission has run kBenchStartS, unpaused,
// with the player in place (BenchTick).
void BenchWait() {
    BenchRecEnd();
    g_bench.recOn = false;
    g_bench.camSet = 0;
    g_bench.waiting = true;
    g_bench.cur = 0;
    g_bench.measuring = false;
    g_bench.samples.clear();
    g_bench.warmT = g_bench.noAcT = 0;
    BenchStageText(L"starting");
    BenchLog(L"waiting until the mission has run " + bench::Num(kBenchStartS * g_bench.scale, 1) + L" s");
}

// The player is in place: in an aircraft, or (spectator, or still choosing a slot) after 20 s of running mission
// without one (60 s in multiplayer). The time counts only while DCS runs (BenchTick moves noAcT with the pauses).
bool BenchInPlace(double now) {
    if (BenchInfo("player").find(L"in an aircraft") != std::wstring::npos) {
        g_bench.noAcT = 0;
        return true;
    }
    if (!g_bench.noAcT) g_bench.noAcT = now;
    return now - g_bench.noAcT >= (BenchInfo("role") == L"single player" ? 20 : 60) * g_bench.scale;
}

// DCS is paused, or the hook has stopped stepping (it steps once a second while the mission runs).
bool BenchHalted() {
    const auto* b = g_bench.b;
    return b->paused || static_cast<LONG64>(GetTickCount64()) - b->hookTick > kBenchHookStaleMs;
}

// A pause was seen while measuring: the frames of the last kBenchRollbackS (the hook reports a second late) are taken
// back, and the clock with them. Only when the pause may have begun before that is the stage marked PAUSED.
void BenchRollback(double now) {
    const auto* b = g_bench.b;
    const LONG64 sinceHook = static_cast<LONG64>(GetTickCount64()) - b->hookTick;
    const double began = (static_cast<double>(sinceHook) + (b->paused ? 1000.0 : 0.0)) / 1000.0;  // at most this long ago
    const BenchRun::Mark* keep = nullptr;
    for (const auto& m : g_bench.marks)
        if (m.t <= now - kBenchRollbackS) keep = &m;
    if (keep) {
        g_bench.samples.resize(std::min(g_bench.samples.size(), keep->n));
        g_bench.phaseT = now - keep->elapsed;
    } else {  // the measurement began within the last seconds: it starts again
        g_bench.samples.clear();
        g_bench.phaseT = now;
    }
    g_bench.marks.clear();
    if (began > kBenchRollbackS) g_bench.rep.stages[g_bench.cur].paused = true;
    BenchLog(L"measurement taken back to " + bench::Num(now - g_bench.phaseT, 1) + L" s" + (began > kBenchRollbackS ? L" (paused frames may be in it)" : L""));
}

// Settling: true once the stage's pictures are there and have run long enough.
bool BenchSettle(double now) {
    bench::Stage& s = g_bench.rep.stages[g_bench.cur];
    const double sc = g_bench.scale, waited = now - g_bench.phaseT;
    if (s.cams < 0) {  // the hook alone: after the start wait (BenchTick), 10 s more for the caches to settle
        g_bench.readyT = g_bench.phaseT;
        return waited >= 10 * sc;
    }
    if (s.cams == 0) {  // ILARTS started: the PLAT's picture
        if (!g_bench.readyT && HeaderValid() && ViewFor(0).main) g_bench.readyT = now;
        if (!g_bench.readyT && waited > 60 * sc) {
            g_bench.readyT = now;
            s.note = L"no PLAT picture";
        }
        return g_bench.readyT && now - g_bench.readyT >= 8 * sc;
    }
    if (!g_bench.camSet) {  // n views: the cameras once DCS has listed them (at once if it has, but too few)
        if (BenchPick(s)) {
            CameraReconcile();
            g_bench.phaseT = now;
        } else if (CamLive() || g_bench.noCams || waited > 20 * sc) {
            s.skipped = true;
            s.note = CamLive() ? L"fewer cameras than views" : L"no cameras (DCS did not list them)";
            g_bench.noCams = g_bench.noCams || !CamLive();
            return true;
        }
        return false;
    }
    if (!g_bench.readyT) {
        int live = 0, n = 0;
        for (int i = 0; i < CameraCount() && i < 32; ++i) {
            if (!BenchWantsCamera(i)) continue;
            ++n;
            live += CameraRendering(i) && ViewFor(kFeeds + i).main != nullptr;
        }
        bool ok = live == n;
        if (ok && s.rec) {  // every recorded source's channel has pictures: start the recording
            const double t = video::Now();
            int ch = 0;
            for (const auto& c : g_rec.extra) ch += c->ok && ChannelActive(*c) && t - c->lastFrame <= 1.0;
            ok = ch >= n + 1;  // the PLAT and each view
        }
        if (!ok && waited > 30 * sc) {
            ok = true;
            if (live < n) s.note = std::to_wstring(live) + L" of " + std::to_wstring(n) + L" views had a picture";
        }
        if (ok) {
            g_bench.readyT = now;
            if (s.rec) {
                const double t = video::Now();
                g_bench.recStage = g_bench.cur;
                for (auto& c : g_rec.extra)
                    if (c->ok && ChannelActive(*c) && t - c->lastFrame <= 2.0) {
                        c->eng.beginRecording(kBenchRecKey, video::kNullSink, t);
                        g_bench.recCh.push_back(c.get());
                    }
                BenchLog(L"recording " + std::to_wstring(g_bench.recCh.size()) + L" sources into the null sink");
            }
        }
    }
    return g_bench.readyT && now - g_bench.readyT >= (s.rec ? 5 : 6) * sc;
}

void BenchStart() {
    auto* b = g_bench.b;
    g_bench.active = true;
    g_bench.session = b->session;
    g_bench.whoSeq = b->whoSeq;
    g_bench.rep = bench::Report{};
    g_bench.rep.stages = bench::Plan();
    g_bench.rep.measureS = g_bench.measureS;
    g_bench.hw = false;
    g_bench.noCams = false;
    g_bench.headerSeen = false;
    g_bench.dcs.clear(), g_bench.mission.clear(), g_bench.carrier.clear();
    g_bench.luid = LUID{};
    g_bench.read = b->frames;
    g_bench.lastFrames = -1;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t w[64];
    swprintf_s(w, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_bench.rep.when = w;
    swprintf_s(w, L"%04u-%02u-%02u %02u-%02u-%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_bench.stamp = w;
    // once: the switch is off again from now on, whatever happens
    WritePrivateProfileStringW(L"benchmark", L"next_mission", L"0", app.iniPath.c_str());
    ++g_con.formRev;
    g_bench.compositeWas = g_rec.composite;
    g_rec.composite = false;  // a file per source (the composite would be one more picture)
    g_bench.halt = false;
    g_bench.resumeT = -1e9;
    g_bench.lastT = 0;
    g_bench.marks.clear();
    g_bench.result = 0;
    BenchLog(L"run started (session " + std::to_wstring(g_bench.session) + L")");
    BenchWait();
}

constexpr double kBenchReportWaitS = 3.0;
void BenchReport();

void BenchFinish(const std::wstring& why, bool waitForEncoders) {
    BenchRecEnd();
    g_bench.recOn = false;
    g_bench.camSet = 0;
    g_bench.active = false;
    g_bench.waiting = false;
    g_rec.composite = g_bench.compositeWas;
    if (g_bench.b && g_bench.b->session == g_bench.session) BenchCommand(lsoshare::kBenchCmdDone);
    g_bench.doneSession = g_bench.session;
    BenchStageText(L"");
    CameraReconcile();  // the user's cameras again
    g_bench.rep.result = why;
    // The last recording stage's frame count comes from its encoders after they stop: the report waits for it
    // (at most kBenchReportWaitS; BenchTick writes it). Closing ILARTS writes it at once.
    if (waitForEncoders && g_bench.recPending > 0) {
        g_bench.reportDue = true;
        g_bench.reportBy = video::Now() + kBenchReportWaitS;
        return;
    }
    BenchReport();
}

void BenchReport() {
    const std::wstring why = g_bench.rep.result;
    g_bench.reportDue = false;
    g_bench.recPending = 0;
    bool any = false;
    for (const auto& s : g_bench.rep.stages) any = any || s.measured;
    BenchHeader();
    std::wstring path;
    if (any) path = bench::WriteReport(g_bench.rep, BenchFolder(), g_bench.stamp);
    if (!path.empty()) g_bench.lastReport = path;
    g_bench.result = why.empty() ? 1 : 2;  // the title-bar lamp
    g_bench.resultWhy = why;
    g_bench.resultReport = path;
    ++g_con.formRev;
    BenchLog(L"run " + (why.empty() ? std::wstring(L"complete") : L"stopped: " + why) + (path.empty() ? L"" : L"; report " + path));
    Flash(why.empty() ? L"Benchmark done: SETTINGS > Benchmark" : L"Benchmark stopped: " + why, 15000);
}

// Every loop: the block, the run's checks and stages.
void BenchTick() {
    if (g_bench.reportDue && (g_bench.recPending <= 0 || video::Now() >= g_bench.reportBy)) BenchReport();
    if (!BenchOpen()) return;
    auto* b = g_bench.b;
    InterlockedExchange64(&b->viewerTick, static_cast<LONG64>(GetTickCount64()));
    const LONG state = b->hookState, session = b->session;
    if (!g_bench.active) {
        if (state == lsoshare::kBenchHolding && session != g_bench.doneSession) {
            BenchStart();
        } else if (state == lsoshare::kBenchReleased && session != g_bench.doneSession) {
            // a run under way that this viewer did not start (it was restarted during one): it is over
            g_bench.session = session;
            g_bench.doneSession = session;
            BenchCommand(lsoshare::kBenchCmdDone);
            BenchLog(L"session " + std::to_wstring(session) + L" was under way without this viewer: closed");
        }
        return;
    }
    const double now = video::Now();
    const double dt = g_bench.lastT > 0 ? std::max(0.0, now - g_bench.lastT) : 0;
    g_bench.lastT = now;
    // DCS paused (or the hook not stepping): nothing counts, and kBenchResumeS after it neither
    const bool halted = BenchHalted();
    if (halted != g_bench.halt) {
        g_bench.halt = halted;
        if (halted) {
            BenchLog(L"DCS paused");
            if (g_bench.measuring) BenchRollback(now);
        } else {
            g_bench.resumeT = now;
            BenchLog(L"DCS running again");
        }
        BenchStageText(BenchStageName());
    }
    const bool hold = g_bench.halt || now - g_bench.resumeT < kBenchResumeS * std::max(0.34, g_bench.scale);
    if (hold) {  // the clocks stop
        g_bench.phaseT += dt;
        if (g_bench.readyT) g_bench.readyT += dt;
        if (g_bench.noAcT) g_bench.noAcT += dt;
        g_bench.warmT = 0;
    }
    // frame times since the last look (a gap longer than the ring is lost: the viewer was stalled)
    const LONG64 n = b->frames;
    if (n != g_bench.lastFrames) g_bench.lastFrames = n, g_bench.framesT = now;
    LONG64 from = g_bench.read;
    if (n - from > lsoshare::kBenchRing) from = n - lsoshare::kBenchRing;
    if (g_bench.measuring && !hold) {
        for (LONG64 i = from; i < n; ++i) g_bench.samples.push_back(b->frameMs[i % lsoshare::kBenchRing]);
        g_bench.marks.push_back({now, now - g_bench.phaseT, g_bench.samples.size()});
        while (g_bench.marks.size() > 2 && g_bench.marks[1].t < now - kBenchRollbackS - 1) g_bench.marks.erase(g_bench.marks.begin());
    }
    g_bench.read = n;
    // reasons to stop
    if (session != g_bench.session) return BenchFinish(L"a new mission started");
    if (state == lsoshare::kBenchEnded) {
        const std::string r(b->endReason, strnlen(b->endReason, sizeof(b->endReason)));
        return BenchFinish(r.empty() ? L"the mission ended" : Utf8W(r));
    }
    if (b->whoSeq != g_bench.whoSeq) {
        g_bench.whoSeq = b->whoSeq;
        if (g_bench.cur == 0) {  // still the hook alone: wait for the new aircraft or slot, then stage 1 again
            BenchLog(L"player changed before or during stage 1: it starts again");
            BenchWait();
        } else {
            return BenchFinish(L"you changed aircraft or slot");
        }
    }
    if (now - g_bench.framesT > 5 * std::max(1.0, g_bench.scale)) return BenchFinish(L"DCS stopped rendering");
    if (HeaderValid()) {
        if (!g_bench.headerSeen) BenchNoteHeader();
        g_bench.headerSeen = true;
    } else if (g_bench.headerSeen) return BenchFinish(L"DCS stopped sharing");
    // before stage 1: the mission running for kBenchStartS, unpaused, with the player in place
    if (g_bench.waiting) {
        if (hold || !BenchInPlace(now)) g_bench.warmT = 0;
        else if (!g_bench.warmT) g_bench.warmT = now;
        if (g_bench.warmT && now - g_bench.warmT >= kBenchStartS * g_bench.scale) {
            BenchLog(L"the mission has run " + bench::Num(now - g_bench.warmT, 1) + L" s");
            BenchEnter(0);
        }
        return;
    }
    // the stage
    bench::Stage& s = g_bench.rep.stages[g_bench.cur];
    if (!g_bench.measuring) {
        if (hold || !BenchSettle(now)) return;
        if (!s.skipped) {
            g_bench.measuring = true;
            g_bench.phaseT = now;
            g_bench.samples.clear();
            g_bench.marks.clear();
            g_bench.cpuT0 = ProcessCpuSeconds();
            g_bench.cpuWallT = now;
            BenchLog(L"measuring " + s.name);
            return;
        }
    } else {
        if (hold || now - g_bench.phaseT < g_bench.measureS) return;
        s.st = bench::Compute(g_bench.samples);
        BenchNoteHeader();
        s.measured = s.st.frames > 0;
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        s.cpu = (ProcessCpuSeconds() - g_bench.cpuT0) / std::max(0.001, now - g_bench.cpuWallT) / std::max<DWORD>(1, si.dwNumberOfProcessors) * 100;
        BenchLog(s.name + L": " + std::to_wstring(s.st.frames) + L" frames, avg " + bench::Num(s.st.avgFps, 1) + L" fps, 1% low " +
                 bench::Num(s.st.low1Fps, 1) + L", 0.1% low " + bench::Num(s.st.low01Fps, 1) + L", ILARTS CPU " + bench::Num(s.cpu, 1) + L"%" +
                 (s.sources.empty() ? L"" : L" [" + s.sources + L"]"));
    }
    if (g_bench.cur + 1 < g_bench.rep.stages.size()) BenchEnter(g_bench.cur + 1);
    else BenchFinish(L"");
}

// The status LCD while a run is under way: "BENCHMARK 4/12 · 1 VIEW + RECORDING", "BENCHMARK · STARTING", "· PAUSED".
std::wstring BenchStatus() {
    if (!g_bench.active || g_bench.cur >= g_bench.rep.stages.size()) return {};
    std::wstring t = g_bench.waiting ? std::wstring(L"BENCHMARK \x00B7 STARTING")
                                     : L"BENCHMARK " + std::to_wstring(g_bench.cur + 1) + L"/" + std::to_wstring(g_bench.rep.stages.size()) +
                                           L" \x00B7 " + Upper(g_bench.rep.stages[g_bench.cur].name);
    return t + (g_bench.halt ? L" \x00B7 PAUSED" : L"");
}

// The title-bar lamp: "" (none), "armed" (the switch is on), "running", "complete" or "error" (the last run, until it is
// clicked or ILARTS closes); *tip its tooltip. Arming again puts the last result away.
std::wstring BenchLamp(std::wstring* tip) {
    const int armed = GetPrivateProfileIntW(L"benchmark", L"next_mission", 0, app.iniPath.c_str()) != 0;
    if (armed && g_bench.armedWas == 0) g_bench.result = 0;
    g_bench.armedWas = armed;
    if (g_bench.active) {
        *tip = L"Benchmark " + (g_bench.waiting ? std::wstring(L"\x00B7 starting")
                                                : std::to_wstring(g_bench.cur + 1) + L"/" + std::to_wstring(g_bench.rep.stages.size()) +
                                                      L" \x00B7 " + g_bench.rep.stages[g_bench.cur].name) +
               (g_bench.halt ? L" \x00B7 paused" : L"");
        return L"running";
    }
    if (g_bench.result == 1) {
        *tip = L"Benchmark complete\nReport ready";
        return L"complete";
    }
    if (g_bench.result == 2) {
        *tip = L"Benchmark stopped: " + g_bench.resultWhy + (g_bench.resultReport.empty() ? L"" : L"\nReport ready (the stages run)");
        return L"error";
    }
    if (armed) {
        *tip = L"Benchmark armed: next mission";
        return L"armed";
    }
    return {};
}

// A click on the lamp after a run (armed or running: the page opens SETTINGS > Benchmark): its report, if it has one,
// and the lamp goes out.
void BenchLampClick() {
    if (g_bench.active || !g_bench.result) return;
    const std::wstring p = g_bench.resultReport;
    g_bench.result = 0;
    if (p.empty() || GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    TestLog(L"benchmark report: " + p);
    if (!g_test.offscreen) ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The newest report in the folder (SETTINGS > Benchmark > Last report), "" if none.
std::wstring BenchLastReport() {
    if (!g_bench.lastReport.empty() && GetFileAttributesW(g_bench.lastReport.c_str()) != INVALID_FILE_ATTRIBUTES) return g_bench.lastReport;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((BenchFolder() + L"\\ILARTS benchmark *.html").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring best;
    do {
        if (best.empty() || wcscmp(fd.cFileName, best.c_str()) > 0) best = fd.cFileName;  // names sort by date and time
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best.empty() ? best : BenchFolder() + L"\\" + best;
}
void BenchOpenFolder() {
    const std::wstring dir = BenchFolder();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    TestLog(L"benchmarks folder: " + dir);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void BenchOpenLast() {
    const std::wstring p = BenchLastReport();
    TestLog(L"benchmark report: " + p);
    if (!p.empty()) ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- --test-bench-report <folder>: the statistics' unit tests, then a report from made-up frame times in <folder>.
// Exit code: the number of failures.
int BenchSelfTest(const std::wstring& dir, const std::wstring& logPath) {
    int fails = 0;
    std::wstring log;
    auto check = [&](bool ok, const std::wstring& what) {
        log += (ok ? L"ok   " : L"FAIL ") + what + L"\n";
        fails += !ok;
    };
    auto same = [](double a, double b) { return std::fabs(a - b) < 1e-6 * std::max(1.0, std::fabs(b)); };
    {  // percentiles, nearest rank
        std::vector<float> v;
        for (int i = 1; i <= 100; ++i) v.push_back(static_cast<float>(i));
        check(same(bench::Percentile(v, 99), 99) && same(bench::Percentile(v, 100), 100) && same(bench::Percentile(v, 50), 50) &&
                  same(bench::Percentile(v, 0), 1) && same(bench::Percentile(v, 99.9), 100),
              L"percentile of 1..100: p50 50, p99 99, p99.9 100, p0 1, p100 100");
        check(bench::Percentile({}, 99) == 0 && same(bench::Percentile({7.f}, 99.9), 7), L"percentile of nothing / one value");
    }
    {  // constant 10 ms: 100 fps everywhere
        const bench::Stats s = bench::Compute(std::vector<float>(1200, 10.f));
        check(s.frames == 1200 && same(s.avgFps, 100) && same(s.low1Fps, 100) && same(s.low01Fps, 100) && same(s.seconds, 12),
              L"1200 frames of 10 ms: 100 fps, lows 100, 12 s");
    }
    {  // 1000 frames, 11 of them 50 ms: p99 (rank 990) = 50 ms -> 1% low 20 fps; 9 slow ones: rank 990 = 10 ms
        std::vector<float> v(989, 10.f);
        v.insert(v.end(), 11, 50.f);
        bench::Stats s = bench::Compute(v);
        check(same(s.p99Ms, 50) && same(s.low1Fps, 20) && same(s.low01Fps, 20) && same(s.avgMs, (989 * 10.0 + 11 * 50.0) / 1000),
              L"11 slow frames in 1000: 1% low 20 fps, average 10.44 ms");
        std::vector<float> w(991, 10.f);
        w.insert(w.end(), 9, 50.f);
        s = bench::Compute(w);
        check(same(s.p99Ms, 10) && same(s.p999Ms, 50) && same(s.low1Fps, 100) && same(s.low01Fps, 20), L"9 slow frames in 1000: 1% low 100 fps, 0.1% low 20 fps");
        v.push_back(-1.f), v.push_back(std::numeric_limits<float>::quiet_NaN()), v.push_back(0.f);
        check(bench::Compute(v).frames == 1000, L"invalid frame times are left out");
        // order does not matter
        std::reverse(w.begin(), w.end());
        check(same(bench::Compute(w).p999Ms, 50), L"unsorted input");
    }
    {  // deltas: 100 -> 85 fps = -15 fps, +1.8 ms, -15 %
        check(bench::Delta(85, 100) == L"\x2212" L"15 fps \x00B7 +1.8 ms \x00B7 \x2212" L"15%", L"delta 100 -> 85 fps");
        check(bench::Delta(57.7, 60) == L"\x2212" L"2.3 fps \x00B7 +0.7 ms \x00B7 \x2212" L"4%", L"delta 60 -> 57.7 fps");
        check(bench::Delta(120, 120) == L"\x00B1" L"0.0 fps \x00B7 \x00B1" L"0.0 ms \x00B7 \x00B1" L"0%", L"delta of equal values");
    }
    // a made-up run: 12 stages, frame times from a fixed pseudo-random sequence with a few hitches
    bench::Report r;
    r.when = L"2026-11-19 21:04:10";
    r.stages = bench::Plan();
    r.info = {{L"ILARTS", L"test"}, {L"DCS", L"2.9.31.29434"}, {L"Graphics card", L"Test adapter (16 GB)"}, {L"Processor", L"Test CPU, 16 threads"},
              {L"DCS window", L"2560 \x00D7 1440"}, {L"Mission", L"Case I <test> & \"quotes\""}, {L"Carrier", L"CVN-71 (CVN_71)"},
              {L"Player", L"FA-18C_hornet, single player"}, {L"Recording", L"1024 \x00D7 768, 30 fps, 2.2 Mbit/s, graphics card encoder"},
              {L"Also running", L"black box off, streams 0, Spout senders 0, virtual cameras 0, CCTV look on"}};
    uint32_t seed = 12345;
    auto rnd = [&] { return (seed = seed * 1664525u + 1013904223u) / 4294967296.0; };
    for (size_t i = 0; i < r.stages.size(); ++i) {
        bench::Stage& s = r.stages[i];
        const double ms = 8.0 + (s.cams >= 0 ? 0.9 : 0) + std::max(0, s.cams) * 1.1 + (s.rec ? 0.4 + 0.15 * s.cams : 0);
        std::vector<float> v;
        for (int k = 0; k < static_cast<int>(20000 / ms); ++k) {
            double t = ms * (0.92 + 0.16 * rnd());
            if (rnd() < 0.006) t *= 2.2;   // a hitch
            if (rnd() < 0.0008) t *= 5.0;  // a rare stall
            v.push_back(static_cast<float>(t));
        }
        s.st = bench::Compute(v);
        s.measured = true;
        s.cpu = 1.5 + std::max(0, s.cams) * 0.6 + (s.rec ? 2.0 : 0);
        if (s.cams > 0) s.sources = s.rec ? L"PLAT, ISLAND, BOW BUBBLE" : L"ISLAND, BOW BUBBLE";
    }
    r.stages[5].paused = true;
    r.stages.back().measured = false;
    r.stages.back().skipped = true;
    r.stages.back().note = L"fewer cameras than views";
    const std::wstring path = bench::WriteReport(r, dir, L"2026-11-19 21-04-10");
    check(!path.empty(), L"report written: " + path);
    {  // the HTML has a row per stage, escapes text and shows the deltas
        const std::wstring h = bench::ReportHtml(r);
        size_t rows = 0;
        for (size_t p = 0; (p = h.find(L"<tr><td>", p)) != std::wstring::npos; ++p) ++rows;
        check(rows == r.stages.size(), L"one row per stage (" + std::to_wstring(rows) + L")");
        check(h.find(L"Case I &lt;test&gt; &amp; &quot;quotes&quot;") != std::wstring::npos, L"header text escaped");
        check(h.find(L"class=\"d c\"") != std::wstring::npos && h.find(L"skipped") != std::wstring::npos && h.find(L"PAUSED") != std::wstring::npos,
              L"deltas, a skipped stage, a paused stage");
        const std::wstring j = bench::ReportJson(r);
        check(j.find(L"\"low01Fps\"") != std::wstring::npos && j.find(L"\\\"quotes\\\"") != std::wstring::npos, L"JSON with escaped text");
    }
    std::wstring head = L"bench self-test: " + std::to_wstring(fails) + L" failures\n";
    if (!logPath.empty()) bench::WriteUtf8(logPath, head + log);
    return fails;
}
