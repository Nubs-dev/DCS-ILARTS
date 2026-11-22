// ---------------------------------------------------------------- updates (GitHub Releases)
// (Included by lso_viewer.cpp after repair.inc.)
//
// The releases of the public repository (kUpdateRepo) are the update feed: no server of our own. Once a day (and at
// start when the last check is older) a worker thread asks GitHub's REST API for the newest release
// (GET https://api.github.com/repos/<repo>/releases/latest: drafts and pre-releases are never "latest"),
// unauthenticated, with a generic User-Agent (ILARTS-updater/<version>) and nothing else about the user. A failed
// check (offline, GitHub's rate limit of 60 requests an hour per address, an error) is silent and tried again after
// 1, 2, 4 ... hours (at most a day; after the rate limit's reset time). [update] in ILARTS.ini keeps the schedule and
// the newest release seen, so the title bar shows an update at once after a restart.
//
// The title bar's indicator (state.update) appears only when a newer release exists and the user has not skipped
// it. Its menu: INSTALL (installer installations: downloads DCS-ILARTS-Setup-<version>.exe, checks its size and
// SHA-256 against the API's "digest" and its Authenticode signature, closes ILARTS and starts it), RELEASE NOTES
// (the release page), SKIP THIS VERSION. A mod-manager installation (no Inno Setup uninstaller beside the mod) gets
// RELEASE PAGE instead of INSTALL: the installer would put files under the mod manager's feet.
//
// Versions: numbers and dots, a release's tag is v<version>. A fourth part marks a test build of the first three
// (1.0.29.2 comes before 1.0.29); development builds (0.0.0) never check.
//
// Tests: --test-update-feed <file or http://127.0.0.1:port/...> stands in for the API (and allows loopback http for
// the asset), --test-update-version, --test-update-mode installer|zip, --test-update-dcs 0|1, --test-update-signer;
// --test-update-selftest runs the unit tests (versions, JSON, releases, backoff). docs/DEVELOPMENT.md, "Updates".

constexpr wchar_t kUpdateRepo[] = L"Nubs-dev/DCS-ILARTS";  // owner/name of the public repository: its releases
constexpr wchar_t kUpdateParams[] = L"/SILENT /SP- /ILARTSUPDATE=1";  // the installer, started by INSTALL (ILARTS.iss)

namespace upd {

// ---- versions

// "1.0.29", "v1.1.0", "1.0.29.2": numbers and dots, an optional leading v. False for anything else.
inline bool ParseVersion(const std::string& in, std::vector<unsigned>* out) {
    std::string s = in;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    out->clear();
    if (s.empty()) return false;
    unsigned long long cur = 0;
    bool digit = false;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (!digit) return false;
            out->push_back(static_cast<unsigned>(cur));
            cur = 0;
            digit = false;
            continue;
        }
        if (s[i] < '0' || s[i] > '9') return false;
        cur = cur * 10 + static_cast<unsigned>(s[i] - '0');
        digit = true;
        if (cur > 0xFFFFFFFFull) return false;
    }
    return out->size() <= 8;
}

