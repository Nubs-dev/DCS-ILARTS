// Offline self-test of the Phase 5 sharing mechanism (no DCS involved).
// For each format DCS uses: create a texture with exactly the producer's flags on device A,
// publish it as a named NT handle, open it by name on an independent device B, synchronise with
// the keyed mutex (key 0), copy, read back on B and compare with what A wrote.

#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../src/bench_protocol.h"
#include "../src/share_protocol.h"

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool MakeDevice(IDXGIAdapter1* a, ID3D11Device** d, ID3D11DeviceContext** c) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    return SUCCEEDED(D3D11CreateDevice(a, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, d, nullptr, c));
}

bool TestFormat(IDXGIAdapter1* adapter, DXGI_FORMAT fmt, const char* name, uint32_t pattern) {
    ID3D11Device *devA = nullptr, *devB = nullptr;
    ID3D11DeviceContext *ctxA = nullptr, *ctxB = nullptr;
    ID3D11Texture2D *src = nullptr, *shared = nullptr, *opened = nullptr, *staging = nullptr;
    IDXGIKeyedMutex *kmA = nullptr, *kmB = nullptr;
    IDXGIResource1* res1 = nullptr;
    ID3D11Device1* devB1 = nullptr;
    HANDLE handle = nullptr;
    bool ok = false;
    const UINT W = 64, H = 32;
    wchar_t handleName[64];
    swprintf_s(handleName, L"Local\\DCS_LSO_selftest_%S_%lu", name, GetCurrentProcessId());
    do {
        if (!MakeDevice(adapter, &devA, &ctxA) || !MakeDevice(adapter, &devB, &ctxB)) {
            printf("  %s: device creation failed\n", name);
            break;
        }
        // "DCS" source texture with known contents (render target + SRV, like DCS's).
        std::vector<uint32_t> px(W * H, pattern);
        D3D11_TEXTURE2D_DESC d{W, H, 1, 1, fmt, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0};
        D3D11_SUBRESOURCE_DATA init{px.data(), W * 4, 0};
        HRESULT hr = devA->CreateTexture2D(&d, &init, &src);
        if (FAILED(hr)) { printf("  %s: source create hr=0x%08lX\n", name, hr); break; }

        // Producer's shared copy (identical flags to src/share.cpp CreateShared).
        D3D11_TEXTURE2D_DESC sd = d;
        sd.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        hr = devA->CreateTexture2D(&sd, nullptr, &shared);
        if (FAILED(hr)) { printf("  %s: shared create hr=0x%08lX  <-- format not shareable\n", name, hr); break; }
        shared->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&kmA));
        shared->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&res1));
        hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, handleName, &handle);
        if (FAILED(hr)) { printf("  %s: CreateSharedHandle hr=0x%08lX\n", name, hr); break; }

        // Producer frame: AcquireSync(0, 0) -> copy -> ReleaseSync(0)
        hr = kmA->AcquireSync(0, 0);
        if (hr != S_OK) { printf("  %s: producer AcquireSync hr=0x%08lX\n", name, hr); break; }
        ctxA->CopySubresourceRegion(shared, 0, 0, 0, 0, src, 0, nullptr);
        kmA->ReleaseSync(0);

        // Consumer: open by name on an independent device.
        devB->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&devB1));
        hr = devB1->OpenSharedResourceByName(handleName, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                             __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&opened));
        if (FAILED(hr)) { printf("  %s: OpenSharedResourceByName hr=0x%08lX\n", name, hr); break; }
        opened->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&kmB));
        D3D11_TEXTURE2D_DESC st = d;
        st.Usage = D3D11_USAGE_STAGING;
        st.BindFlags = 0;
        st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        devB->CreateTexture2D(&st, nullptr, &staging);
        hr = kmB->AcquireSync(0, 1000);
        if (hr != S_OK) { printf("  %s: consumer AcquireSync hr=0x%08lX\n", name, hr); break; }
        ctxB->CopyResource(staging, opened);
        kmB->ReleaseSync(0);

        // While B is idle, the producer must be able to acquire again with timeout 0.
        hr = kmA->AcquireSync(0, 0);
        bool reacquire = hr == S_OK;
        if (reacquire) kmA->ReleaseSync(0);

        D3D11_MAPPED_SUBRESOURCE m{};
        hr = ctxB->Map(staging, 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr)) { printf("  %s: Map hr=0x%08lX\n", name, hr); break; }
        size_t bad = 0;
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < W; ++x)
                if (static_cast<const uint32_t*>(m.pData)[y * (m.RowPitch / 4) + x] != pattern) ++bad;
        ctxB->Unmap(staging, 0);
        printf("  %-16s shared create OK, open-by-name OK, keyed mutex OK, producer re-acquire %s, pixels %s (%zu bad)\n",
               name, reacquire ? "OK" : "FAILED", bad ? "MISMATCH" : "match", bad);
        ok = bad == 0 && reacquire;
    } while (false);
    SafeRelease(staging);
    SafeRelease(kmB);
    SafeRelease(opened);
    SafeRelease(devB1);
    if (handle) CloseHandle(handle);
    SafeRelease(res1);
    SafeRelease(kmA);
    SafeRelease(shared);
    SafeRelease(src);
    SafeRelease(ctxB);
    SafeRelease(devB);
    SafeRelease(ctxA);
    SafeRelease(devA);
    return ok;
}

