#include "hullscan.h"

#include "common.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace lsodiag {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxCandidates = 600;
constexpr size_t kMaxMatches = 50000;
constexpr int kMaxJumpTimes = 12;
constexpr double kScanDeg = 8.0;        // orientation tolerance for the scan (forward and up axis)
constexpr double kScanMetres = 100.0;   // position tolerance for the scan
constexpr int kWatchPasses = 100;       // after the scan: watch every match 100 x 100 ms
// Keep matches that changed this many times while watched. (1..15 was "rare updaters only"; the render
// transforms turned out to change ~50 times in 10 s, so every changing copy is kept now.)
constexpr int kRareMin = 1, kRareMax = 1000000;

struct Layout {
    const char* name;
    int stride;  // element step between the components of one axis
    int yStart;  // element offset of the up axis' first component
};
// 4x4 / 3x3 matrices stored by rows (axes are rows) or by columns (axes are columns)
const Layout kLayouts[] = {{"row4", 1, 4}, {"row3", 1, 3}, {"col4", 4, 1}, {"col3", 3, 1}};

struct Cand {
    unsigned char* addr = nullptr;
    bool isDouble = false;
    int layout = 0;      // matrix layout 0..3; quaternion 4 + (order xyzw ? 1 : 0) + (transposed ? 2 : 0);
                         // position 8 (x y z consecutive) / 9 (x y z at stride 4: column-major translation)
    std::string where;    // region / module
    std::string context;  // 16 elements from the start, at scan time
    // statistics since the last report
    long n = 0, bad = 0, changed = 0, bigJumps = 0, smallJumps = 0;
    double lagMin = 0, lagMax = 0, lagSum = 0, lastLag = 0;   // heading lag (deg) or sideways offset (m)
    double alongMin = 0, alongMax = 0;                           // positions: offset along the ship (m)
    bool haveLast = false;
    double jumpT[kMaxJumpTimes] = {};
    double jumpD[kMaxJumpTimes] = {};
    unsigned char lastRaw[96] = {};
    long totalN = 0, totalChanged = 0;
};

std::mutex g_mu;  // guards g_ship*, g_cands
double g_ship[12] = {};   // axes x, y, z, then position (when the hook sends it)
bool g_haveShip = false, g_havePos = false;
std::vector<Cand> g_cands;
std::atomic<bool> g_running{false};
std::string g_label;
LARGE_INTEGER g_freq{}, g_t0{};

double Now() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    return double(q.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
}

double YawDeg(double xx, double xz) { return std::atan2(xz, xx) * 180.0 / kPi; }
double Wrap(double d) {
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}