// Release order: < 0 a is older, 0 the same, > 0 a is newer. The first three numbers are the release; anything after
// them marks a test build of that release (docs/RELEASING.md): 1.0.28 < 1.0.29.1 < 1.0.29.2 < 1.0.29 < 1.0.30.
// Missing numbers are 0 (1.1 = 1.1.0 = 1.1.0.0).
inline int CompareVersions(std::vector<unsigned> a, std::vector<unsigned> b) {
    a.resize(std::max<size_t>(a.size(), 3));
    b.resize(std::max<size_t>(b.size(), 3));
    for (size_t i = 0; i < 3; ++i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    while (a.size() > 3 && a.back() == 0) a.pop_back();
    while (b.size() > 3 && b.back() == 0) b.pop_back();
    const bool ta = a.size() > 3, tb = b.size() > 3;  // a test build
    if (ta != tb) return ta ? -1 : 1;
    for (size_t i = 3; i < std::max(a.size(), b.size()); ++i) {
        const unsigned x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
inline int CompareVersions(const std::string& a, const std::string& b) {
    std::vector<unsigned> va, vb;
    ParseVersion(a, &va);
    ParseVersion(b, &vb);
    return CompareVersions(va, vb);
}

// ---- a small JSON reader (the API's answer; depth and size limited)

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> items;      // array elements, or an object's values
    std::vector<std::string> keys;  // an object's keys
    const JVal* get(const char* k) const {
        if (type != Obj) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k) return &items[i];
        return nullptr;
    }
    std::string str(const char* k) const {
        const JVal* v = get(k);
        return v && v->type == Str ? v->s : std::string();
    }
    bool flag(const char* k) const {
        const JVal* v = get(k);
        return v && v->type == Bool && v->b;
    }
    double num(const char* k, double def) const {
        const JVal* v = get(k);
        return v && v->type == Num ? v->n : def;
    }
};

class JParser {
public:
    explicit JParser(const std::string& t) : p_(t.data()), e_(t.data() + t.size()) {}
    bool parse(JVal* out) {
        if (!value(*out)) return false;
        ws();
        return p_ == e_;
    }

private:
    const char *p_, *e_;
    int depth_ = 0;
    void ws() {
        while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }
    bool lit(const char* w) {
        const size_t n = strlen(w);
        if (static_cast<size_t>(e_ - p_) < n || memcmp(p_, w, n) != 0) return false;
        p_ += n;
        return true;
    }
    static void put(std::string& o, unsigned c) {  // a code point as UTF-8
        if (c < 0x80) o += static_cast<char>(c);
        else if (c < 0x800) o += static_cast<char>(0xC0 | c >> 6), o += static_cast<char>(0x80 | (c & 0x3F));
        else if (c < 0x10000) o += static_cast<char>(0xE0 | c >> 12), o += static_cast<char>(0x80 | (c >> 6 & 0x3F)), o += static_cast<char>(0x80 | (c & 0x3F));
        else o += static_cast<char>(0xF0 | c >> 18), o += static_cast<char>(0x80 | (c >> 12 & 0x3F)), o += static_cast<char>(0x80 | (c >> 6 & 0x3F)),
             o += static_cast<char>(0x80 | (c & 0x3F));
    }
    bool hex4(unsigned* v) {
        if (e_ - p_ < 4) return false;
        *v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            *v <<= 4;
            if (c >= '0' && c <= '9') *v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') *v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') *v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        if (p_ >= e_ || *p_ != '"') return false;
        ++p_;
        while (p_ < e_ && *p_ != '"') {
            if (static_cast<unsigned char>(*p_) < 0x20) return false;
            if (*p_ != '\\') {
                o += *p_++;
                continue;
            }
            if (++p_ >= e_) return false;
            const char c = *p_++;
            switch (c) {
                case '"': case '\\': case '/': o += c; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    unsigned u = 0;
                    if (!hex4(&u)) return false;
                    if (u >= 0xD800 && u < 0xDC00 && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {  // a surrogate pair
                        p_ += 2;
                        unsigned lo = 0;
                        if (!hex4(&lo)) return false;
                        u = lo >= 0xDC00 && lo < 0xE000 ? 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00) : 0xFFFD;
                    } else if (u >= 0xD800 && u < 0xE000) {
                        u = 0xFFFD;
                    }
                    put(o, u);
                    break;
                }
                default: return false;
            }
        }
        if (p_ >= e_) return false;
        ++p_;
        return true;
    }
    bool value(JVal& v) {
        ws();
        if (p_ >= e_ || depth_ > 64) return false;
        const char c = *p_;
        if (c == '{' || c == '[') {
            ++depth_;
            ++p_;
            v.type = c == '{' ? JVal::Obj : JVal::Arr;
            ws();
            if (p_ < e_ && *p_ == (c == '{' ? '}' : ']')) {
                ++p_;
                --depth_;
                return true;
            }
            for (;;) {
                if (c == '{') {
                    ws();
                    std::string k;
                    if (!string(k)) return false;
                    ws();
                    if (p_ >= e_ || *p_ != ':') return false;
                    ++p_;
                    v.keys.push_back(std::move(k));
                }
                v.items.emplace_back();
                if (!value(v.items.back())) return false;
                ws();
                if (p_ < e_ && *p_ == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < e_ && *p_ == (c == '{' ? '}' : ']')) {
                    ++p_;
                    --depth_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') return v.type = JVal::Str, string(v.s);
        if (lit("true")) return v.type = JVal::Bool, v.b = true, true;
        if (lit("false")) return v.type = JVal::Bool, v.b = false, true;
        if (lit("null")) return v.type = JVal::Null, true;
        const char* s = p_;
        while (p_ < e_ && (strchr("+-.eE", *p_) || (*p_ >= '0' && *p_ <= '9'))) ++p_;
        if (p_ == s) return false;
        const std::string t(s, p_);
        char* end = nullptr;
        v.type = JVal::Num;
        v.n = strtod(t.c_str(), &end);
        return end == t.c_str() + t.size();
    }
};
inline bool ParseJson(const std::string& text, JVal* out) {
    *out = JVal();
    return JParser(text).parse(out);
}

// ---- a release, as the API describes it

struct Release {
    std::string version;   // the tag without its "v"
    std::string page;      // the release's page (html_url)
    std::string asset;     // DCS-ILARTS-Setup-<version>.exe ("" when the release has none)
    std::string url;       // its browser_download_url
    std::string sha256;    // 64 lower-case hex digits from the asset's "digest" ("" when GitHub gave none)
    long long size = -1;   // the asset's size in bytes
    bool prerelease = false, draft = false;
};
inline std::string InstallerName(const std::string& version) { return "DCS-ILARTS-Setup-" + version + ".exe"; }

// The API's release object -> r. False (and why) when it is not a usable release: no tag of the form v<version>.
inline bool ReadRelease(const std::string& json, Release* r, std::string* why) {
    *r = Release();
    JVal v;
    if (!ParseJson(json, &v) || v.type != JVal::Obj) return *why = "the answer is not a JSON object", false;
    const std::string tag = v.str("tag_name");
    std::vector<unsigned> parts;
    if (tag.empty() || (tag[0] != 'v' && tag[0] != 'V') || !ParseVersion(tag, &parts))
        return *why = "no release tag of the form v<version> (\"" + tag.substr(0, 40) + "\")", false;
    r->version = tag.substr(1);
    r->prerelease = v.flag("prerelease");
    r->draft = v.flag("draft");
    r->page = v.str("html_url");
    if (r->page.rfind("https://github.com/", 0) != 0) r->page.clear();  // a page on GitHub, or none (the caller's fallback)
    if (const JVal* assets = v.get("assets"); assets && assets->type == JVal::Arr)
        for (const JVal& a : assets->items) {
            if (a.str("name") != InstallerName(r->version)) continue;
            r->asset = a.str("name");
            r->url = a.str("browser_download_url");
            r->size = static_cast<long long>(a.num("size", -1));
            std::string d = a.str("digest");
            for (auto& ch : d) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (d.size() == 7 + 64 && d.rfind("sha256:", 0) == 0 && d.find_first_not_of("0123456789abcdef", 7) == std::string::npos) r->sha256 = d.substr(7);
            break;
        }
    return true;
}

// The wait before the next check after `fails` failures in a row: 1, 2, 4, 8, 16 hours, then a day.
inline long long BackoffSeconds(int fails) { return fails <= 0 ? 86400 : std::min<long long>(86400, 3600LL << std::min(fails - 1, 5)); }

}  // namespace upd

// ---- state

struct UpdateCtl {
    // switches (ParseArgs)
    std::wstring testFeed, testVersion, testMode, testSigner, testSelf;
    int testDcs = -1;           // --test-update-dcs 0|1: DCS taken as running or not
    // main thread
    bool enabled = true;        // [update] check
    enum Phase { Idle, Available, Downloading, Failed } phase = Idle;
    upd::Release rel;           // the newest release known (kept in [update])
    bool have = false;
    std::wstring error;         // why the last install failed (the tooltip)
    long long nextCheck = 0;    // unix seconds
    int fails = 0;
    ULONGLONG notBefore = 0;    // the first check: a few seconds after start
    bool installerMode = false;  // installed by the installer (UpdInstallerMode, at start)
    bool dcs = false;           // DCS running (sampled every 2 s while there is an update)
    ULONGLONG dcsAt = 0;
    std::wstring launch;        // the verified installer, started once ILARTS has closed
    HANDLE lock = INVALID_HANDLE_VALUE;  // held open (no writers) from its verification until it has started
    // the worker
    std::thread worker;
    std::atomic<bool> busy{false}, cancel{false};
    std::atomic<long long> got{0}, total{0};
    std::atomic<void*> request{nullptr};  // the WinHTTP request under way (closed to cancel it at exit)
    std::mutex m;
    bool resultReady = false;
    bool resultCheck = false;   // the result of a check (else of a download)
    bool resultOk = false;
    upd::Release resultRel;
    std::wstring resultWhy, resultPath;
    long long resultRetry = 0;  // a rate limit: not before this (unix seconds)
    HANDLE resultLock = INVALID_HANDLE_VALUE;
} g_upd;

std::wstring UpdRunningVersion() { return g_upd.testVersion.empty() ? std::wstring(ILARTS_VERSION_W) : g_upd.testVersion; }
// Checks only in a release or test build; a development build (0.0.0) only with a test feed.
bool UpdAllowed() { return g_upd.enabled && (!g_upd.testFeed.empty() || UpdRunningVersion() != L"0.0.0"); }
long long UnixNow() { return static_cast<long long>(_time64(nullptr)); }
std::wstring UpdApiUrl() { return g_upd.testFeed.empty() ? std::wstring(L"https://api.github.com/repos/") + kUpdateRepo + L"/releases/latest" : g_upd.testFeed; }
std::wstring UpdPage() {
    return g_upd.have && !g_upd.rel.page.empty() ? Utf8W(g_upd.rel.page) : std::wstring(L"https://github.com/") + kUpdateRepo + L"/releases/latest";
}
// Installed by the installer: its uninstaller lives in Mods\tech\ILARTS\uninstall (ILARTS.iss UninstallFilesDir).
// Without it (OMM, OvGME, by hand) INSTALL would put files under the mod manager's feet.
bool UpdInstallerMode() {
    if (!g_upd.testMode.empty()) return g_upd.testMode == L"installer";
    WIN32_FIND_DATAW fd;
    const HANDLE f = FindFirstFileW((ParentDir(ExeDir()) + L"\\uninstall\\unins*.exe").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return false;
    FindClose(f);
    return true;
}
std::wstring UpdFolder() {
    wchar_t t[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, t);
    return std::wstring(t) + L"ILARTS-update";
}
bool UpdNewer(const upd::Release& r) { return !r.draft && !r.prerelease && upd::CompareVersions(r.version, Narrow(UpdRunningVersion())) > 0; }
bool UpdSkipped(const upd::Release& r) { return Utf8W(r.version) == IniGet(L"update", L"skip", L""); }
bool UpdCanInstall() { return g_upd.have && g_upd.installerMode && !g_upd.rel.url.empty() && !g_upd.rel.sha256.empty(); }

// ---- HTTP (WinHTTP, synchronous, on the worker)

struct HttpOut {
    DWORD status = 0, err = 0;  // err: a WinHTTP / Win32 error (offline, TLS, timeout ...)
    std::wstring remaining, reset, retryAfter;  // x-ratelimit-remaining, x-ratelimit-reset, retry-after
};
// https only; plain http only to the loopback address while a test feed is in use.
bool UpdUrlAllowed(const std::wstring& url) {
    if (url.rfind(L"https://", 0) == 0) return true;
    return !g_upd.testFeed.empty() && (url.rfind(L"http://127.0.0.1:", 0) == 0 || url.rfind(L"http://localhost:", 0) == 0);
}
bool HttpGet(const std::wstring& url, const wchar_t* accept, HttpOut* o, const std::function<bool(const char*, size_t)>& sink) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host, uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path, uc.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra, uc.dwExtraInfoLength = 2048;
    if (!UpdUrlAllowed(url) || !WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return o->err = ERROR_INVALID_PARAMETER, false;
    const std::wstring ua = L"ILARTS-updater/" + UpdRunningVersion();
    HINTERNET s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);  // before Windows 8.1
    if (!s) return o->err = GetLastError(), false;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", (std::wstring(path) + extra).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    if (r) {
        g_upd.request = r;
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;  // GitHub's downloads redirect to its storage (https)
        WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
        const std::wstring hdr = std::wstring(L"Accept: ") + accept + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (WinHttpSendRequest(r, hdr.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD st = 0, n = sizeof(st);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &n, WINHTTP_NO_HEADER_INDEX);
            o->status = st;
            auto header = [&](const wchar_t* name, std::wstring* out) {
                wchar_t b[128] = {};
                DWORD bn = sizeof(b);
                if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CUSTOM, name, b, &bn, WINHTTP_NO_HEADER_INDEX)) *out = b;
            };
            header(L"x-ratelimit-remaining", &o->remaining);
            header(L"x-ratelimit-reset", &o->reset);
            header(L"retry-after", &o->retryAfter);
            ok = true;
            std::vector<char> buf(64 * 1024);
            for (;;) {
                DWORD got = 0;
                if (g_upd.cancel || !WinHttpReadData(r, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
                    o->err = g_upd.cancel ? ERROR_CANCELLED : GetLastError();
                    ok = false;
                    break;
                }
                if (!got) break;
                if (!sink(buf.data(), got)) {
                    ok = false;
                    break;
                }
            }
        } else {
            o->err = GetLastError();
        }
    } else {
        o->err = GetLastError();
    }
    if (r && g_upd.request.exchange(nullptr)) WinHttpCloseHandle(r);  // else UpdateShutdown closed it
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// ---- integrity: SHA-256 (CNG) and Authenticode

