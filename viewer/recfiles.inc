// Recordings on disk (included by lso_viewer.cpp): one folder per DCS mission, and the storage limit.
//
// Sessions. Every recording (manual, pass, each camera's channel, black box, saved replays) and every still of one
// mission goes into "<recordings>\<date> <HHMM> <mission>" (e.g. "2026-11-17 1432 Case I recovery"), stills into
// its "Stills" subfolder - unless [video] stills_folder is set, which is used as is for every mission. A mission is
// one DCS process + Header::passEpoch (the hook makes a new epoch every mission start or restart); the date and
// time are when this viewer first saw it. The folder is created with its first file, so a mission without
// recordings leaves nothing behind. Before the viewer has seen any mission the folder is named after the viewer's
// start: "<date> <HHMM> No mission".
//
// Storage limit ([video] storage_limit = 1, storage_limit_gb). Before a recording or still starts and whenever a
// file is finished (and every 30 s while recording), a worker thread adds up everything under the recordings
// folder (and the stills folder when it lies elsewhere) plus what the files being written are still expected to
// need, and deletes the oldest files (last write) until that fits. Only files ILARTS wrote are deleted: every
// path ILARTS writes is listed in a ledger, the hidden file ".ilarts-files" in the recordings folder (paths
// relative to it, absolute for a stills folder elsewhere); a leftover "<listed>.part" counts too. A file that is
// open (being written, or by another program) is never deleted: it is opened for deletion without sharing first.
// Folders the deletions emptied, and empty session or Stills folders, are removed; the recordings folder itself
// never is.

// ---------------------------------------------------------------- sessions

struct Session {
    bool seen = false;    // a mission has been seen
    DWORD pid = 0;
    uint32_t epoch = 0;
    std::wstring stamp;    // "2026-11-17 1432"
    std::wstring mission;  // as the hook gives it (until the folder is named)
    std::wstring name;     // the folder's name, fixed by its first file ("" = none yet)
} g_session;

std::wstring SessionStamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf_s(b, L"%04u-%02u-%02u %02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    return b;
}

// A mission name as part of a folder name: no characters Windows refuses, single spaces, at most 48 characters.
std::wstring SessionLabel(const std::wstring& mission) {
    std::wstring n;
    for (wchar_t c : mission) {
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L' ';
        if (c != L' ' || (!n.empty() && n.back() != L' ')) n += c;
    }
    if (n.size() > 48) n.resize(48);
    while (!n.empty() && (n.back() == L' ' || n.back() == L'.')) n.pop_back();
    return n;
}

// Every frame (RecTick): another DCS process or a new mission epoch starts a new session.
void SessionTick() {
    if (g_session.stamp.empty()) g_session.stamp = SessionStamp();  // the viewer's start: before any mission
    if (!HeaderValid()) return;                                       // between missions: the last one's folder stays
    const DWORD pid = app.header->producerPid;
    const uint32_t epoch = app.header->passEpoch;
    if (epoch == 0) return;  // a new header the hook has not filled in yet
    if (!g_session.seen || pid != g_session.pid || epoch != g_session.epoch) {
        g_session = Session{};
        g_session.seen = true;
        g_session.pid = pid;
        g_session.epoch = epoch;
        g_session.stamp = SessionStamp();
    }
    if (g_session.name.empty()) {
        const char* m = app.header->missionName;
        g_session.mission = Utf8W(std::string(m, strnlen(m, sizeof(app.header->missionName))));
    }
}

std::wstring SessionFolderName() {
    if (!g_session.name.empty()) return g_session.name;
    if (g_session.stamp.empty()) g_session.stamp = SessionStamp();
    std::wstring label = SessionLabel(g_session.mission);
    if (label.empty()) label = g_session.seen ? L"Mission" : L"No mission";
    return g_session.stamp + L" " + label;
}