// --serve <seconds> [--mission NAME ...] [--mission-every S] <capture.dds> [capture.dds ...]: publishes captured
// DCS frames through the real sharing protocol (same mapping, flags, keyed mutex and naming as src/share.cpp) so the
// viewer can be tested without DCS. Target name = file name up to the first '-' (e.g. LSOFRAME-2026...dds).
// Uses the adapter with the most dedicated VRAM (the discrete GPU DCS uses).
// --passes <own>:<flags>,<flags>,... (with --serve, before the .dds files): also fakes the hook's passes (protocol v8 whose-pass
// flags): Header::ownAircraft = <own> (0 not reported, 1 in an aircraft, 2 none) and one pass per <flags> value (bits:
// 1 own, 2 section, 4 flight), pass n under way from 6 + 8 (n - 1) s for 4 s, then a trap.
struct FakePasses {
    uint32_t own = 0;
    std::vector<uint32_t> flags;
    bool on = false;
};
FakePasses g_fake;

void FakePassTick(lsoshare::Header* hdr, double t) {
    if (!g_fake.on) return;
    hdr->ownAircraft = g_fake.own;
    if (!hdr->passEpoch) hdr->passEpoch = 1;  // --mission sets its own
    uint32_t n = 0;
    const int total = static_cast<int>(g_fake.flags.size());
    for (int k = total - 1; k >= 0 && n < 4; --k) {  // newest first, up to 4
        const double t0 = 6.0 + 8.0 * k;
        if (t < t0) continue;
        auto& p = hdr->passes[n];
        memset(&p, 0, sizeof(p));
        p.id = static_cast<uint32_t>(k + 1);
        p.active = t < t0 + 4.0 ? 1 : 0;
        p.result = p.active ? 0 : 1;
        p.range = 1000;
        strcpy_s(p.type, "FA-18C_hornet");
        sprintf_s(p.board, "%d", 300 + k + 1);
        sprintf_s(p.pilot, "Enfield 1-%d", k + 1);
        strcpy_s(p.carrier, "CVN_72");
        p.caseNo = 1;
        sprintf_s(p.callsign, "Flags%u", g_fake.flags[k]);
        hdr->passFlags[n] = g_fake.flags[k];
        static int printed = -1;
        if (p.active && k > printed) {
            printed = k;
            printf("pass %d flags %u own %u\n", k + 1, g_fake.flags[k], g_fake.own);
            fflush(stdout);
        }
        ++n;
    }
    MemoryBarrier();
    hdr->passCount = n;
}

// --cams <n>: also stands in for the extra cameras (protocol v7): ISLAND and the deck camera's 5 render slots DECK ..
// DECK 5, each slot able to show n deck positions (1..16; the hook's 15 names first - its 6 hangar positions marked
// as the hook marks them (kViewHangar) - then HANGAR 7 ..., marked too). Each camera has
// a 320 x 240 picture of its own: grey, with one white bar per position number (the island: a white diagonal), so a
// screenshot shows which position a slot delivers. Like DCS it takes its time: a camera asked for (cameraMask) starts
// rendering 250 ms later (enabled, frames counted only while it renders), a slot asked for a position (requestSeq)
// reports it as current 120 ms later and its picture changes 2 frames after that. Every change is printed with the time,
// and so is each change of what a camera the viewer has asked for should follow (ptz[i].trackMode: "track 1" = nothing).
struct FakeCam {
    std::string name;
    ID3D11Texture2D* src = nullptr;
    int target = -1;            // index in Header::targets
    ULONGLONG enableAt = 0;     // asked for: renders from then
    LONG seq = 0;               // the last requestSeq acted on
    int request = -1;
    ULONGLONG viewAt = 0;       // the requested position becomes current then
    int drawn = -2;             // the position its picture shows
    int lag = 0;                // frames until the picture follows current
    int track = -1;             // the trackMode (* 64 + position) printed last
};
struct FakeCams {
    int views = 0;              // 0 = no cameras
    std::vector<FakeCam> cams;
    LONG cameraSeq = 0;
    uint32_t mask = 0;
};
FakeCams g_cams;
const char* kFakeViewNames[] = {"BOW BUBBLE", "CATAPULT 1", "CROTCH", "ELEVATOR 1", "ELEVATOR 2", "CORRAL", "WAIST BUBBLE", "ELEVATOR 3", "JUNKYARD",
                                "HANGAR FWD", "HANGAR AFT", "HANGAR EL 1", "HANGAR EL 2", "HANGAR EL 3", "HANGAR EL 4"};
constexpr int kFakeFirstHangar = 9;