std::string Sha256Hex(HANDLE f) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return out;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        LARGE_INTEGER zero{};
        SetFilePointerEx(f, zero, nullptr, FILE_BEGIN);
        std::vector<unsigned char> buf(256 * 1024);
        DWORD n = 0;
        bool ok = true;
        while ((ok = ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) != 0) && n) BCryptHashData(h, buf.data(), n, 0);
        unsigned char d[32];
        if (ok && BCryptFinishHash(h, d, 32, 0) == 0)
            for (unsigned char c : d) {
                char x[3];
                sprintf_s(x, "%02x", c);
                out += x;
            }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

// 1: a valid Authenticode signature (the signer's name in *who), 0: not signed, -1: a signature that does not verify
// (tampered, untrusted root, expired without a timestamp ...). Revocation is not checked (offline, no delay).
int Authenticode(const std::wstring& path, std::wstring* who, LONG* code) {
    WINTRUST_FILE_INFO fi{sizeof(fi)};
    fi.pcwszFilePath = path.c_str();
    WINTRUST_DATA wd{sizeof(wd)};
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG r = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    if (r == ERROR_SUCCESS && who)
        if (CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData))
            if (CRYPT_PROVIDER_SGNR* sg = WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0))
                if (sg->csCertChain && sg->pasCertChain && sg->pasCertChain[0].pCert) {
                    wchar_t n[256] = {};
                    CertGetNameStringW(sg->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, n, 256);
                    *who = n;
                }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    if (code) *code = r;
    if (r == ERROR_SUCCESS) return 1;
    if (r == TRUST_E_NOSIGNATURE || r == TRUST_E_SUBJECT_FORM_UNKNOWN || r == TRUST_E_PROVIDER_UNKNOWN) return 0;
    return -1;
}