// The current mission's folder, created (and its name fixed) when create is set. "" if !create and none exists yet.
std::wstring SessionFolder(bool create = true) {
    if (g_session.name.empty()) {
        if (!create) return {};
        // a folder of that name is another session's (a mission restarted within the same minute): "... (2)"
        const std::wstring base = SessionFolderName();
        g_session.name = base;
        for (int i = 2; i < 100 && GetFileAttributesW((VideoFolder() + L"\\" + g_session.name).c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
            g_session.name = base + L" (" + std::to_wstring(i) + L")";
    }
    const std::wstring dir = VideoFolder() + L"\\" + g_session.name;
    if (create) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

// [video] stills_folder as set ("" = a Stills folder in each mission's folder).
std::wstring CustomStillsFolder() {
    wchar_t buf[MAX_PATH] = {};
    GetPrivateProfileStringW(L"video", L"stills_folder", L"", buf, MAX_PATH, app.iniPath.c_str());
    return buf;
}

// Where the next still goes (created when create is set).
std::wstring StillsFolder(bool create = true) {
    std::wstring dir = CustomStillsFolder();
    if (dir.empty()) {
        const std::wstring s = SessionFolder(create);
        dir = (s.empty() ? VideoFolder() + L"\\" + SessionFolderName() : s) + L"\\Stills";
    }
    if (create) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

// FOLDER: the current mission's folder once it has files, else the recordings folder.
void OpenVideoFolder() {
    std::wstring dir = SessionFolder(false);
    if (dir.empty() || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) dir = VideoFolder();
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// SETTINGS > Installation > Logs: where the DLL writes ILARTS.log (src/common.cpp ResolveLogPath: %LOCALAPPDATA%\ILARTS,
// else the temp folder). Made if DCS has not run with ILARTS yet.
std::wstring LogsFolder() {
    wchar_t base[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) n = GetTempPathW(MAX_PATH, base);
    std::wstring dir(base, n < MAX_PATH ? n : 0);
    if (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    return dir + L"\\ILARTS";
}
void OpenLogsFolder() {
    const std::wstring dir = LogsFolder();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);  // ERROR_ALREADY_EXISTS is fine
    TestLog(L"logs folder: " + dir);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- storage limit

namespace store {

// A path as a key: lower case, backslashes, no doubled separators (but a leading \\server).
std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    std::wstring k;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i] == L'/' ? L'\\' : s[i];
        if (c == L'\\' && i > 1 && !k.empty() && k.back() == L'\\') continue;
        k += c;
    }
    return k;
}
bool Under(const std::wstring& path, const std::wstring& dir) {  // path lies inside dir (both lower case or not)
    return path.size() > dir.size() + 1 && !_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) && (path[dir.size()] == L'\\' || path[dir.size()] == L'/');
}
std::wstring Parent(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}
std::wstring Trim(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

// The files ILARTS wrote under one recordings folder (keys: absolute paths, lower case).
struct Ledger {
    std::wstring root;  // as loaded ("" = not loaded)
    std::set<std::wstring> keys;
    std::map<std::wstring, std::wstring> paths;  // key -> path as written
    std::wstring file() const { return root + L"\\.ilarts-files"; }
    std::wstring line(const std::wstring& path) const { return Under(path, root) ? path.substr(root.size() + 1) : path; }

    // Loads the ledger of root; entries whose file (and .part) no longer exist are dropped.
    void load(const std::wstring& r) {
        root = Trim(r);
        keys.clear();
        paths.clear();
        std::string text;
        HANDLE h = CreateFileW(file().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[65536];
            DWORD got = 0;
            while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) text.append(buf, got);
            CloseHandle(h);
        }
        bool dropped = false;
        for (size_t a = 0; a < text.size();) {
            size_t e = text.find('\n', a);
            if (e == std::string::npos) e = text.size();
            std::string l = text.substr(a, e - a);
            a = e + 1;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (l.empty()) continue;
            const int n = MultiByteToWideChar(CP_UTF8, 0, l.data(), static_cast<int>(l.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, l.data(), static_cast<int>(l.size()), w.data(), n);
            const std::wstring path = (w.size() > 1 && w[1] == L':') || w.rfind(L"\\\\", 0) == 0 ? w : root + L"\\" + w;
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW((path + L".part").c_str()) == INVALID_FILE_ATTRIBUTES) {
                dropped = true;
                continue;
            }
            keys.insert(Lower(path));
            paths[Lower(path)] = path;
        }
        if (dropped) save();
    }
    // Adds one path (appended to the file).
    void note(const std::wstring& path) {
        const std::wstring k = Lower(path);
        if (!keys.insert(k).second) return;
        paths[k] = path;
        append(line(path));
    }
    void append(const std::wstring& l) {
        HANDLE h = CreateFileW(file().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        const std::string u = Utf8(l) + "\r\n";
        DWORD put = 0;
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &put, nullptr);
        CloseHandle(h);
    }
    void forget(const std::wstring& path) {
        keys.erase(Lower(path));
        paths.erase(Lower(path));
    }
    bool has(const std::wstring& path) const {
        const std::wstring k = Lower(path);
        if (keys.count(k)) return true;
        return k.size() > 5 && k.compare(k.size() - 5, 5, L".part") == 0 && keys.count(k.substr(0, k.size() - 5));
    }
    // Rewrites the file (written beside it, then swapped in).
    void save() {
        std::string all;
        for (const auto& kv : paths) all += Utf8(line(kv.second)) + "\r\n";
        const std::wstring tmp = file() + L".new";
        DeleteFileW(tmp.c_str());
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD put = 0;
        const bool ok = WriteFile(h, all.data(), static_cast<DWORD>(all.size()), &put, nullptr) && put == all.size();
        CloseHandle(h);
        if (!ok || !MoveFileExW(tmp.c_str(), file().c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
    }
    static std::string Utf8(const std::wstring& w) {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }
};

// A file being written: what it may still grow to. rolling = no known end (a manual recording): bytes are kept free
// beyond its current size; else bytes is its expected final size.
struct Reserve {
    std::wstring path;
    unsigned long long bytes = 0;
    bool rolling = false;
};
struct Job {
    std::wstring root;                  // the recordings folder (the ledger's)
    std::vector<std::wstring> others;   // further folders counted (a stills folder elsewhere)
    unsigned long long limit = 0;       // bytes
    std::vector<Reserve> reserves;
    unsigned long long extra = 0;       // files about to be written (no path yet)
    std::vector<std::wstring> keepDirs; // folders never removed (the current mission's)
};
struct Result {
    int deleted = 0, dirsRemoved = 0;
    unsigned long long freed = 0, total = 0, need = 0;
    bool over = false;  // still over the limit: nothing more ILARTS may delete
};

struct File {
    std::wstring path;
    unsigned long long size;
    ULONGLONG written;
};
void Scan(const std::wstring& dir, std::vector<File>& files, unsigned long long& total, int depth) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 8 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                Scan(dir + L"\\" + fd.cFileName, files, total, depth + 1);
            continue;
        }
        const unsigned long long size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        total += size;
        files.push_back({dir + L"\\" + fd.cFileName, size, (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Deletes path unless anything has it open (opened for deletion without sharing: fails while it is in use).
bool DeleteIfClosed(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), DELETE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
}

// "2026-11-17 1432 ..." (a mission's folder) or "Stills"
bool SessionLike(const std::wstring& name) {
    if (!_wcsicmp(name.c_str(), L"Stills")) return true;
    const wchar_t* d = L"dddd-dd-dd dddd ";
    if (name.size() < wcslen(d)) return false;
    for (size_t i = 0; d[i]; ++i)
        if (d[i] == L'd' ? !iswdigit(name[i]) : name[i] != d[i]) return false;
    return true;
}
bool DirEmpty(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool empty = true;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) empty = false;
    } while (empty && FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}
// Removes the empty folders below dir (deepest first) that the deletions emptied or that are mission / Stills folders.
int RemoveEmpty(const std::wstring& dir, const std::set<std::wstring>& touched, const std::set<std::wstring>& keep, int depth) {
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    std::vector<std::wstring> subs;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && wcscmp(fd.cFileName, L".") &&
            wcscmp(fd.cFileName, L".."))
            subs.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const auto& s : subs) {
        const std::wstring sub = dir + L"\\" + s;
        if (depth < 8) n += RemoveEmpty(sub, touched, keep, depth + 1);
        const std::wstring k = Lower(sub);
        if (keep.count(k) || !(touched.count(k) || SessionLike(s))) continue;
        if (DirEmpty(sub) && RemoveDirectoryW(sub.c_str())) ++n;
    }
    return n;
}

// One pass of the limit (worker thread, or the self-test).
Result Enforce(const Job& job, Ledger& ledger) {
    Result r;
    const std::wstring root = Trim(job.root);
    if (_wcsicmp(ledger.root.c_str(), root.c_str())) ledger.load(root);
    std::vector<std::wstring> dirs{root};
    for (const auto& o : job.others) {
        const std::wstring t = Trim(o);
        if (!t.empty() && _wcsicmp(t.c_str(), root.c_str()) && !Under(t, root)) dirs.push_back(t);
    }
    std::vector<File> files;
    for (const auto& d : dirs) Scan(d, files, r.total, 0);
    std::set<std::wstring> busy;  // being written: never deleted
    unsigned long long pending = job.extra;
    for (const auto& rs : job.reserves) {
        busy.insert(Lower(rs.path));
        unsigned long long now = 0;
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(rs.path.c_str(), GetFileExInfoStandard, &a)) now = (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
        pending += rs.rolling ? rs.bytes : (rs.bytes > now ? rs.bytes - now : 0);
    }
    r.need = r.total + pending;
    std::vector<File> mine;
    for (auto& f : files)
        if (ledger.has(f.path) && !busy.count(Lower(f.path))) mine.push_back(f);
    std::sort(mine.begin(), mine.end(), [](const File& a, const File& b) { return a.written < b.written; });
    std::set<std::wstring> touched;
    bool changed = false;
    for (const auto& f : mine) {
        if (r.need <= job.limit) break;
        if (!DeleteIfClosed(f.path)) continue;
        r.need -= std::min(r.need, f.size);
        r.freed += f.size;
        ++r.deleted;
        const std::wstring lf = Lower(f.path);
        if (ledger.keys.count(lf)) ledger.forget(f.path), changed = true;
        for (std::wstring d = Parent(lf);; d = Parent(d)) {  // its folder and those above it, up to (not) the root
            bool inside = false;
            for (const auto& rd : dirs)
                if (Under(d, Lower(rd))) inside = true;
            if (!inside) break;
            touched.insert(d);
        }
    }
    if (changed) ledger.save();
    r.over = r.need > job.limit;
    std::set<std::wstring> keep;
    for (const auto& k : job.keepDirs) {  // and every folder above them
        for (std::wstring d = Lower(Trim(k)); d.size() > 3; d = Parent(d)) keep.insert(d);
    }
    for (const auto& rs : job.reserves)
        for (std::wstring d = Parent(Lower(rs.path)); d.size() > 3; d = Parent(d)) keep.insert(d);
    for (const auto& d : dirs) r.dirsRemoved += RemoveEmpty(d, touched, keep, 0);
    return r;
}

// The worker: notes paths into the ledger and runs the limit, off the window's thread.
struct Worker {
    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    std::vector<std::pair<std::wstring, std::wstring>> notes;  // (recordings folder, path)
    bool haveJob = false;
    Job job;
    Ledger ledger;
    // results for the window
    std::atomic<int> deleted{0}, runs{0}, dirsRemoved{0};
    std::atomic<bool> over{false};
    std::atomic<unsigned long long> freed{0};

    void start() {
        if (started) return;
        started = true;
        std::thread([this] { loop(); }).detach();
    }
    void loop() {
        for (;;) {
            std::vector<std::pair<std::wstring, std::wstring>> n;
            Job j;
            bool run = false;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [&] { return !notes.empty() || haveJob; });
                n.swap(notes);
                if (haveJob) j = std::move(job), haveJob = false, run = true;
            }
            for (const auto& [root, path] : n) {
                if (_wcsicmp(ledger.root.c_str(), Trim(root).c_str())) ledger.load(root);
                ledger.note(path);
            }
            if (!run) continue;
            const Result r = Enforce(j, ledger);
            deleted += r.deleted;
            dirsRemoved += r.dirsRemoved;
            freed += r.freed;
            over = r.over;
            ++runs;
        }
    }
    void note(const std::wstring& root, const std::wstring& path) {
        start();
        {
            std::lock_guard<std::mutex> l(m);
            notes.emplace_back(root, path);
        }
        cv.notify_one();
    }
    // The newest job replaces one not started yet (it carries every file being written); what was about to be
    // written adds up.
    void post(Job j) {
        start();
        {
            std::lock_guard<std::mutex> l(m);
            if (haveJob) j.extra += job.extra;
            job = std::move(j);
            haveJob = true;
        }
        cv.notify_one();
    }
};

}  // namespace store