// Poses (protocol v9 Header::camPose), as the hook sends them: its CAMERAS values for ISLAND and the 15 views (others:
// at the origin), overlaid with the entries of --tuning <file> (an ILARTS-cameras.lua; re-read every 0.5 s when its
// text changes, entries applied in order, a field taken out goes back to the table's value). Each change of a
// camera's pose is printed: "cam DECK 2 pose HANGAR FWD pos 59.5 14.5 4 look -60 10.5 0 fov 50".
struct FakePose {
    const char* name;
    float pos[3], look[3], fov;
};
const FakePose kFakePoses[] = {
    {"ISLAND", {-41.6f, 29.5f, 21.6f}, {-103.4f, 26.2f, -4.3f}, 48},
    {"BOW BUBBLE", {71.6f, 21.4f, 6.1f}, {-28.1f, 21.4f, 13.3f}, 70},
    {"CATAPULT 1", {97.4f, 21.4f, 21.2f}, {-1.3f, 21.4f, 5.4f}, 60},
    {"CROTCH", {99.0f, 21.4f, -21.0f}, {6.5f, 21.4f, 16.9f}, 60},
    {"ELEVATOR 1", {43.0f, 21.4f, 39.2f}, {-9.8f, 21.4f, -45.7f}, 60},
    {"ELEVATOR 2", {-5.0f, 21.4f, 39.3f}, {65.3f, 21.4f, -31.8f}, 60},
    {"CORRAL", {-2.2f, 21.4f, 38.7f}, {65.4f, 21.4f, -35.0f}, 60},
    {"WAIST BUBBLE", {-21.8f, 21.4f, -37.4f}, {48.4f, 21.4f, 33.8f}, 60},
    {"ELEVATOR 3", {-84.1f, 21.4f, 39.4f}, {12.3f, 21.4f, 12.8f}, 60},
    {"JUNKYARD", {-81.2f, 21.1f, 39.0f}, {15.1f, 21.1f, 12.1f}, 60},
    {"HANGAR FWD", {59.5f, 14.5f, 4.0f}, {-60.0f, 10.5f, 0.0f}, 50},
    {"HANGAR AFT", {-128.0f, 14.5f, -5.0f}, {-20.0f, 10.5f, 0.0f}, 50},
    {"HANGAR EL 1", {27.4f, 14.5f, -6.6f}, {27.4f, 12.0f, 18.0f}, 60},
    {"HANGAR EL 2", {-20.0f, 14.5f, -17.3f}, {-20.0f, 12.5f, 18.0f}, 60},
    {"HANGAR EL 3", {-99.0f, 14.5f, -15.3f}, {-99.0f, 12.5f, 18.0f}, 60},
    {"HANGAR EL 4", {-105.2f, 14.5f, 15.3f}, {-105.2f, 12.5f, -18.0f}, 60},
};
struct FakeTuning {
    std::wstring path;
    std::string text;
    bool read = false;
    ULONGLONG at = 0;
    std::vector<FakePose> entries;
    std::vector<std::string> names;
    std::vector<int> has;  // bit 0 pos, 1 look, 2 fov
} g_tuning;

// The file's entries: comments dropped, then every "name = '...'" up to the next one (enough for a stand-in).
void FakeTuningRead() {
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_tuning.path.c_str(), L"rb") == 0 && f) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
        fclose(f);
    }
    if (g_tuning.read && s == g_tuning.text) return;
    g_tuning.read = true;
    g_tuning.text = s;
    std::string code;
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 2, "--") == 0) {
            const size_t e = s.find('\n', i);
            i = e == std::string::npos ? s.size() : e;
        } else {
            code += s[i++];
        }
    }
    g_tuning.entries.clear(), g_tuning.names.clear(), g_tuning.has.clear();
    for (size_t at = code.find("name"); at != std::string::npos;) {
        const size_t q = code.find_first_of("'\"", at);
        if (q == std::string::npos) break;
        const size_t q2 = code.find(code[q], q + 1);
        if (q2 == std::string::npos) break;
        const size_t next = code.find("name", q2);
        const std::string t = code.substr(q2, next == std::string::npos ? std::string::npos : next - q2);
        FakePose p{};
        int has = 0;
        auto vec = [&](const char* key, float* v, int bit) {
            const size_t k = t.find(key);
            const size_t lb = k == std::string::npos ? k : t.find('{', k);
            if (lb != std::string::npos && sscanf_s(t.c_str() + lb + 1, " %f , %f , %f", &v[0], &v[1], &v[2]) == 3) has |= bit;
        };
        vec("pos", p.pos, 1);
        vec("look", p.look, 2);
        const size_t fv = t.find("fov");
        const size_t eq = fv == std::string::npos ? fv : t.find('=', fv);
        if (eq != std::string::npos && sscanf_s(t.c_str() + eq + 1, " %f", &p.fov) == 1) has |= 4;
        g_tuning.names.push_back(code.substr(q + 1, q2 - q - 1));
        g_tuning.entries.push_back(p);
        g_tuning.has.push_back(has);
        at = next;
    }
    printf("tuning file read: %zu entries\n", g_tuning.entries.size());
}