// The installer, downloaded and renamed: holds it open against writers, checks its SHA-256 against the release's
// and its signature. A signed ILARTS.exe (SignPath, docs/RELEASING.md) accepts only an installer signed by the same
// name; an unsigned one accepts an unsigned installer or a valid signature, never an invalid one.
bool UpdVerify(const std::wstring& path, const upd::Release& rel, HANDLE* lock, std::wstring* why) {
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return *why = L"the download could not be opened", false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    const std::string sha = Sha256Hex(f);
    std::wstring who, mine;
    LONG code = 0;
    const int sig = Authenticode(path, &who, &code);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!g_upd.testSigner.empty()) mine = g_upd.testSigner;
    else if (Authenticode(exe, &mine, nullptr) != 1) mine.clear();
    wchar_t c[16];
    swprintf_s(c, L"0x%08lX", static_cast<unsigned long>(code));
    ConsoleLog(L"update: verify " + path + L": " + std::to_wstring(size.QuadPart) + L" bytes, sha256 " + Utf8W(sha) + L", signature " +
               (sig == 1 ? L"valid (" + who + L")" : sig == 0 ? std::wstring(L"none") : L"INVALID " + std::wstring(c)) +
               (mine.empty() ? L"" : L"; ILARTS signed by " + mine));
    if (rel.size >= 0 && size.QuadPart != rel.size) *why = L"the download has the wrong size";
    else if (sha.empty() || sha != rel.sha256) *why = L"the download's SHA-256 does not match the release";
    else if (sig < 0) *why = std::wstring(L"the installer's signature is not valid (") + c + L")";
    else if (!mine.empty() && (sig != 1 || who != mine)) *why = L"the installer is not signed by " + mine;
    if (!why->empty()) {
        CloseHandle(f);
        return false;
    }
    *lock = f;
    return true;
}

// ---- the worker's two jobs

void UpdFinish(bool check, bool ok, const upd::Release& rel, const std::wstring& why, long long retry, const std::wstring& path, HANDLE lock) {
    std::lock_guard<std::mutex> l(g_upd.m);
    g_upd.resultReady = true;
    g_upd.resultCheck = check;
    g_upd.resultOk = ok;
    g_upd.resultRel = rel;
    g_upd.resultWhy = why;
    g_upd.resultRetry = retry;
    g_upd.resultPath = path;
    g_upd.resultLock = lock;
}

void UpdCheckJob() {
    const std::wstring url = UpdApiUrl();
    std::string body;
    HttpOut o;
    bool got = false;
    if (url.rfind(L"http", 0) == 0) {
        got = HttpGet(url, L"application/vnd.github+json", &o, [&](const char* p, size_t n) {
            body.append(p, n);
            return body.size() < (2u << 20);  // a release is a few kB
        });
    } else {  // a test feed from a file
        body = ReadFileBytes(url);
        got = !body.empty();
        o.status = got ? 200 : 0;
        o.err = got ? 0 : ERROR_FILE_NOT_FOUND;
    }
    upd::Release rel;
    std::string why8;
    long long retry = 0;
    if (!got || o.status == 0) {
        UpdFinish(true, false, rel, L"no connection (error " + std::to_wstring(o.err) + L")", 0, L"", INVALID_HANDLE_VALUE);
        return;
    }
    if (o.status == 403 || o.status == 429) {  // the rate limit (60 an hour per address, unauthenticated)
        if (!o.reset.empty()) retry = _wtoi64(o.reset.c_str());
        if (!o.retryAfter.empty()) retry = std::max(retry, UnixNow() + _wtoi64(o.retryAfter.c_str()));
        UpdFinish(true, false, rel, L"GitHub answered " + std::to_wstring(o.status) + (o.remaining == L"0" ? L" (rate limit)" : L""), retry, L"", INVALID_HANDLE_VALUE);
        return;
    }
    if (o.status != 200) {  // 404: no release yet
        UpdFinish(true, false, rel, L"GitHub answered " + std::to_wstring(o.status), 0, L"", INVALID_HANDLE_VALUE);
        return;
    }
    if (!upd::ReadRelease(body, &rel, &why8)) {
        UpdFinish(true, false, rel, Utf8W(why8), 0, L"", INVALID_HANDLE_VALUE);
        return;
    }
    UpdFinish(true, true, rel, L"", 0, L"", INVALID_HANDLE_VALUE);
}