struct StorageLimit {
    bool on = false;
    double gb = 50;
    store::Worker worker;
    bool requested = false;     // run once more soon (a file was finished)
    ULONGLONG lastRun = 0;
    bool overShown = false;
} g_storage;

void StorageReadSettings() {
    wchar_t b[64] = {};
    GetPrivateProfileStringW(L"video", L"storage_limit", L"0", b, 64, app.iniPath.c_str());
    g_storage.on = _wtof(b) > 0.5;
    GetPrivateProfileStringW(L"video", L"storage_limit_gb", L"50", b, 64, app.iniPath.c_str());
    g_storage.gb = std::clamp(b[0] ? _wtof(b) : 50.0, 0.01, 100000.0);  // the dialog: 1 GB and up (less: tests)
    g_storage.requested = true;  // a new limit (or the start) applies at once
}

// Lists a path ILARTS is about to write (the limit may delete it later).
void StorageNote(const std::wstring& path) { g_storage.worker.note(VideoFolder(), path); }

// Bytes per second of a recording file (the H.264 bit rate with room for its peaks).
// bitrate: the channel's (a composite's is higher), 0 = the recording settings'.
double RecBytesPerSecond(int bitrate = 0) {
    if (bitrate <= 0) bitrate = g_engCfg.bitrate > 0 ? g_engCfg.bitrate : 2200000;
    return std::max(bitrate, 300000) / 8.0 * 1.25;
}