void FakePoseTick(lsoshare::Header* hdr, size_t p, const char* shown, double t) {
    FakePose pose{shown, {}, {}, 60};
    for (const auto& d : kFakePoses)
        if (!strcmp(d.name, shown)) pose = d;
    for (size_t k = 0; k < g_tuning.entries.size(); ++k) {
        if (g_tuning.names[k] != shown) continue;
        const FakePose& e = g_tuning.entries[k];
        if (g_tuning.has[k] & 1) memcpy(pose.pos, e.pos, sizeof(pose.pos));
        if (g_tuning.has[k] & 2) memcpy(pose.look, e.look, sizeof(pose.look));
        if (g_tuning.has[k] & 4) pose.fov = e.fov;
    }
    auto& d = hdr->camPose[p];
    if (d.valid && !strcmp(d.name, shown) && !memcmp(d.pos, pose.pos, sizeof(d.pos)) && !memcmp(d.look, pose.look, sizeof(d.look)) && d.fovDeg == pose.fov)
        return;
    strncpy_s(d.name, shown, _TRUNCATE);
    memcpy(d.pos, pose.pos, sizeof(d.pos));
    memcpy(d.look, pose.look, sizeof(d.look));
    d.fovDeg = pose.fov;
    MemoryBarrier();
    d.valid = 1;
    InterlockedIncrement(&d.seq);
    printf("%.3f cam %s pose %s pos %g %g %g look %g %g %g fov %g\n", t, g_cams.cams[p].name.c_str(), shown, pose.pos[0], pose.pos[1], pose.pos[2],
           pose.look[0], pose.look[1], pose.look[2], pose.fov);
}

void FakeCamPicture(ID3D11DeviceContext* ctx, FakeCam& c, int view, bool island) {
    const UINT W = 320, H = 240;
    std::vector<uint32_t> px(W * H);
    const uint32_t bg = 0x30 + static_cast<uint32_t>((view < 0 ? 0 : view) * 9 % 0x60);
    const uint32_t grey = 0xFF000000u | bg << 16 | bg << 8 | bg;
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) {
            bool white = false;
            if (island) white = abs(static_cast<int>(x * H / W) - static_cast<int>(y)) < 4;
            else for (int b = 0; b <= view; ++b) white = white || (x >= 16u + b * 18u && x < 26u + b * 18u && y > 60 && y < 180);
            px[y * W + x] = white ? 0xFFFFFFFFu : grey;
        }
    ctx->UpdateSubresource(c.src, 0, nullptr, px.data(), W * 4, 0);
}

void FakeCamTick(lsoshare::Header* hdr, ID3D11DeviceContext* ctx, ULONGLONG now, ULONGLONG begin) {
    if (!g_cams.views) return;
    const double t = (now - begin) / 1000.0;
    if (!g_tuning.path.empty() && now >= g_tuning.at) {
        g_tuning.at = now + 500;
        FakeTuningRead();
    }
    if (hdr->control.cameraSeq != g_cams.cameraSeq) {
        g_cams.cameraSeq = hdr->control.cameraSeq;
        MemoryBarrier();
        const uint32_t m = hdr->control.cameraMask;
        for (size_t p = 0; p < g_cams.cams.size(); ++p) {
            const bool was = (g_cams.mask >> p) & 1, on = (m >> p) & 1;
            if (on && !was) g_cams.cams[p].enableAt = now + 250;
            if (!on && was) {
                hdr->cameras[p].enabled = 0;
                printf("%.3f cam %s off\n", t, g_cams.cams[p].name.c_str());
            }
        }
        g_cams.mask = m;
    }
    for (size_t p = 0; p < g_cams.cams.size(); ++p) {
        FakeCam& c = g_cams.cams[p];
        auto& v = hdr->views[p];
        if (((g_cams.mask >> p) & 1) && !hdr->cameras[p].enabled && now >= c.enableAt) {
            hdr->cameras[p].enabled = 1;
            printf("%.3f cam %s on\n", t, c.name.c_str());
        }
        if (p > 0) {  // a deck slot: its position
            const LONG seq = v.requestSeq;
            if (seq != c.seq) {
                c.seq = seq;
                MemoryBarrier();
                c.request = v.request;
                c.viewAt = now + 120;
            }
            if (c.viewAt && now >= c.viewAt) {
                c.viewAt = 0;
                if (c.request >= 0 && c.request < g_cams.views && c.request != v.current) {
                    v.current = c.request;
                    c.lag = 2;
                    printf("%.3f cam %s view %d %s\n", t, c.name.c_str(), v.current, v.names[v.current]);
                }
            }
        }
        FakePoseTick(hdr, p, p == 0 ? "ISLAND" : v.current >= 0 ? v.names[v.current] : "", t);
        const int track = ((g_cams.mask >> p) & 1) ? hdr->ptz[p].trackMode : -1, at = p > 0 ? v.current : -1;
        if (track >= 0 && track * 64 + at != c.track)
            printf("%.3f cam %s track %d%s%s\n", t, c.name.c_str(), track, at >= 0 ? " at " : "", at >= 0 ? v.names[at] : "");
        c.track = track >= 0 ? track * 64 + at : -1;
        if (!hdr->cameras[p].enabled) continue;
        const int want = p > 0 ? v.current : -1;
        if (want != c.drawn && (c.lag <= 0 || --c.lag == 0)) {
            FakeCamPicture(ctx, c, want, p == 0);
            c.drawn = want;
        }
    }
    fflush(stdout);
}