void UpdDownloadJob(upd::Release rel) {
    const std::wstring dir = UpdFolder(), name = Utf8W(rel.asset), path = dir + L"\\" + name, part = path + L".part";
    CreateDirectoryW(dir.c_str(), nullptr);
    DeleteFileW(part.c_str());
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return UpdFinish(false, false, rel, L"could not write to " + dir, 0, L"", INVALID_HANDLE_VALUE);
    g_upd.got = 0;
    g_upd.total = rel.size;
    HttpOut o;
    bool wrote = true;
    const bool ok = HttpGet(Utf8W(rel.url), L"application/octet-stream", &o, [&](const char* p, size_t n) {
        DWORD put = 0;
        if (o.status != 200) return false;
        wrote = WriteFile(f, p, static_cast<DWORD>(n), &put, nullptr) && put == n;
        g_upd.got += static_cast<long long>(n);
        return wrote && (rel.size < 0 || g_upd.got <= rel.size);  // never more than the release says
    });
    CloseHandle(f);
    std::wstring why;
    if (g_upd.cancel) why = L"cancelled";
    else if (!ok && o.status == 0) why = L"no connection (error " + std::to_wstring(o.err) + L")";
    else if (o.status != 200) why = L"GitHub answered " + std::to_wstring(o.status);
    else if (!wrote) why = L"could not write to " + dir;
    else if (!ok && rel.size >= 0 && g_upd.got > rel.size) why = L"the download is larger than the release says";
    else if (!ok) why = L"the download broke off (error " + std::to_wstring(o.err) + L")";
    HANDLE lock = INVALID_HANDLE_VALUE;
    if (why.empty()) {
        DeleteFileW(path.c_str());
        if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) why = L"could not write to " + dir;
        else UpdVerify(path, rel, &lock, &why);
        if (!why.empty()) DeleteFileW(path.c_str());
    }
    DeleteFileW(part.c_str());
    UpdFinish(false, why.empty(), rel, why, 0, path, lock);
}

void UpdStart(bool check) {
    if (g_upd.busy) return;
    if (g_upd.worker.joinable()) g_upd.worker.join();
    g_upd.busy = true;
    g_upd.cancel = false;
    const upd::Release rel = g_upd.rel;
    g_upd.worker = std::thread([check, rel] {
        if (check) UpdCheckJob();
        else UpdDownloadJob(rel);
        g_upd.busy = false;
    });
}

// ---- the main thread

// [update] in the ini: the newest release seen (so an update shows at once after a restart) and the schedule.
void UpdSaveRelease() {
    const wchar_t* ini = app.iniPath.c_str();
    WritePrivateProfileStringW(L"update", L"latest", Utf8W(g_upd.rel.version).c_str(), ini);
    WritePrivateProfileStringW(L"update", L"page", Utf8W(g_upd.rel.page).c_str(), ini);
    WritePrivateProfileStringW(L"update", L"asset", Utf8W(g_upd.rel.asset).c_str(), ini);
    WritePrivateProfileStringW(L"update", L"url", Utf8W(g_upd.rel.url).c_str(), ini);
    WritePrivateProfileStringW(L"update", L"sha256", Utf8W(g_upd.rel.sha256).c_str(), ini);
    WritePrivateProfileStringW(L"update", L"size", std::to_wstring(g_upd.rel.size).c_str(), ini);
}
void UpdLoadRelease() {
    g_upd.rel = upd::Release();
    g_upd.rel.version = Narrow(IniGet(L"update", L"latest", L""));
    std::vector<unsigned> parts;
    g_upd.have = upd::ParseVersion(g_upd.rel.version, &parts);
    if (!g_upd.have) return;
    g_upd.rel.page = Narrow(IniGet(L"update", L"page", L""));
    g_upd.rel.asset = Narrow(IniGet(L"update", L"asset", L""));
    g_upd.rel.url = Narrow(IniGet(L"update", L"url", L""));
    g_upd.rel.sha256 = Narrow(IniGet(L"update", L"sha256", L""));
    g_upd.rel.size = _wtoi64(IniGet(L"update", L"size", L"-1").c_str());
    if (g_upd.rel.asset != upd::InstallerName(g_upd.rel.version)) g_upd.rel.asset.clear(), g_upd.rel.url.clear();
}
void UpdSetSchedule(long long next) {
    g_upd.nextCheck = next;
    WritePrivateProfileStringW(L"update", L"next_check", std::to_wstring(next).c_str(), app.iniPath.c_str());
    WritePrivateProfileStringW(L"update", L"fails", std::to_wstring(g_upd.fails).c_str(), app.iniPath.c_str());
}
// What the indicator shows, from what is known.
void UpdRefresh() {
    if (g_upd.phase == UpdateCtl::Downloading) return;
    const bool show = UpdAllowed() && g_upd.have && UpdNewer(g_upd.rel) && !UpdSkipped(g_upd.rel);
    if (!show) g_upd.phase = UpdateCtl::Idle;
    else if (g_upd.phase == UpdateCtl::Idle) g_upd.phase = UpdateCtl::Available;
}

// The setting (SETTINGS > Installation > Check for updates), at start and after the settings are saved.
void UpdateApplySettings() {
    g_upd.enabled = _wtoi(IniGet(L"update", L"check", L"1").c_str()) != 0;
    UpdRefresh();
}