// Makes room for what is being written plus extra bytes about to be (no-op with the limit off).
void StorageRequest(unsigned long long extra = 0) {
    g_storage.requested = false;
    if (!g_storage.on) return;
    g_storage.lastRun = GetTickCount64();
    store::Job j;
    j.root = VideoFolder();
    const std::wstring stills = CustomStillsFolder();
    if (!stills.empty()) j.others.push_back(stills);
    j.limit = static_cast<unsigned long long>(g_storage.gb * 1e9);
    j.extra = extra;
    for (const auto& r : g_rec.recs)
        for (size_t i = 0; i < r.paths.size(); ++i) {  // paths[i] is written by extra[i] (RecBegin)
            const double bps = RecBytesPerSecond(i < r.extra.size() ? r.extra[i]->eng.config().bitrate : 0);
            store::Reserve rs;
            rs.path = r.paths[i];
            rs.rolling = r.manual;  // no end known: keep 5 minutes free beyond it
            rs.bytes = static_cast<unsigned long long>(bps * (r.manual ? 300.0 : g_rec.preroll + g_rec.autoMaxS + g_rec.postroll));
            j.reserves.push_back(rs);
        }
    const std::wstring session = SessionFolder(false);
    if (!session.empty()) j.keepDirs.push_back(session), j.keepDirs.push_back(session + L"\\Stills");
    g_storage.worker.post(std::move(j));
}