// --bench <ms> (with --serve): stands in for an armed benchmark (src/bench_protocol.h). The bench block says "holding"
// (the hook alone) and the share stays invisible (magic 0) until the viewer releases it; meanwhile a thread writes
// made-up DCS frame times: <ms> each, + 0.9 ms once sharing, + 1.1 ms per camera rendering, +-8 % jitter and a hitch now
// and then. --bench-end S: the mission ends at S s (hookState Ended); --bench-who S: the player changes slot at S s.
// --bench-pause S:D: DCS is paused from S s for D s (the paused flag; the hook keeps stepping, as in DCS);
// --bench-stall S:D: the hook does not step from S s for D s (hookTick stands still); --bench-board S: the player is
// not in an aircraft until S s (briefing, slot choice).
struct FakeBench {
    double ms = 0;  // 0 = off
    double endAt = 0, whoAt = 0, boardAt = 0;
    double pauseAt = -1, pauseFor = 0, stallAt = -1, stallFor = 0;
    bool paused = false, stalled = false;
    HANDLE map = nullptr;
    lsoshare::Bench* b = nullptr;
    std::atomic<bool> released{false}, quit{false};
    std::atomic<int> cams{0};
    std::thread frames;
    LONG lastCmd = 0;
    std::string stage;
} g_bench;

void FakeBenchStart() {
    wchar_t name[96];
    lsoshare::BenchMappingName(name, _countof(name));
    g_bench.map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(lsoshare::Bench), name);
    auto* b = g_bench.b = static_cast<lsoshare::Bench*>(MapViewOfFile(g_bench.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(lsoshare::Bench)));
    const LONG session = b->magic == lsoshare::kBenchMagic ? b->session : 0;  // a viewer kept the last one: go on counting
    memset(b, 0, sizeof(*b));
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    b->qpcFrequency = f.QuadPart;
    b->producerPid = GetCurrentProcessId();
    b->version = lsoshare::kBenchVersion;
    strcpy_s(b->info, g_bench.boardAt > 0 ? "mission=Bench test\naircraft=\nrole=single player\nplayer=no slot, no aircraft\ndcs=2.9.31.29434\n"
                                          : "mission=Bench test\naircraft=FA-18C_hornet\nrole=single player\nplayer=no slot, in an aircraft\ndcs=2.9.31.29434\n");
    b->hookTick = static_cast<LONG64>(GetTickCount64());
    b->hookState = lsoshare::kBenchHolding;
    b->session = session + 1;
    MemoryBarrier();
    b->magic = lsoshare::kBenchMagic;
    printf("bench session %ld holding\n", b->session);
    g_bench.frames = std::thread([] {
        uint32_t seed = 777;
        auto rnd = [&] { return (seed = seed * 1664525u + 1013904223u) / 4294967296.0; };
        LARGE_INTEGER q0, q, f;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        double sim = 0;  // ms of frames written
        while (!g_bench.quit) {
            Sleep(2);
            QueryPerformanceCounter(&q);
            const double real = (q.QuadPart - q0.QuadPart) * 1000.0 / f.QuadPart;
            while (sim < real) {
                double ms = g_bench.ms + (g_bench.released ? 0.9 : 0) + 1.1 * g_bench.cams;
                ms *= 0.92 + 0.16 * rnd();
                if (rnd() < 0.005) ms *= 2.5;
                const LONG64 n = g_bench.b->frames;
                g_bench.b->frameMs[n % lsoshare::kBenchRing] = static_cast<float>(ms);
                InterlockedExchange64(&g_bench.b->frames, n + 1);
                sim += ms;
            }
        }
    });
}