void UpdateInit() {
    {  // the last update's installer, if it is no longer running
        WIN32_FIND_DATAW fd;
        const std::wstring dir = UpdFolder();
        const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
            while (FindNextFileW(h, &fd));
            FindClose(h);
            RemoveDirectoryW(dir.c_str());
        }
    }
    g_upd.installerMode = UpdInstallerMode();
    UpdLoadRelease();
    g_upd.nextCheck = _wtoi64(IniGet(L"update", L"next_check", L"0").c_str());
    g_upd.fails = _wtoi(IniGet(L"update", L"fails", L"0").c_str());
    if (!g_upd.testFeed.empty()) g_upd.nextCheck = 0;  // a test feed: checked at start
    if (g_upd.nextCheck > UnixNow() + 86400 * 2) g_upd.nextCheck = 0;  // the clock went back
    g_upd.notBefore = GetTickCount64() + (g_upd.testFeed.empty() ? 8000 : 1000);  // not in the way of the start
    UpdateApplySettings();
    if (g_upd.phase == UpdateCtl::Available) ConsoleLog(L"update: " + Utf8W(g_upd.rel.version) + L" available (known from the last check)");
}

void UpdateTick() {
    bool ready = false;
    {
        std::lock_guard<std::mutex> l(g_upd.m);
        ready = g_upd.resultReady;
    }
    if (ready) {
        std::lock_guard<std::mutex> l(g_upd.m);
        g_upd.resultReady = false;
        const std::wstring running = UpdRunningVersion();
        if (g_upd.resultCheck && g_upd.resultOk) {
            g_upd.rel = g_upd.resultRel;
            g_upd.have = true;
            g_upd.fails = 0;
            UpdSaveRelease();
            UpdSetSchedule(UnixNow() + 86400);
            const bool newer = UpdNewer(g_upd.rel);
            ConsoleLog(L"update: latest " + Utf8W(g_upd.rel.version) + L", running " + running + L": " +
                       (!newer ? L"no update" : UpdSkipped(g_upd.rel) ? L"newer, skipped" : L"newer") + L"; installer " +
                       (g_upd.rel.url.empty() ? L"none" : Utf8W(g_upd.rel.asset)) + L", sha256 " + (g_upd.rel.sha256.empty() ? L"none" : L"given") +
                       L"; install mode " + (g_upd.installerMode ? L"installer" : L"release page"));
            if (g_upd.phase == UpdateCtl::Failed) g_upd.phase = UpdateCtl::Available;
            UpdRefresh();
        } else if (g_upd.resultCheck) {
            ++g_upd.fails;
            long long next = UnixNow() + upd::BackoffSeconds(g_upd.fails);
            if (g_upd.resultRetry > 0) next = std::max(next, g_upd.resultRetry + 60);
            UpdSetSchedule(next);
            ConsoleLog(L"update: check failed: " + g_upd.resultWhy + L"; next check in " + std::to_wstring(next - UnixNow()) + L" s");
        } else if (g_upd.resultOk) {  // the installer is here and verified: close, then start it (wWinMain's end)
            g_upd.launch = g_upd.resultPath;
            g_upd.lock = g_upd.resultLock;
            ConsoleLog(L"update: " + Utf8W(g_upd.rel.version) + L" verified; ILARTS closes and starts the installer");
            PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
        } else {
            const bool cancelled = g_upd.resultWhy == L"cancelled";
            g_upd.phase = cancelled ? UpdateCtl::Available : UpdateCtl::Failed;
            g_upd.error = cancelled ? L"" : g_upd.resultWhy;
            ConsoleLog(cancelled ? std::wstring(L"update: download cancelled") : L"update: install refused: " + g_upd.resultWhy);
        }
    }
    const ULONGLONG t = GetTickCount64();
    if (g_upd.phase != UpdateCtl::Idle && (t - g_upd.dcsAt > 2000 || !g_upd.dcsAt)) {
        g_upd.dcsAt = t;
        g_upd.dcs = g_upd.testDcs >= 0 ? g_upd.testDcs != 0 : DcsRunning();
    }
    if (!UpdAllowed() || g_upd.busy || t < g_upd.notBefore || UnixNow() < g_upd.nextCheck || !g_upd.launch.empty()) return;
    g_upd.nextCheck = UnixNow() + 600;  // until the answer (a check under way is not started again)
    ConsoleLog(L"update: checking " + UpdApiUrl());
    UpdStart(true);
}

// The title bar's indicator: shown only with an update (or its download / failure).
void UpdateStateJson(Json& s) {
    if (g_upd.phase == UpdateCtl::Idle || !UpdAllowed()) {
        s.null(L"update");
        return;
    }
    const std::wstring v = Utf8W(g_upd.rel.version);
    const bool can = UpdCanInstall();
    const bool down = g_upd.phase == UpdateCtl::Downloading, failed = g_upd.phase == UpdateCtl::Failed;
    std::wstring label = v + L" AVAILABLE", tip;  // the page puts a download icon before it
    if (down) {
        const long long got = g_upd.got, total = g_upd.total;
        if (g_upd.launch.empty() && total > 0) label = v + L" \x00B7 " + std::to_wstring(std::min<long long>(99, got * 100 / total)) + L"%";
        else label = v + L" \x00B7 \x2026";
        tip = L"Downloading DCS ILARTS " + v;
    } else if (failed) {
        label = L"UPDATE FAILED";
        tip = L"DCS ILARTS " + v + L" could not be installed: " + g_upd.error;
    } else {
        tip = L"DCS ILARTS " + v + L" is available (this is " + UpdRunningVersion() + L")";
        if (!can)
            tip += g_upd.installerMode ? L"\nThe release has no verifiable installer: download it from the release page"
                                       : L"\nInstalled with a mod manager or by hand: download it from the release page";
    }
    s.open(L"update");
    s.str(L"version", v);
    s.str(L"label", label);
    s.str(L"tone", failed ? L"amber" : L"green");
    s.str(L"tip", tip);
    s.open(L"menu", L'[');
    auto item = [&](const wchar_t* id, const std::wstring& text, bool on) {
        s.open(nullptr);
        s.str(L"id", id);
        s.str(L"label", text);
        s.flag(L"on", on);
        s.close();
    };
    if (down) {
        item(L"cancel", L"CANCEL", g_upd.launch.empty());
    } else {
        if (can) item(L"install", g_upd.dcs ? std::wstring(L"CLOSE DCS TO INSTALL") : failed ? L"TRY AGAIN" : L"INSTALL", !g_upd.dcs);
        item(L"notes", can ? L"RELEASE NOTES" : L"RELEASE PAGE", true);
        item(L"skip", L"SKIP THIS VERSION", true);
    }
    s.close(L']');
    s.close();
}