// Quaternion (w, x, y, z) of the rotation matrix m (row-major 3x3) and back; inverses of each other.
void QuatFromMatrix(const double* m, double* q) {
    const double tr = m[0] + m[4] + m[8];
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        q[0] = 0.25 * s; q[1] = (m[7] - m[5]) / s; q[2] = (m[2] - m[6]) / s; q[3] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const double s = std::sqrt(1.0 + m[0] - m[4] - m[8]) * 2;
        q[0] = (m[7] - m[5]) / s; q[1] = 0.25 * s; q[2] = (m[1] + m[3]) / s; q[3] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const double s = std::sqrt(1.0 + m[4] - m[0] - m[8]) * 2;
        q[0] = (m[2] - m[6]) / s; q[1] = (m[1] + m[3]) / s; q[2] = 0.25 * s; q[3] = (m[5] + m[7]) / s;
    } else {
        const double s = std::sqrt(1.0 + m[8] - m[0] - m[4]) * 2;
        q[0] = (m[3] - m[1]) / s; q[1] = (m[2] + m[6]) / s; q[2] = (m[5] + m[7]) / s; q[3] = 0.25 * s;
    }
}
void MatrixFromQuat(const double* q, double* m) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w); m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w); m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w); m[7] = 2 * (y * z + x * w); m[8] = 1 - 2 * (x * x + y * y);
}
// Targets: quaternion of the ship matrix (rows = axes) and of its transpose, in (w,x,y,z).
void ShipQuats(const double* ship, double qt[2][4]) {
    double mt[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) mt[r * 3 + c] = ship[c * 3 + r];
    QuatFromMatrix(ship, qt[0]);
    QuatFromMatrix(mt, qt[1]);
}
// Ship forward axis from a stored quaternion of kind k (0..3).
template <typename F>
bool QuatForward(F el, int k, double* fwd) {
    double q[4];
    if (k & 1) { q[0] = el(3); q[1] = el(0); q[2] = el(1); q[3] = el(2); }
    else { for (int i = 0; i < 4; ++i) q[i] = el(i); }
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 0.98 && n < 1.02)) return false;
    double m[9];
    MatrixFromQuat(q, m);
    if (k & 2) { fwd[0] = m[0]; fwd[1] = m[3]; fwd[2] = m[6]; }   // transposed: forward = column 0
    else { fwd[0] = m[0]; fwd[1] = m[1]; fwd[2] = m[2]; }          // forward = row 0
    return true;
}
// Does p hold a quaternion matching the ship (within ~1 deg)? Returns kind 0..3 or -1.
template <typename T>
int QuatMatch(const T* p, size_t avail, const double qt[2][4]) {
    if (avail < 4) return -1;
    double v[4];
    for (int i = 0; i < 4; ++i) v[i] = double(p[i]);
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
    if (!(n > 0.98 && n < 1.02)) return -1;
    for (int k = 0; k < 4; ++k) {
        const double* t = qt[k >> 1];
        double d;
        if (k & 1) d = v[3] * t[0] + v[0] * t[1] + v[1] * t[2] + v[2] * t[3];
        else d = v[0] * t[0] + v[1] * t[1] + v[2] * t[2] + v[3] * t[3];
        if (std::fabs(d) / n > std::cos(kScanDeg * 0.5 * kPi / 180)) return k;
    }
    return -1;
}

int SpanElements(const Layout& l) { return std::max(l.yStart + 2 * l.stride + 1, 2 * l.stride + 1) + 0; }

// Do the elements at p (count avail) hold the ship's forward and up axes in layout l?
template <typename T>
bool Matches(const T* p, size_t avail, const Layout& l, const double* ship) {
    const size_t need = size_t(SpanElements(l));
    if (avail < need) return false;
    double x[3], y[3];
    for (int i = 0; i < 3; ++i) {
        x[i] = double(p[i * l.stride]);
        y[i] = double(p[l.yStart + i * l.stride]);
    }
    const double nx = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    const double ny = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (!(nx > 0.9 && nx < 1.1 && ny > 0.9 && ny < 1.1)) return false;
    const double dx = (x[0] * ship[0] + x[1] * ship[1] + x[2] * ship[2]) / nx;
    const double dy = (y[0] * ship[3] + y[1] * ship[4] + y[2] * ship[5]) / ny;
    return dx > std::cos(kScanDeg * kPi / 180) && dy > std::cos(kScanDeg * kPi / 180);
}

std::string Where(const MEMORY_BASIC_INFORMATION& mbi, const void* a) {
    char out[200];
    std::string mod = DescribeAddress(a);
    _snprintf_s(out, _countof(out), _TRUNCATE, "region %p size 0x%llX %s %s", mbi.AllocationBase,
                static_cast<unsigned long long>(mbi.RegionSize),
                mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : "private", mod.c_str());
    return out;
}

template <typename T>
std::string Context(const T* p, size_t avail) {
    std::string s;
    char b[32];
    for (size_t i = 0; i < 16 && i < avail; ++i) {
        _snprintf_s(b, _countof(b), _TRUNCATE, i ? " %.5g" : "%.5g", double(p[i]));
        s += b;
    }
    return s;
}

std::wstring OutPath() {
    std::wstring p = LogPath();
    const size_t k = p.find_last_of(L"\\/");
    return (k == std::wstring::npos ? std::wstring() : p.substr(0, k + 1)) + L"hullscan.log";
}

void WriteOut(const std::string& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, OutPath().c_str(), L"ab") == 0 && f) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