// Once a loop: the viewer's commands, the hook's end. True once the share may be shown (released).
bool FakeBenchTick(lsoshare::Header* hdr, double t) {
    if (!g_bench.b) return true;
    auto* b = g_bench.b;
    int cams = 0;
    for (int i = 0; i < lsoshare::kMaxCameras; ++i) cams += hdr->cameras[i].enabled != 0;
    g_bench.cams = cams;
    const bool paused = g_bench.pauseAt >= 0 && t >= g_bench.pauseAt && t < g_bench.pauseAt + g_bench.pauseFor;
    if (paused != g_bench.paused) printf("%.3f bench %s\n", t, paused ? "paused" : "unpaused");
    g_bench.paused = paused;
    b->paused = paused ? 1 : 0;
    const bool stalled = g_bench.stallAt >= 0 && t >= g_bench.stallAt && t < g_bench.stallAt + g_bench.stallFor;
    if (stalled != g_bench.stalled) printf("%.3f bench hook %s\n", t, stalled ? "stalled" : "stepping again");
    g_bench.stalled = stalled;
    if (!stalled) b->hookTick = static_cast<LONG64>(GetTickCount64());
    if (g_bench.boardAt > 0 && t >= g_bench.boardAt) {
        g_bench.boardAt = 0;
        strcpy_s(b->info, "mission=Bench test\naircraft=FA-18C_hornet\nrole=single player\nplayer=no slot, in an aircraft\ndcs=2.9.31.29434\n");
        InterlockedIncrement(&b->whoSeq);
        printf("%.3f bench player in an aircraft\n", t);
    }
    const std::string stage(b->stage, strnlen(b->stage, sizeof(b->stage)));
    if (stage != g_bench.stage) {
        g_bench.stage = stage;
        if (!stage.empty()) printf("%.3f bench stage %s\n", t, stage.c_str());
    }
    const LONG cmd = b->commandSession == b->session ? b->command : 0;
    if (cmd != g_bench.lastCmd) {
        g_bench.lastCmd = cmd;
        if (cmd == lsoshare::kBenchCmdRelease && b->hookState == lsoshare::kBenchHolding) {
            g_bench.released = true;
            b->hookState = lsoshare::kBenchReleased;
            printf("%.3f bench released: sharing starts\n", t);
        } else if (cmd == lsoshare::kBenchCmdDone) {
            strcpy_s(b->endReason, "run over");
            b->hookState = lsoshare::kBenchEnded;
            printf("%.3f bench run over\n", t);
        }
    }
    if (g_bench.whoAt > 0 && t >= g_bench.whoAt) {
        g_bench.whoAt = 0;
        strcpy_s(b->info, "mission=Bench test\naircraft=\nrole=single player\nplayer=no slot, no aircraft\ndcs=2.9.31.29434\n");
        InterlockedIncrement(&b->whoSeq);
        printf("%.3f bench player changed\n", t);
    }
    if (g_bench.endAt > 0 && t >= g_bench.endAt && b->hookState != lsoshare::kBenchEnded) {
        strcpy_s(b->endReason, "mission ended");
        b->hookState = lsoshare::kBenchEnded;
        printf("%.3f bench mission ended\n", t);
    }
    fflush(stdout);
    return g_bench.released;
}

void FakeBenchStop() {
    if (!g_bench.b) return;
    g_bench.quit = true;
    if (g_bench.frames.joinable()) g_bench.frames.join();
    if (g_bench.b->hookState != lsoshare::kBenchEnded) {
        strcpy_s(g_bench.b->endReason, "mission ended");
        g_bench.b->hookState = lsoshare::kBenchEnded;
    }
    UnmapViewOfFile(g_bench.b);
    CloseHandle(g_bench.map);
}

// --mission: stands in for the hook's mission (Header::passEpoch and missionName, v8); with several, the next one
// starts every --mission-every seconds (default 10), each with a new epoch, as a mission restart does.
struct Served {
    std::string name;
    ID3D11Texture2D* src = nullptr;
    ID3D11Texture2D* shared = nullptr;
    IDXGIKeyedMutex* km = nullptr;
    HANDLE handle = nullptr;
    int cam = -1;  // --cams: the camera whose picture this is (copied only while it renders)
};

bool LoadDds(const wchar_t* path, std::vector<unsigned char>& file, UINT& w, UINT& h, DXGI_FORMAT& fmt, UINT& bpp) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
    fclose(f);
    if (file.size() < 148 || memcmp(file.data(), "DDS ", 4) != 0) return false;
    h = *reinterpret_cast<const UINT*>(&file[12]);
    w = *reinterpret_cast<const UINT*>(&file[16]);
    fmt = static_cast<DXGI_FORMAT>(*reinterpret_cast<const UINT*>(&file[128]));
    bpp = static_cast<UINT>((file.size() - 148) / (static_cast<size_t>(w) * h));
    return true;
}

void SetMission(lsoshare::Header* hdr, const std::vector<std::wstring>& missions, size_t i) {
    if (missions.empty()) return;
    const std::wstring& w = missions[i % missions.size()];
    char utf8[sizeof(hdr->missionName)] = {};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, utf8, static_cast<int>(sizeof(utf8)) - 1, nullptr, nullptr);
    memcpy(hdr->missionName, utf8, sizeof(utf8));
    MemoryBarrier();
    hdr->passEpoch = 1000 + static_cast<uint32_t>(i);
    printf("mission %u: %s\n", hdr->passEpoch, utf8);
}