// The indicator's menu: {cmd:'update', what}.
void UpdateCommand(const std::wstring& what) {
    if (what == L"notes") {
        ConsoleLog(L"update: release page " + UpdPage());
        if (g_upd.testFeed.empty() && !g_test.offscreen) ShellExecuteW(nullptr, L"open", UpdPage().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (what == L"skip") {
        WritePrivateProfileStringW(L"update", L"skip", Utf8W(g_upd.rel.version).c_str(), app.iniPath.c_str());
        ConsoleLog(L"update: " + Utf8W(g_upd.rel.version) + L" skipped");
        g_upd.phase = UpdateCtl::Idle;
        UpdRefresh();
    } else if (what == L"cancel") {
        g_upd.cancel = true;
    } else if (what == L"install") {
        g_upd.dcs = g_upd.testDcs >= 0 ? g_upd.testDcs != 0 : DcsRunning();
        g_upd.dcsAt = GetTickCount64();
        if (g_upd.phase == UpdateCtl::Downloading || g_upd.busy || !UpdCanInstall() || g_upd.dcs) {
            ConsoleLog(L"update: install not started (" + std::wstring(g_upd.dcs ? L"DCS is running" : !UpdCanInstall() ? L"no installer to install" : L"busy") + L")");
            return;
        }
        g_upd.phase = UpdateCtl::Downloading;
        g_upd.error.clear();
        g_upd.got = 0;
        g_upd.total = g_upd.rel.size;
        ConsoleLog(L"update: downloading " + Utf8W(g_upd.rel.url));
        UpdStart(false);
    }
}

// Closing: a check or download under way is abandoned.
void UpdateShutdown() {
    g_upd.cancel = true;
    if (void* r = g_upd.request.exchange(nullptr)) WinHttpCloseHandle(r);  // a blocked read returns
    if (g_upd.worker.joinable()) g_upd.worker.join();
    std::lock_guard<std::mutex> l(g_upd.m);
    if (g_upd.resultLock != INVALID_HANDLE_VALUE && g_upd.resultLock != g_upd.lock) CloseHandle(g_upd.resultLock);
    g_upd.resultLock = INVALID_HANDLE_VALUE;
}

// After everything has closed (recordings finished, files let go): the verified installer. It replaces the files
// with the user's earlier choices (/SILENT: a progress window, the DCS-running question still asked) and starts
// ILARTS again (/ILARTSUPDATE=1, ILARTS.iss).
void UpdateLaunch() {
    if (g_upd.launch.empty()) return;
    if (!g_upd.testFeed.empty()) {  // tests never start what they downloaded
        ConsoleLog(L"update: would start \"" + g_upd.launch + L"\" " + kUpdateParams);
    } else {
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"open";
        sei.lpFile = g_upd.launch.c_str();
        sei.lpParameters = kUpdateParams;
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) {
            if (sei.hProcess) CloseHandle(sei.hProcess);
        } else {
            MessageBoxW(nullptr, (L"The installer could not be started:\n" + g_upd.launch).c_str(), L"DCS ILARTS", MB_OK | MB_ICONWARNING);
        }
    }
    if (g_upd.lock != INVALID_HANDLE_VALUE) CloseHandle(g_upd.lock);
    g_upd.lock = INVALID_HANDLE_VALUE;
}

// --test-update-selftest <log>: the parts that need no network. Exit code = failures.
int UpdateSelfTest(const std::wstring& logPath) {
    int fails = 0;
    std::wstring log;
    auto check = [&](bool ok, const std::wstring& what) {
        log += (ok ? L"ok   " : L"FAIL ") + what + L"\n";
        fails += !ok;
    };
    using upd::CompareVersions;
    {  // ordering: test builds come before their release
        const char* order[] = {"0.0.0", "1.0.9", "1.0.28", "1.0.29.1", "1.0.29.2", "1.0.29.10", "1.0.29", "1.0.30.1", "1.0.30", "1.1.0.1", "1.1.0", "1.1.1", "1.10.0", "2.0.0"};
        bool ok = true;
        for (size_t i = 0; i < std::size(order); ++i)
            for (size_t j = 0; j < std::size(order); ++j) {
                const int c = CompareVersions(order[i], order[j]), want = i < j ? -1 : i > j ? 1 : 0;
                if (c != want) {
                    ok = false;
                    log += L"     " + Utf8W(order[i]) + L" vs " + Utf8W(order[j]) + L": " + std::to_wstring(c) + L", want " + std::to_wstring(want) + L"\n";
                }
            }
        check(ok, L"order 0.0.0 < 1.0.9 < 1.0.28 < 1.0.29.1 < 1.0.29.2 < 1.0.29.10 < 1.0.29 < 1.0.30.1 < 1.0.30 < 1.1.0.1 < 1.1.0 < 1.1.1 < 1.10.0 < 2.0.0, every pair");
    }
    check(CompareVersions("1.1", "1.1.0") == 0 && CompareVersions("1.1.0.0", "1.1.0") == 0 && CompareVersions("v1.1.0", "1.1.0") == 0,
          L"1.1 = 1.1.0 = 1.1.0.0 = v1.1.0");
    check(CompareVersions("1.0.29.2.1", "1.0.29.2") > 0 && CompareVersions("1.0.29.2.1", "1.0.29") < 0, L"five parts: after its four-part build, before the release");
    {
        std::vector<unsigned> v;
        check(!upd::ParseVersion("", &v) && !upd::ParseVersion("v", &v) && !upd::ParseVersion("1..2", &v) && !upd::ParseVersion("1.2.", &v) &&
                  !upd::ParseVersion("1.2-beta", &v) && !upd::ParseVersion("99999999999.0", &v) && !upd::ParseVersion("latest", &v),
              L"not versions: empty, v, 1..2, 1.2., 1.2-beta, 99999999999.0, latest");
        check(upd::ParseVersion("v1.0.29.2", &v) && v.size() == 4 && v[3] == 2, L"v1.0.29.2 parses");
    }
    {  // JSON
        upd::JVal j;
        check(upd::ParseJson("{\"a\":[1,2.5,-3e2,true,false,null,{\"b\":\"x\\\"y\\u00e9\\ud83d\\ude00\"}],\"c\":{}}", &j) && j.get("a") &&
                  j.get("a")->items.size() == 7 && j.get("a")->items[2].n == -300 && j.get("a")->items[6].str("b") == "x\"y\xC3\xA9\xF0\x9F\x98\x80",
              L"JSON: arrays, numbers, literals, escapes, surrogate pairs");
        check(!upd::ParseJson("{\"a\":1", &j) && !upd::ParseJson("{\"a\":1}x", &j) && !upd::ParseJson("{a:1}", &j) && !upd::ParseJson("[1,]", &j) &&
                  !upd::ParseJson(std::string(200, '[') + std::string(200, ']'), &j),
              L"JSON: unterminated, trailing text, bare keys, trailing comma, nesting deeper than 64 refused");
    }
    {  // releases
        const std::string ok =
            "{\"tag_name\":\"v1.1.0\",\"html_url\":\"https://github.com/Nubs-dev/DCS-ILARTS/releases/tag/v1.1.0\",\"draft\":false,\"prerelease\":false,"
            "\"assets\":[{\"name\":\"DCS-ILARTS_v1.1.0.zip\",\"browser_download_url\":\"https://x/zip\",\"size\":5,\"digest\":\"sha256:" + std::string(64, '1') + "\"},"
            "{\"name\":\"DCS-ILARTS-Setup-1.1.0.exe\",\"browser_download_url\":\"https://github.com/Nubs-dev/DCS-ILARTS/releases/download/v1.1.0/DCS-ILARTS-Setup-1.1.0.exe\","
            "\"size\":123456,\"digest\":\"sha256:" + std::string(32, 'A') + std::string(32, 'b') + "\"}]}";
        upd::Release r;
        std::string why;
        check(upd::ReadRelease(ok, &r, &why) && r.version == "1.1.0" && r.asset == "DCS-ILARTS-Setup-1.1.0.exe" && r.size == 123456 &&
                  r.sha256 == std::string(32, 'a') + std::string(32, 'b') && r.url.find("/download/v1.1.0/") != std::string::npos && !r.prerelease,
              L"release: version, the installer asset (not the zip), size, digest lower-cased");
        std::string noDigest = ok;
        noDigest.replace(noDigest.rfind("\"digest\""), std::string("\"digest\":\"sha256:").size() + 64 + 1, "\"x\":0");
        check(upd::ReadRelease(noDigest, &r, &why) && r.sha256.empty() && !r.url.empty(), L"release without a digest: no sha256 (INSTALL not offered)");
        std::string md5 = ok;
        md5.replace(md5.rfind("sha256:"), 7, "md5:xx");
        check(upd::ReadRelease(md5, &r, &why) && r.sha256.empty(), L"release with another digest kind: no sha256");
        check(upd::ReadRelease("{\"tag_name\":\"v1.1.0\",\"assets\":[{\"name\":\"DCS-ILARTS-Setup-1.0.9.exe\"}]}", &r, &why) && r.url.empty() && r.page.empty(),
              L"release whose installer is named for another version, no page: no installer, no page");
        check(!upd::ReadRelease("{\"tag_name\":\"nightly\"}", &r, &why) && !upd::ReadRelease("{\"tag_name\":\"1.1.0\"}", &r, &why) &&
                  !upd::ReadRelease("{\"message\":\"Not Found\"}", &r, &why) && !upd::ReadRelease("<html>", &r, &why),
              L"not releases: tag without v, a word, an error object, HTML");
        check(upd::ReadRelease("{\"tag_name\":\"v1.2.0\",\"html_url\":\"http://evil/\",\"prerelease\":true}", &r, &why) && r.page.empty() && r.prerelease,
              L"release page off GitHub ignored; pre-release flag read");
    }
    check(upd::BackoffSeconds(0) == 86400 && upd::BackoffSeconds(1) == 3600 && upd::BackoffSeconds(2) == 7200 && upd::BackoffSeconds(5) == 57600 &&
              upd::BackoffSeconds(6) == 86400 && upd::BackoffSeconds(40) == 86400,
          L"backoff after failures: 1 h, 2 h ... 16 h, then a day");
    {  // newer: drafts and pre-releases never; the running build's version
        const std::wstring keep = g_upd.testVersion;
        upd::Release r;
        r.version = "1.0.29";
        g_upd.testVersion = L"1.0.29.2";
        const bool a = UpdNewer(r);
        g_upd.testVersion = L"1.0.29";
        const bool b = UpdNewer(r);
        r.version = "1.1.0";
        r.prerelease = true;
        const bool c = UpdNewer(r);
        g_upd.testVersion = keep;
        check(a && !b && !c, L"1.0.29 is an update for test build 1.0.29.2, not for 1.0.29; a pre-release never");
    }
    {  // URLs
        const std::wstring keep = g_upd.testFeed;
        g_upd.testFeed.clear();
        const bool a = UpdUrlAllowed(L"https://api.github.com/x") && !UpdUrlAllowed(L"http://api.github.com/x") && !UpdUrlAllowed(L"http://127.0.0.1:8000/x") &&
                       !UpdUrlAllowed(L"file:///c:/x");
        g_upd.testFeed = L"x";
        const bool b = UpdUrlAllowed(L"http://127.0.0.1:8000/x") && !UpdUrlAllowed(L"http://example.com/x");
        g_upd.testFeed = keep;
        check(a && b, L"https only; loopback http only with a test feed");
    }
    FILE* f = nullptr;
    if (!logPath.empty() && _wfopen_s(&f, logPath.c_str(), L"w, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%s%d failure(s)\n", log.c_str(), fails);
        fclose(f);
    }
    return fails;
}