std::string Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[40];
    _snprintf_s(b, _countof(b), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d.%03d ", st.wYear, st.wMonth, st.wDay, st.wHour,
                st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

// Elements needed for a candidate kind (matrix layouts 0..3, quaternions 4..7).
size_t SpanFor(int layout) {
    return layout < 4 ? size_t(SpanElements(kLayouts[layout])) : layout < 8 ? 4 : layout == 8 ? 3 : 9;
}
int PosStride(int layout) { return layout == 9 ? 4 : 1; }

// Normalised forward axis of a candidate's data.
template <typename F>
bool CandForward(F el, int layout, double* fwd) {
    if (layout >= 4) return QuatForward(el, layout - 4, fwd);
    const Layout& l = kLayouts[layout];
    fwd[0] = el(0);
    fwd[1] = el(l.stride);
    fwd[2] = el(2 * l.stride);
    const double n = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (!(n > 0.9 && n < 1.1)) return false;
    for (int i = 0; i < 3; ++i) fwd[i] /= n;
    return true;
}

const char* KindName(int layout) {
    static const char* q[6] = {"quat-wxyz", "quat-xyzw", "quatT-wxyz", "quatT-xyzw", "pos", "pos-col"};
    return layout < 4 ? kLayouts[layout].name : q[layout - 4];
}

struct Match {
    unsigned char* addr;
    bool isDouble;
    int layout;
};

template <typename T>
void ScanBlock(const T* p, size_t count, unsigned char* base, bool isDouble, const double* ship,
               const double qt[2][4], std::vector<Match>& out, long& total) {
    const double pre[4] = {std::fabs(qt[0][0]), std::fabs(qt[0][1]), std::fabs(qt[1][0]), std::fabs(qt[1][1])};
    for (size_t i = 0; i < count; ++i) {
        const double v = double(p[i]);
        if (std::fabs(v - ship[0]) < 0.15) {
            for (int l = 0; l < 4; ++l) {
                if (!Matches(p + i, count - i, kLayouts[l], ship)) continue;
                ++total;
                if (out.size() < kMaxMatches) out.push_back({base + i * sizeof(T), isDouble, l});
            }
        }
        if (ship[9] != 0 && std::fabs(v - ship[9]) < kScanMetres) {
            for (int s = 1; s <= 4; s += 3) {
                if (i + 2 * size_t(s) >= count) continue;
                if (std::fabs(double(p[i + s]) - ship[10]) < kScanMetres &&
                    std::fabs(double(p[i + 2 * s]) - ship[11]) < kScanMetres) {
                    ++total;
                    if (out.size() < kMaxMatches) out.push_back({base + i * sizeof(T), isDouble, s == 1 ? 8 : 9});
                }
            }
        }
        const double av = std::fabs(v);
        if (std::fabs(av - pre[0]) < 0.08 || std::fabs(av - pre[1]) < 0.08 || std::fabs(av - pre[2]) < 0.08 ||
            std::fabs(av - pre[3]) < 0.08) {
            const int k = QuatMatch(p + i, count - i, qt);
            if (k >= 0) {
                ++total;
                if (out.size() < kMaxMatches) out.push_back({base + i * sizeof(T), isDouble, 4 + k});
            }
        }
    }
}

DWORD WINAPI ScanThread(void*) {
    const DWORD t0 = GetTickCount();
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ScanThread), &self);
    std::vector<Match> matches;
    unsigned long long scanned = 0;
    long total = 0;
    std::vector<unsigned char> buf;
    unsigned char* addr = nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        unsigned char* next = static_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool writable = prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE;
        const bool ours = self && mbi.AllocationBase == self;
        if (mbi.State == MEM_COMMIT && writable && !ours && !(mbi.Protect & PAGE_GUARD) &&
            mbi.RegionSize < (1ull << 31)) {
            buf.resize(mbi.RegionSize);
            if (SafeRead(mbi.BaseAddress, buf.data(), mbi.RegionSize)) {
                scanned += mbi.RegionSize;
                double ship[12], qt[2][4];
                {
                    std::lock_guard<std::mutex> g(g_mu);
                    memcpy(ship, g_ship, sizeof(ship));
                }
                ShipQuats(ship, qt);
                unsigned char* base = static_cast<unsigned char*>(mbi.BaseAddress);
                ScanBlock(reinterpret_cast<const float*>(buf.data()), mbi.RegionSize / 4, base, false, ship, qt,
                          matches, total);
                ScanBlock(reinterpret_cast<const double*>(buf.data()), mbi.RegionSize / 8, base, true, ship, qt,
                          matches, total);
            }
        }
        if (next <= addr) break;
        addr = next;
    }
    // Watch every match for kWatchPasses x 100 ms and count how often its bytes change. The drawn pose,
    // if it is refreshed every few seconds, changes only a few times; the simulation state changes every
    // 0.13 s, static data never.
    constexpr size_t kRawMax = 96;
    std::vector<unsigned char> last(matches.size() * kRawMax);
    std::vector<int> changes(matches.size(), 0);
    std::vector<char> readable(matches.size(), 1);
    for (int pass = 0; pass <= kWatchPasses; ++pass) {
        for (size_t k = 0; k < matches.size(); ++k) {
            if (!readable[k]) continue;
            const Match& m = matches[k];
            const size_t bytes = SpanFor(m.layout) * (m.isDouble ? 8 : 4);
            unsigned char raw[kRawMax];
            if (bytes > kRawMax || !SafeRead(m.addr, raw, bytes)) {
                readable[k] = 0;
                continue;
            }
            unsigned char* prev = &last[k * kRawMax];
            if (pass > 0 && memcmp(raw, prev, bytes) != 0) ++changes[k];
            memcpy(prev, raw, bytes);
        }
        if (pass < kWatchPasses) Sleep(100);
    }
    long bucket[5] = {};   // never, 1-15, 16-60, >60 changes, unreadable
    for (size_t k = 0; k < matches.size(); ++k) {
        const int c = changes[k];
        ++bucket[!readable[k] ? 4 : c == 0 ? 0 : c <= 15 ? 1 : c <= 60 ? 2 : 3];
    }
    // Keep the rare updaters that are still roughly where the ship is (loose tolerances).
    double ship[12];
    {
        std::lock_guard<std::mutex> g(g_mu);
        memcpy(ship, g_ship, sizeof(ship));
    }
    std::vector<Cand> found;
    long recheck = 0;
    int kept[2] = {0, 0};   // positions, orientations: up to half of kMaxCandidates each
    for (size_t k = 0; k < matches.size(); ++k) {
        const Match& m = matches[k];
        if (!readable[k] || changes[k] < kRareMin || changes[k] > kRareMax) continue;
        const int kind = m.layout >= 8 ? 0 : 1;
        const size_t es = m.isDouble ? 8 : 4;
        unsigned char raw[128] = {};
        const size_t bytes = SpanFor(m.layout) * es;
        if (bytes > sizeof(raw) || !SafeRead(m.addr, raw, bytes)) continue;
        auto el = [&](int i) -> double {
            if (m.isDouble) {
                double v;
                memcpy(&v, raw + i * 8, 8);
                return v;
            }
            float v;
            memcpy(&v, raw + i * 4, 4);
            return v;
        };
        if (m.layout >= 8) {
            const int ps = PosStride(m.layout);
            const double dx = el(0) - ship[9], dy = el(ps) - ship[10], dz = el(2 * ps) - ship[11];
            if (dx * dx + dy * dy + dz * dz > 1.5 * kScanMetres * 1.5 * kScanMetres) continue;
        } else {
            double f[3];
            if (!CandForward(el, m.layout, f)) continue;
            if (f[0] * ship[0] + f[1] * ship[1] + f[2] * ship[2] < std::cos(1.5 * kScanDeg * kPi / 180)) continue;
        }
        ++recheck;
        if (kept[kind] >= kMaxCandidates / 2) continue;
        ++kept[kind];
        Cand c;
        c.addr = m.addr;
        c.isDouble = m.isDouble;
        c.layout = m.layout;
        MEMORY_BASIC_INFORMATION mi{};
        VirtualQuery(m.addr, &mi, sizeof(mi));
        c.where = Where(mi, m.addr);
        unsigned char wide[128] = {};
        std::string ctx;
        if (SafeRead(m.addr, wide, 16 * es)) {
            char b[32];
            for (int i = 0; i < 16; ++i) {
                double v;
                if (m.isDouble) {
                    memcpy(&v, wide + i * 8, 8);
                } else {
                    float fv;
                    memcpy(&fv, wide + i * 4, 4);
                    v = fv;
                }
                _snprintf_s(b, _countof(b), _TRUNCATE, i ? " %.5g" : "%.5g", v);
                ctx += b;
            }
        }
        char nb[24];
        _snprintf_s(nb, _countof(nb), _TRUNCATE, "changes/10s=%d ", changes[k]);
        c.context = nb + ctx;
        found.push_back(c);
    }
    std::string out;
    const std::string st = Stamp();
    char line[600];
    _snprintf_s(line, _countof(line), _TRUNCATE,
                "%sHULLSCAN scan [%s] done: %ld matches (%zu stored); changes in 10 s: never %ld, 1-15 %ld, 16-60 %ld, "
                ">60 %ld, unreadable %ld; %ld changing copies still near the ship (%zu kept), %.0f MB in %.1f s\n",
                st.c_str(), g_label.c_str(), total, matches.size(), bucket[0], bucket[1], bucket[2], bucket[3], bucket[4],
                recheck, found.size(), scanned / 1048576.0,
                (GetTickCount() - t0) / 1000.0);
    out += line;
    for (size_t i = 0; i < found.size(); ++i) {
        const Cand& c = found[i];
        _snprintf_s(line, _countof(line), _TRUNCATE, "%sHULLSCAN cand #%zu %p %s %s | %s | %s\n", st.c_str(), i, c.addr,
                    c.isDouble ? "double" : "float", KindName(c.layout), c.where.c_str(), c.context.c_str());
        out += line;
    }
    WriteOut(out);
    Log("HULLSCAN scan [%s] done: %ld matches; changes in 10 s: never %ld, 1-15 %ld, 16-60 %ld, >60 %ld; "
        "%ld changing copies near the ship, %zu kept, %.0f MB in %.1f s (list in hullscan.log)",
        g_label.c_str(), total, bucket[0], bucket[1], bucket[2], bucket[3], recheck, found.size(), scanned / 1048576.0,
        (GetTickCount() - t0) / 1000.0);
    {
        std::lock_guard<std::mutex> g(g_mu);
        g_cands.swap(found);
    }
    g_running = false;
    return 0;
}

}  // namespace