int Serve(int seconds, int count, wchar_t** paths, const std::vector<std::wstring>& missions, int missionEvery) {
    const int camCount = g_cams.views ? lsoshare::kMaxCameras : 0;
    if (count < 1 || count + camCount > lsoshare::kMaxTargets) { printf("1..%d dds files\n", lsoshare::kMaxTargets - camCount); return 1; }
    IDXGIFactory1* fac = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&fac));
    IDXGIAdapter1 *a = nullptr, *best = nullptr;
    SIZE_T bestMem = 0;
    DXGI_ADAPTER_DESC1 bd{};
    for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > bestMem) {
            SafeRelease(best);
            best = a;
            bestMem = d.DedicatedVideoMemory;
            bd = d;
        } else {
            a->Release();
        }
    }
    fac->Release();
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (!best || !MakeDevice(best, &dev, &ctx)) { printf("device failed\n"); return 1; }
    best->Release();

    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(lsoshare::Header), lsoshare::kMappingName);
    auto* hdr = static_cast<lsoshare::Header*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(lsoshare::Header)));
    memset(hdr, 0, sizeof(*hdr));
    hdr->adapterLuid = bd.AdapterLuid;
    hdr->producerPid = GetCurrentProcessId();
    hdr->targetCount = static_cast<uint32_t>(count + camCount);
    hdr->version = lsoshare::kVersion;

    std::vector<Served> served(count + camCount);
    for (int i = 0; i < count + camCount; ++i) {
        std::vector<unsigned char> file;
        UINT w = 320, h = 240, bpp = 4;
        DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        Served& s = served[i];
        if (i < count) {
            if (!LoadDds(paths[i], file, w, h, fmt, bpp)) { printf("cannot load %ls\n", paths[i]); return 1; }
            std::wstring base = paths[i];
            base = base.substr(base.find_last_of(L"\\/") + 1);
            base = base.substr(0, base.find(L'-'));
            s.name.assign(base.begin(), base.end());
        } else {  // --cams: camera p's picture
            s.cam = i - count;
            s.name = "FAKECAM" + std::to_string(s.cam) + "_LDR";
            file.assign(148 + static_cast<size_t>(w) * h * 4, 0x40);
        }
        D3D11_TEXTURE2D_DESC d{w, h, 1, 1, fmt, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0};
        D3D11_SUBRESOURCE_DATA init{file.data() + 148, w * bpp, 0};
        HRESULT hr = dev->CreateTexture2D(&d, &init, &s.src);
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&d, nullptr, &s.shared);
        IDXGIResource1* r1 = nullptr;
        wchar_t handleName[96];
        swprintf_s(handleName, L"Local\\DCS_LSO_%S_%lu_1", s.name.c_str(), GetCurrentProcessId());
        if (SUCCEEDED(hr)) hr = s.shared->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&s.km));
        if (SUCCEEDED(hr)) hr = s.shared->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&r1));
        if (SUCCEEDED(hr)) hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, handleName, &s.handle);
        SafeRelease(r1);
        if (FAILED(hr)) { printf("shared setup failed for %s hr=0x%08lX\n", s.name.c_str(), hr); return 1; }
        auto& slot = hdr->targets[i];
        strncpy_s(slot.name, s.name.c_str(), _TRUNCATE);
        wcscpy_s(slot.handleName, handleName);
        slot.width = w;
        slot.height = h;
        slot.format = fmt;
        slot.active = 1;
        MemoryBarrier();
        slot.generation = 1;
        printf("serving %s %ux%u fmt=%d\n", s.name.c_str(), w, h, fmt);
        if (s.cam >= 0) {  // the camera itself
            FakeCam c;
            c.name = s.cam == 0 ? "ISLAND" : s.cam == 1 ? "DECK" : "DECK " + std::to_string(s.cam);
            c.src = s.src;
            c.target = i;
            g_cams.cams.push_back(c);
            auto& hc = hdr->cameras[s.cam];
            strncpy_s(hc.name, c.name.c_str(), _TRUNCATE);
            strncpy_s(hc.ldr, s.name.c_str(), _TRUNCATE);
            hc.available = 1;
            hc.aspect = 4.0f / 3.0f;
            auto& v = hdr->views[s.cam];
            v.current = -1;
            if (s.cam > 0) {
                for (int k = 0; k < g_cams.views; ++k) {
                    if (k < static_cast<int>(_countof(kFakeViewNames))) strcpy_s(v.names[k], kFakeViewNames[k]);
                    else sprintf_s(v.names[k], "HANGAR %d", k - kFakeFirstHangar + 1);
                    if (k >= kFakeFirstHangar) v.names[k][sizeof(v.names[k]) - 1] = lsoshare::kViewHangar;
                }
                v.current = 0;
                v.count = static_cast<uint32_t>(g_cams.views);
            }
        }
    }
    if (camCount) hdr->cameraCount = static_cast<uint32_t>(camCount);
    size_t mission = 0;
    SetMission(hdr, missions, mission);
    if (g_bench.ms > 0) FakeBenchStart();  // the share appears when the viewer releases it
    else hdr->magic = lsoshare::kMagic;
    printf("on %ls for %d s\n", bd.Description, seconds);

    const ULONGLONG begin = GetTickCount64(), end = begin + static_cast<ULONGLONG>(seconds) * 1000;
    unsigned long long copies = 0, skipped = 0;
    while (GetTickCount64() < end) {
        if (missions.size() > 1 && (GetTickCount64() - begin) / (static_cast<ULONGLONG>(missionEvery) * 1000) > mission)
            SetMission(hdr, missions, ++mission);
        if (FakeBenchTick(hdr, (GetTickCount64() - begin) / 1000.0) && hdr->magic != lsoshare::kMagic) {
            MemoryBarrier();
            hdr->magic = lsoshare::kMagic;
        }
        FakePassTick(hdr, (GetTickCount64() - begin) / 1000.0);
        FakeCamTick(hdr, ctx, GetTickCount64(), begin);
        for (int i = 0; i < count + camCount; ++i) {
            Served& s = served[i];
            if (s.cam >= 0 && !hdr->cameras[s.cam].enabled) continue;  // a camera DCS does not render: no frames
            if (s.km->AcquireSync(0, 0) == S_OK) {
                ctx->CopyResource(s.shared, s.src);
                s.km->ReleaseSync(0);
                InterlockedIncrement64(&hdr->targets[i].frameCounter);
                ++copies;
            } else {
                ++skipped;
            }
        }
        Sleep(16);
    }
    printf("served copies=%llu skipped=%llu\n", copies, skipped);
    FakeBenchStop();
    hdr->magic = 0;
    UnmapViewOfFile(hdr);
    CloseHandle(map);
    for (auto& s : served) {
        CloseHandle(s.handle);
        SafeRelease(s.km);
        SafeRelease(s.shared);
        SafeRelease(s.src);
    }
    ctx->Release();
    dev->Release();
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && wcscmp(argv[1], L"--serve") == 0) {
        std::vector<std::wstring> missions;
        int every = 10, i = 3;
        for (; i + 1 < argc; i += 2) {
            if (wcscmp(argv[i], L"--mission") == 0) missions.push_back(argv[i + 1]);
            else if (wcscmp(argv[i], L"--mission-every") == 0) every = std::max(1, _wtoi(argv[i + 1]));
            else if (wcscmp(argv[i], L"--cams") == 0) g_cams.views = std::clamp(_wtoi(argv[i + 1]), 1, lsoshare::kMaxViews);
            else if (wcscmp(argv[i], L"--tuning") == 0) g_tuning.path = argv[i + 1];  // with --cams: an ILARTS-cameras.lua
            else if (wcscmp(argv[i], L"--bench") == 0) g_bench.ms = std::max(1.0, _wtof(argv[i + 1]));
            else if (wcscmp(argv[i], L"--bench-end") == 0) g_bench.endAt = _wtof(argv[i + 1]);
            else if (wcscmp(argv[i], L"--bench-who") == 0) g_bench.whoAt = _wtof(argv[i + 1]);
            else if (wcscmp(argv[i], L"--bench-board") == 0) g_bench.boardAt = _wtof(argv[i + 1]);
            else if (wcscmp(argv[i], L"--bench-pause") == 0 || wcscmp(argv[i], L"--bench-stall") == 0) {  // S:D
                const bool pause = wcscmp(argv[i], L"--bench-pause") == 0;
                const wchar_t* c = wcschr(argv[i + 1], L':');
                (pause ? g_bench.pauseAt : g_bench.stallAt) = _wtof(argv[i + 1]);
                (pause ? g_bench.pauseFor : g_bench.stallFor) = c ? _wtof(c + 1) : 5;
            }
            else if (wcscmp(argv[i], L"--passes") == 0) {
                g_fake.on = true;
                g_fake.own = static_cast<uint32_t>(_wtoi(argv[i + 1]));
                const wchar_t* c = wcschr(argv[i + 1], L':');
                while (c && *c) {
                    g_fake.flags.push_back(static_cast<uint32_t>(_wtoi(c + 1)));
                    c = wcschr(c + 1, L',');
                }
            } else break;
        }
        return Serve(_wtoi(argv[2]), argc - i, argv + i, missions, every);
    }
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return 1;
    IDXGIAdapter1* a = nullptr;
    int failures = 0;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            a->Release();
            continue;
        }
        printf("adapter %u: %ls (LUID %08lX:%08lX)\n", i, d.Description, d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
        // DCS formats from the capture: LSOFRAME R11G11B10_FLOAT, LSOFINALTARGET/LSOSCREEN2 R8G8B8A8_UNORM.
        failures += !TestFormat(a, DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 0x80FF4020u);
        failures += !TestFormat(a, DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10_FLOAT", 0x3C0F01E0u);
        a->Release();
    }
    f->Release();
    printf(failures ? "SELFTEST FAILED (%d)\n" : "SELFTEST PASSED\n", failures);
    return failures ? 1 : 0;
}