// Every frame: a finished file (or 30 s of recording) runs the limit again; says so once when nothing more can go.
void StorageTick() {
    if (!g_storage.on) return;
    const ULONGLONG now = GetTickCount64();
    if ((g_storage.requested && now - g_storage.lastRun >= 1000) || (!g_rec.recs.empty() && now - g_storage.lastRun >= 30000)) StorageRequest();
    const bool over = g_storage.worker.over;
    if (over && !g_storage.overShown) RecStatus(L"Storage limit reached: no older recordings to delete", 15000);
    g_storage.overShown = over;
}

// ---------------------------------------------------------------- self-test (--test-storage <folder> [--test-log f])
// Builds a recordings folder in <folder>, runs the limit on it and checks what is left. Returns the process exit code.
int StorageSelfTest(const std::wstring& base, const std::wstring& log) {
    FILE* lf = nullptr;
    if (!log.empty()) _wfopen_s(&lf, log.c_str(), L"w, ccs=UTF-8");
    int failures = 0;
    auto say = [&](const std::wstring& s) {
        if (lf) fwprintf(lf, L"%s\n", s.c_str());
        wprintf(L"%s\n", s.c_str());
    };
    auto check = [&](bool ok, const std::wstring& what) {
        say((ok ? L"ok   " : L"FAIL ") + what);
        if (!ok) ++failures;
    };
    const std::wstring root = base + L"\\rec", stills = base + L"\\stills";
    SHCreateDirectoryExW(nullptr, root.c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, stills.c_str(), nullptr);
    DeleteFileW((root + L"\\.ilarts-files").c_str());
    const unsigned long long MB = 1000000;
    ULONGLONG t0 = 0;
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        t0 = ((static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 100ULL * 86400 * 10000000;  // 100 days ago
    }
    store::Ledger ledger;
    ledger.load(root);
    auto make = [&](const std::wstring& rel, int age, bool listed, const std::wstring& listAs = L"") {
        const std::wstring p = rel.find(L':') != std::wstring::npos ? rel : root + L"\\" + rel;
        SHCreateDirectoryExW(nullptr, store::Parent(p).c_str(), nullptr);
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        std::vector<char> data(MB, 'x');
        DWORD put = 0;
        WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr);
        const ULONGLONG t = t0 + static_cast<ULONGLONG>(age) * 3600ULL * 10000000;
        FILETIME ft{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)};
        SetFileTime(h, nullptr, nullptr, &ft);
        CloseHandle(h);
        if (listed) ledger.note(listAs.empty() ? p : listAs);
        return p;
    };
    auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    // 9 files of 1 MB: 7 of ILARTS's, a video and a text file of the user's
    const std::wstring partBase = root + L"\\2026-01-02 1000 Mid\\Stills\\b.png";
    const std::wstring bPart = make(L"2026-01-02 1000 Mid\\Stills\\b.png.part", 0, true, partBase);  // a leftover: its PNG is listed
    const std::wstring user = make(L"user.mp4", 0, false);
    const std::wstring notes = make(L"notes.txt", 0, false);
    const std::wstring a = make(L"2026-01-01 1000 Old\\a.mp4", 1, true);
    const std::wstring aPng = make(L"2026-01-01 1000 Old\\Stills\\a.png", 2, true);
    const std::wstring s = make(stills + L"\\s.png", 3, true);
    const std::wstring b = make(L"2026-01-02 1000 Mid\\b.mp4", 4, true);
    const std::wstring c = make(L"2026-01-03 1000 New\\c.mp4", 5, true);
    const std::wstring d = make(L"2026-01-03 1000 New\\d.mp4", 6, true);
    SHCreateDirectoryExW(nullptr, (root + L"\\User folder").c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, (root + L"\\2026-01-04 0900 Empty\\Stills").c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, (root + L"\\2026-01-05 0900 Current").c_str(), nullptr);
    // c is open (being written by someone): it must survive even though it is old enough to go
    HANDLE hold = CreateFileW(c.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);

    store::Ledger fresh;  // as the worker would: from the file
    fresh.load(root);
    check(fresh.keys.size() == 7, L"ledger lists 7 files (" + std::to_wstring(fresh.keys.size()) + L")");
    store::Job j;
    j.root = root;
    j.others.push_back(stills);
    const unsigned long long slack = 65536;  // the ledger is in the folder too
    j.limit = 4 * MB + slack;
    j.extra = 1 * MB;  // a file about to be written
    j.keepDirs.push_back(root + L"\\2026-01-05 0900 Current");
    store::Result r = store::Enforce(j, fresh);
    say(L"pass 1: total " + std::to_wstring(r.total) + L", deleted " + std::to_wstring(r.deleted) + L", freed " + std::to_wstring(r.freed) +
        L", folders removed " + std::to_wstring(r.dirsRemoved) + L", over " + std::to_wstring(r.over));
    check(r.total >= 9 * MB && r.total < 9 * MB + slack, L"total counts every file, the user's too");
    check(!exists(bPart) && !exists(a) && !exists(aPng) && !exists(s) && !exists(b) && !exists(d), L"the oldest of ILARTS's files deleted (.part leftover, stills folder elsewhere too)");
    check(exists(c), L"an open file is kept");
    check(exists(user) && exists(notes), L"files ILARTS did not write are kept");
    check(!r.over && r.need <= j.limit, L"fits the limit (need " + std::to_wstring(r.need) + L")");
    check(!exists(root + L"\\2026-01-01 1000 Old") && !exists(root + L"\\2026-01-02 1000 Mid"), L"emptied mission folders removed");
    check(!exists(root + L"\\2026-01-04 0900 Empty"), L"an empty mission folder removed");
    check(exists(root + L"\\User folder"), L"an empty folder of the user's kept");
    check(exists(root + L"\\2026-01-05 0900 Current"), L"the current mission's folder kept");
    check(exists(root + L"\\2026-01-03 1000 New") && exists(root), L"a folder with a file kept, the recordings folder kept");
    check(exists(stills), L"the stills folder elsewhere kept");
    // pass 2: an impossible limit - the user's files and the open one cannot go
    j.limit = 1 * MB + slack;
    j.extra = 0;
    r = store::Enforce(j, fresh);
    check(r.over && r.deleted == 0 && exists(c), L"over the limit with nothing left to delete: reported, the open file kept");
    CloseHandle(hold);
    // pass 3: the file is closed now; a reserve keeps a file being written
    const std::wstring e = make(L"2026-01-05 0900 Current\\e.mp4", 7, true);
    fresh.load(root);
    j.limit = 4 * MB + slack;
    store::Reserve rs;
    rs.path = e;
    rs.bytes = 2 * MB;  // expected final size: 1 MB more to come
    j.reserves.push_back(rs);
    r = store::Enforce(j, fresh);
    say(L"pass 3: total " + std::to_wstring(r.total) + L", need " + std::to_wstring(r.need) + L", deleted " + std::to_wstring(r.deleted));
    check(!exists(c) && exists(e) && !r.over, L"closed file deleted, the file being written kept, room for its remainder");
    check(!exists(root + L"\\2026-01-03 1000 New"), L"its emptied folder removed");
    // the ledger on disk no longer lists deleted files
    store::Ledger after;
    after.load(root);
    check(after.keys.size() == 1 && after.has(e), L"ledger rewritten (" + std::to_wstring(after.keys.size()) + L" entries)");
    // folder names
    check(SessionLabel(L"Case I: \"night\" / trap?") == L"Case I night trap", L"mission name cleaned: '" + SessionLabel(L"Case I: \"night\" / trap?") + L"'");
    check(SessionLabel(std::wstring(80, L'a')).size() == 48, L"mission name shortened");
    say(failures ? L"STORAGE TEST FAILED" : L"STORAGE TEST PASSED");
    if (lf) fclose(lf);
    return failures ? 1 : 0;
}