void HullScanShip(const char* axes) {
    double a[12] = {};
    const int got = axes ? sscanf_s(axes, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &a[0], &a[1], &a[2], &a[3],
                                    &a[4], &a[5], &a[6], &a[7], &a[8], &a[9], &a[10], &a[11])
                         : 0;
    if (got != 9 && got != 12) return;
    if (!g_t0.QuadPart) QueryPerformanceCounter(&g_t0);
    const double t = Now();
    const double shipYaw = YawDeg(a[0], a[2]);
    std::lock_guard<std::mutex> g(g_mu);
    memcpy(g_ship, a, sizeof(a));
    g_haveShip = true;
    g_havePos = got == 12;
    for (Cand& c : g_cands) {
        const size_t es = c.isDouble ? 8 : 4;
        const size_t bytes = SpanFor(c.layout) * es;
        unsigned char raw[96] = {};
        ++c.n;
        ++c.totalN;
        if (bytes > sizeof(raw) || !SafeRead(c.addr, raw, bytes)) {
            ++c.bad;
            continue;
        }
        auto el = [&](int i) -> double {
            if (c.isDouble) {
                double v;
                memcpy(&v, raw + i * 8, 8);
                return v;
            }
            float v;
            memcpy(&v, raw + i * 4, 4);
            return v;
        };
        if (memcmp(raw, c.lastRaw, bytes) != 0) {
            ++c.changed;
            ++c.totalChanged;
            memcpy(c.lastRaw, raw, bytes);
        }
        double lag;
        if (c.layout >= 8) {
            // offset from the smooth ship position in ship axes: along = forward (x), side = right (z)
            const int ps = PosStride(c.layout);
            const double dx = el(0) - a[9], dy = el(ps) - a[10], dz = el(2 * ps) - a[11];
            if (!g_havePos || dx * dx + dy * dy + dz * dz > 1.5 * kScanMetres * 1.5 * kScanMetres) {
                ++c.bad;
                continue;
            }
            const double along = dx * a[0] + dy * a[1] + dz * a[2];
            lag = dx * a[6] + dy * a[7] + dz * a[8];
            if (!c.haveLast || along < c.alongMin) c.alongMin = along;
            if (!c.haveLast || along > c.alongMax) c.alongMax = along;
        } else {
            double fwd[3];
            if (!CandForward(el, c.layout, fwd)) {
                ++c.bad;
                continue;
            }
            lag = Wrap(YawDeg(fwd[0], fwd[2]) - shipYaw);
            if (std::fabs(lag) > 1.5 * kScanDeg) {  // no longer this ship's orientation (memory reused)
                ++c.bad;
                continue;
            }
        }
        const double bigStep = c.layout >= 8 ? 0.1 : 0.25, smallStep = c.layout >= 8 ? 0.02 : 0.05;
        if (c.haveLast) {
            const double d = lag - c.lastLag;
            if (std::fabs(d) > bigStep) {
                if (c.bigJumps < kMaxJumpTimes) {
                    c.jumpT[c.bigJumps] = t;
                    c.jumpD[c.bigJumps] = d;
                }
                ++c.bigJumps;
            } else if (std::fabs(d) > smallStep) {
                ++c.smallJumps;
            }
        }
        if (!c.haveLast || lag < c.lagMin) c.lagMin = lag;
        if (!c.haveLast || lag > c.lagMax) c.lagMax = lag;
        c.lagSum += lag;
        c.lastLag = lag;
        c.haveLast = true;
    }
}

std::string HullScanStart(const char* label) {
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (!g_haveShip) return "no ship axes yet";
    }
    if (g_running.exchange(true)) return "a scan is already running";
    g_label = label ? label : "";
    HANDLE th = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
    if (!th) {
        g_running = false;
        return "CreateThread failed";
    }
    SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(th);
    return "";
}

std::string HullScanStats() {
    std::string out;
    const std::string st = Stamp();
    char line[700];
    long cands = 0, valid = 0, moving = 0, saw = 0;
    std::lock_guard<std::mutex> g(g_mu);
    const double t = g_t0.QuadPart ? Now() : 0;
    for (size_t i = 0; i < g_cands.size(); ++i) {
        Cand& c = g_cands[i];
        ++cands;
        const long good = c.n - c.bad;
        if (good > 0) {
            ++valid;
            if (c.changed > 0) ++moving;
            const bool sawtooth = c.bigJumps > 0 && c.lagMax - c.lagMin > 0.2;
            if (sawtooth) ++saw;
            std::string jumps, along;
            char b[64];
            if (c.layout >= 8) {
                _snprintf_s(b, _countof(b), _TRUNCATE, " along %+.3f..%+.3f m", c.alongMin, c.alongMax);
                along = b;
            }
            for (long k = 0; k < c.bigJumps && k < kMaxJumpTimes; ++k) {
                _snprintf_s(b, _countof(b), _TRUNCATE, " %.2f(%+.2f)", c.jumpT[k], c.jumpD[k]);
                jumps += b;
            }
            _snprintf_s(line, _countof(line), _TRUNCATE,
                        "%sHULLSCAN #%zu %p %s %s t=%.2f n=%ld bad=%ld changed=%ld lag min %+.3f max %+.3f mean %+.3f "
                        "last %+.3f %s jumps>big=%ld >small=%ld%s%s%s | at%s\n",
                        st.c_str(), i, c.addr, c.isDouble ? "double" : "float", KindName(c.layout), t, c.n, c.bad,
                        c.changed, c.lagMin, c.lagMax, c.lagSum / double(good), c.lastLag,
                        c.layout >= 8 ? "m sideways" : "deg", c.bigJumps, c.smallJumps, sawtooth ? " SAWTOOTH" : "",
                        c.totalChanged == 0 ? " never-changed" : "", along.c_str(), jumps.c_str());
            out += line;
        }
        c.n = c.bad = c.changed = c.bigJumps = c.smallJumps = 0;
        c.alongMin = c.alongMax = 0;
        c.lagMin = c.lagMax = c.lagSum = 0;
        c.haveLast = false;
    }
    if (!out.empty()) WriteOut(out);
    char sum[200];
    _snprintf_s(sum, _countof(sum), _TRUNCATE, "candidates=%ld valid=%ld changing=%ld sawtooth=%ld%s", cands, valid,
                moving, saw, g_running ? " (scan running)" : "");
    return sum;
}

}  // namespace lsodiag
