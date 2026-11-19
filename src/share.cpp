// Phase 5 producer: per-frame GPU copy of DCS render targets into shareable D3D11 textures.
//
// Per target:
//   * a texture on DCS's device with the source's size/format, created with
//     D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX and published
//     under a named NT handle (consumers open it by name; no handle duplication needed)
//   * every frame, ON THE DCS RENDER THREAD (EndFrame renderer callback, render_callback.h):
//     keyed mutex AcquireSync(0, timeout 0) -> CopyResource -> ReleaseSync(0).
//     A busy mutex means a consumer is mid-copy: the frame is skipped, DCS never waits.
//   * once a second: isRenderTargetExists (no dcs.log noise when absent) and, when present,
//     re-open the source to detect recreation (pointer or description change -> new generation).
//
// Metadata for consumers lives in a named file mapping (share_protocol.h).
// The DCS-side work is one CopyResource per target per frame on the immediate context; no
// pipeline state (shaders, targets, viewports, bindings) is touched.

#include "share.h"

#include "capture.h"
#include "common.h"
#include "compat.h"
#include "engine.h"
#include "mfd.h"
#include "render_callback.h"
#include "share_protocol.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace lsodiag {
namespace {

constexpr ULONGLONG kRevalidateMs = 1000;

struct Target {
    std::string name;
    ID3D11Texture2D* src = nullptr;  // our COM reference to DCS's texture
    ID3D11Texture2D* shared = nullptr;
    IDXGIKeyedMutex* mutex = nullptr;
    HANDLE handle = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    LONG generation = 0;
    unsigned long long copies = 0, skipped = 0, failures = 0;
    ULONGLONG nextCheck = 0;
    int slot = -1;
};

struct State {
    bool running = false;
    Renderer renderer;
    ID3D11Device* dev = nullptr;          // borrowed from DCS
    ID3D11DeviceContext* ctx = nullptr;   // borrowed from DCS
    HANDLE mapping = nullptr;
    lsoshare::Header* header = nullptr;
    std::vector<Target> targets;
    DWORD thread = 0;
    bool logHeld = false;
} g;

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void ReleaseShared(Target& t) {
    SafeRelease(t.mutex);
    SafeRelease(t.shared);
    if (t.handle) {
        CloseHandle(t.handle);
        t.handle = nullptr;
    }
}

lsoshare::TargetSlot& Slot(const Target& t) { return g.header->targets[t.slot]; }

void PublishInactive(Target& t) {
    if (g.header && t.slot >= 0) Slot(t).active = 0;
}

// (Re)creates the shareable copy for the current source description. Logs every step.
bool CreateShared(Target& t, const D3D11_TEXTURE2D_DESC& srcDesc) {
    ReleaseShared(t);
    D3D11_TEXTURE2D_DESC d = srcDesc;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.CPUAccessFlags = 0;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    HRESULT hr = g.dev->CreateTexture2D(&d, nullptr, &t.shared);
    if (FAILED(hr)) {
        Log("SHARE %s CreateTexture2D(shared %ux%u fmt=%d) failed hr=0x%08lX", t.name.c_str(), d.Width, d.Height,
            static_cast<int>(d.Format), static_cast<unsigned long>(hr));
        return false;
    }
    hr = t.shared->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&t.mutex));
    IDXGIResource1* res1 = nullptr;
    if (SUCCEEDED(hr)) hr = t.shared->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&res1));
    ++t.generation;
    wchar_t handleName[96];
    swprintf_s(handleName, L"Local\\DCS_LSO_%S_%lu_%ld", t.name.c_str(), GetCurrentProcessId(), t.generation);
    if (SUCCEEDED(hr)) {
        hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, handleName,
                                      &t.handle);
    }
    SafeRelease(res1);
    if (FAILED(hr)) {
        Log("SHARE %s keyed mutex / CreateSharedHandle failed hr=0x%08lX", t.name.c_str(), static_cast<unsigned long>(hr));
        ReleaseShared(t);
        return false;
    }
    t.desc = srcDesc;

    lsoshare::TargetSlot& s = Slot(t);
    s.active = 0;
    MemoryBarrier();
    strncpy_s(s.name, t.name.c_str(), _TRUNCATE);
    wcsncpy_s(s.handleName, handleName, _TRUNCATE);
    s.width = d.Width;
    s.height = d.Height;
    s.format = static_cast<uint32_t>(d.Format);
    MemoryBarrier();
    InterlockedExchange(&s.generation, t.generation);
    s.active = 1;
    Log("SHARE %s published %ux%u fmt=%d as %S (generation %ld)", t.name.c_str(), d.Width, d.Height,
        static_cast<int>(d.Format), handleName, t.generation);
    return true;
}

// Once a second: is the source still there, and is it still the same texture?
void Revalidate(Target& t) {
    DWORD code = 0;
    int exists = SehIsRtExists(g.renderer, t.name.c_str(), &code);
    if (exists < 0) {
        DisableEngine("isRenderTargetExists", code);
        return;
    }
    if (!exists) {
        if (t.src) Log("SHARE %s source disappeared", t.name.c_str());
        SafeRelease(t.src);
        PublishInactive(t);
        return;
    }
    std::string why;
    ID3D11Texture2D* tex = OpenDcsTexture(g.renderer, t.name.c_str(), t.src == nullptr, why);
    if (!tex) {
        if (t.src || t.failures == 0) Log("SHARE %s cannot open source: %s", t.name.c_str(), why.c_str());
        ++t.failures;
        SafeRelease(t.src);
        PublishInactive(t);
        return;
    }
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    bool sameTex = tex == t.src;
    bool sameDesc = t.shared && d.Width == t.desc.Width && d.Height == t.desc.Height && d.Format == t.desc.Format;
    SafeRelease(t.src);
    t.src = tex;
    if (!sameTex) Log("SHARE %s source texture %p (%ux%u fmt=%d samples=%u)", t.name.c_str(), static_cast<void*>(tex),
                      d.Width, d.Height, static_cast<int>(d.Format), d.SampleDesc.Count);
    if (d.SampleDesc.Count != 1 || d.ArraySize != 1) {
        Log("SHARE %s unsupported source (samples=%u array=%u); not sharing", t.name.c_str(), d.SampleDesc.Count,
            d.ArraySize);
        SafeRelease(t.src);
        PublishInactive(t);
        return;
    }
    if (!sameDesc && !CreateShared(t, d)) {
        SafeRelease(t.src);
        PublishInactive(t);
        return;
    }
    if (g.header) Slot(t).active = 1;
}

// SEH around the per-frame D3D calls (objects are validated; this is a last line of defence).
bool SehCopy(Target& t, DWORD* code, HRESULT* acquire) {
    __try {
        *acquire = t.mutex->AcquireSync(0, 0);
        if (*acquire != S_OK) return false;  // WAIT_TIMEOUT: consumer is copying; skip this frame
        // Source has 1 mip / 1 slice (checked); CopySubresourceRegion tolerates a mip-count mismatch.
        g.ctx->CopySubresourceRegion(t.shared, 0, 0, 0, 0, t.src, 0, nullptr);
        t.mutex->ReleaseSync(0);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------- render-thread work

SRWLOCK g_lock = SRWLOCK_INIT;  // guards g and g_captureQueue between Lua thread and render thread
std::vector<std::string> g_captureQueue;
volatile LONG64 g_frameCount = 0;  // EndFrame callbacks (DCS frames), for the cameras' performance check
// Render targets whose existence the hook asks about (extra cameras): the renderer's list may only be read
// on the render thread, so the answers are refreshed there once a second.
struct ProbeName {
    std::string name;
    int exists = -1;  // -1 not checked yet
};
std::vector<ProbeName> g_probes;
ULONGLONG g_probeAt = 0;

// Frame-order experiment (Option B). For each candidate renderer event E: paint a 32x32 white
// square into LSOFRAME at event E for kPaintFrames frames, then kRestFrames frames without. Every
// EndFrame (before any EndFrame paint) a 24x24 box of LSOFRAME and of LSOFINALTARGET at the marker
// position is copied into a staging ring and read back a few frames later without stalling. Tallies
// per (event, phase) tell at which event a write into LSOFRAME reliably reaches the LSO station
// composite. Uses only CopySubresourceRegion / Map (no pipeline state).
constexpr int kPaintFrames = 60, kRestFrames = 30, kRing = 6;
constexpr UINT kLsoX = 20, kLsoY = 20, kFinalX = 218, kFinalY = 170, kBox = 24;

struct ProbeSlot {
    ID3D11Texture2D* lso = nullptr;
    ID3D11Texture2D* fin = nullptr;
    bool pending = false;
    int eventIdx = 0, phase = 0;
};

struct Experiment {
    bool active = false;
    std::vector<int> events;
    size_t idx = 0;
    int phase = 0;  // 0 paint, 1 rest
    int framesLeft = 0;
    int fires[9] = {};
    // tallies[eventIdx][phase][0=lso,1=final][0=marked,1=total]
    std::vector<std::array<std::array<std::array<int, 2>, 2>, 2>> tallies;
    std::vector<std::array<int, 2>> firesDuringPaint;
    ID3D11Texture2D* marker = nullptr;
    DXGI_FORMAT markerFmt = DXGI_FORMAT_UNKNOWN;
    ProbeSlot ring[kRing];
    int ringWrite = 0;
    int seqFrames = 0;  // frames of event order still to log
    std::string seq;
    int drainFrames = 0;
} g_exp;

ID3D11Texture2D* TargetSrc(const char* name) {
    for (auto& t : g.targets)
        if (t.name == name && t.src) return t.src;
    return nullptr;
}

bool SehCopyBox(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT dx, UINT dy, ID3D11Texture2D* src, const D3D11_BOX* box,
                DWORD* code) {
    __try {
        ctx->CopySubresourceRegion(dst, 0, dx, dy, 0, src, 0, box);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float SmallFloatBits(uint32_t bits, int mantBits) {
    uint32_t exp = bits >> mantBits, mant = bits & ((1u << mantBits) - 1);
    if (exp == 0) return std::ldexp(static_cast<float>(mant), -14 - mantBits);
    if (exp == 31) return 65504.0f;
    return std::ldexp(static_cast<float>(mant | (1u << mantBits)), static_cast<int>(exp) - 15 - mantBits);
}

// Mean brightness 0..1 of a mapped box (R11G11B10_FLOAT or 8-bit RGBA).
float MeanOf(const D3D11_MAPPED_SUBRESOURCE& m, DXGI_FORMAT f) {
    double sum = 0;
    for (UINT y = 0; y < kBox; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch);
        for (UINT x = 0; x < kBox; ++x) {
            uint32_t v = row[x];
            if (f == DXGI_FORMAT_R11G11B10_FLOAT)
                sum += (SmallFloatBits(v & 0x7FF, 6) + SmallFloatBits((v >> 11) & 0x7FF, 6) + SmallFloatBits((v >> 22) & 0x3FF, 5)) / 3.0;
            else
                sum += ((v & 0xFF) + ((v >> 8) & 0xFF) + ((v >> 16) & 0xFF)) / (3.0 * 255.0);
        }
    }
    return static_cast<float>(sum / (kBox * kBox));
}

void ExperimentRelease() {
    SafeRelease(g_exp.marker);
    for (auto& s : g_exp.ring) {
        SafeRelease(s.lso);
        SafeRelease(s.fin);
        s.pending = false;
    }
}

void PaintAt(int ev) {
    ID3D11Texture2D* lso = TargetSrc("LSOFRAME");
    if (!lso) return;
    D3D11_TEXTURE2D_DESC d{};
    lso->GetDesc(&d);
    if (!g_exp.marker || g_exp.markerFmt != d.Format) {
        SafeRelease(g_exp.marker);
        uint32_t white = d.Format == DXGI_FORMAT_R11G11B10_FLOAT ? (0x3C0u | (0x3C0u << 11) | (0x1E0u << 22)) : 0xFFFFFFFFu;
        std::vector<uint32_t> px(32 * 32, white);
        D3D11_TEXTURE2D_DESC md{32, 32, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA init{px.data(), 32 * 4, 0};
        if (FAILED(g.dev->CreateTexture2D(&md, &init, &g_exp.marker))) {
            Log("EXPERIMENT marker texture creation failed");
            g_exp.active = false;
            return;
        }
        g_exp.markerFmt = d.Format;
    }
    DWORD code = 0;
    if (!SehCopyBox(g.ctx, lso, 16, 16, g_exp.marker, nullptr, &code)) {
        DisableEngine("experiment marker copy", code);
        g_exp.active = false;
    }
    (void)ev;
}

// Called for every renderer event on the render thread while the experiment runs (lock held).
void ExperimentOnEvent(int ev) {
    if (g_exp.seqFrames > 0) {
        g_exp.seq += (g_exp.seq.empty() ? "" : " ") + std::to_string(ev);
        if (ev == 6 && --g_exp.seqFrames == 0) {
            Log("EXPERIMENT per-frame event order (render thread): %s", g_exp.seq.c_str());
        }
    }
    if (!g_exp.active || g_exp.idx >= g_exp.events.size()) return;
    if (ev != 6) {
        ++g_exp.fires[ev];
        if (g_exp.phase == 0 && ev == g_exp.events[g_exp.idx]) {
            ++g_exp.firesDuringPaint[g_exp.idx][0];
            PaintAt(ev);
        }
    }
}

void ExperimentDrain() {
    for (int k = 0; k < kRing; ++k) {
        ProbeSlot& s = g_exp.ring[(g_exp.ringWrite + k) % kRing];
        if (!s.pending) continue;
        D3D11_MAPPED_SUBRESOURCE ml{}, mf{};
        if (g.ctx->Map(s.lso, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ml) != S_OK) break;
        if (g.ctx->Map(s.fin, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mf) != S_OK) {
            g.ctx->Unmap(s.lso, 0);
            break;
        }
        D3D11_TEXTURE2D_DESC dl{}, df{};
        s.lso->GetDesc(&dl);
        s.fin->GetDesc(&df);
        bool lsoMarked = MeanOf(ml, dl.Format) > 0.6f;
        bool finMarked = MeanOf(mf, df.Format) > 0.35f;
        g.ctx->Unmap(s.lso, 0);
        g.ctx->Unmap(s.fin, 0);
        auto& t = g_exp.tallies[s.eventIdx][s.phase];
        t[0][0] += lsoMarked;
        t[0][1] += 1;
        t[1][0] += finMarked;
        t[1][1] += 1;
        s.pending = false;
    }
}

void ExperimentReport() {
    Log("EXPERIMENT results (marker present / frames probed at EndFrame, before any EndFrame paint):");
    for (size_t i = 0; i < g_exp.events.size(); ++i) {
        const auto& t = g_exp.tallies[i];
        Log("EXPERIMENT  paint@event %d (fired %d times while painting): PAINT phase LSOFRAME %d/%d, LSOFINALTARGET %d/%d | "
            "REST phase LSOFRAME %d/%d, LSOFINALTARGET %d/%d",
            g_exp.events[i], g_exp.events[i] == 6 ? kPaintFrames : g_exp.firesDuringPaint[i][0], t[0][0][0], t[0][0][1],
            t[0][1][0], t[0][1][1], t[1][0][0], t[1][0][1], t[1][1][0], t[1][1][1]);
    }
    std::string f;
    for (int ev = 0; ev < 9; ++ev) f += " e" + std::to_string(ev) + "=" + std::to_string(g_exp.fires[ev]);
    Log("EXPERIMENT render-thread event fires during experiment:%s | all threads: %s", f.c_str(), RenderEventStats().c_str());
}

// EndFrame part (lock held): probe, optional EndFrame paint, phase bookkeeping.
void ExperimentOnEndFrame() {
    if (!g_exp.active) return;
    if (g_exp.drainFrames > 0) {
        ExperimentDrain();
        if (--g_exp.drainFrames == 0) {
            ExperimentReport();
            g_exp.active = false;
            ExperimentRelease();
        }
        return;
    }
    ID3D11Texture2D* lso = TargetSrc("LSOFRAME");
    ID3D11Texture2D* fin = TargetSrc("LSOFINALTARGET");
    if (!lso || !fin) return;  // wait until the LSO view exists
    ExperimentDrain();
    ProbeSlot& s = g_exp.ring[g_exp.ringWrite];
    if (!s.lso) {
        D3D11_TEXTURE2D_DESC dl{}, df{};
        lso->GetDesc(&dl);
        fin->GetDesc(&df);
        for (auto* d : {&dl, &df}) {
            d->Width = d->Height = kBox;
            d->MipLevels = d->ArraySize = 1;
            d->SampleDesc = {1, 0};
            d->Usage = D3D11_USAGE_STAGING;
            d->BindFlags = 0;
            d->CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            d->MiscFlags = 0;
        }
        if (FAILED(g.dev->CreateTexture2D(&dl, nullptr, &s.lso)) || FAILED(g.dev->CreateTexture2D(&df, nullptr, &s.fin))) {
            Log("EXPERIMENT staging creation failed");
            g_exp.active = false;
            return;
        }
    }
    if (!s.pending) {
        D3D11_BOX bl{kLsoX, kLsoY, 0, kLsoX + kBox, kLsoY + kBox, 1}, bf{kFinalX, kFinalY, 0, kFinalX + kBox, kFinalY + kBox, 1};
        DWORD code = 0;
        if (!SehCopyBox(g.ctx, s.lso, 0, 0, lso, &bl, &code) || !SehCopyBox(g.ctx, s.fin, 0, 0, fin, &bf, &code)) {
            DisableEngine("experiment probe copy", code);
            g_exp.active = false;
            return;
        }
        s.pending = true;
        s.eventIdx = static_cast<int>(g_exp.idx);
        s.phase = g_exp.phase;
        g_exp.ringWrite = (g_exp.ringWrite + 1) % kRing;
    }
    if (g_exp.phase == 0 && g_exp.events[g_exp.idx] == 6) PaintAt(6);
    if (--g_exp.framesLeft <= 0) {
        if (g_exp.phase == 0) {
            g_exp.phase = 1;
            g_exp.framesLeft = kRestFrames;
        } else if (++g_exp.idx < g_exp.events.size()) {
            g_exp.phase = 0;
            g_exp.framesLeft = kPaintFrames;
        } else {
            g_exp.drainFrames = kRing + 4;  // collect outstanding probes, then report
        }
    }
}

void StopLocked();

// Runs on the DCS render thread from the EndFrame callback. Never blocks: if the Lua thread holds
// the lock (start/stop/capture request), this frame is simply skipped.
void RenderFrame(int ev) {
    if (ev != 6) {
        // Other renderer events: only the frame-order experiment acts, and only on the render thread.
        if (GetCurrentThreadId() != RenderThreadId() || !TryAcquireSRWLockExclusive(&g_lock)) return;
        if (g.running && !EngineDisabled()) ExperimentOnEvent(ev);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    InterlockedIncrement64(&g_frameCount);
    MfdOnRenderThread();  // pending viewport resolution requests (own lock, never blocks)
    if (!TryAcquireSRWLockExclusive(&g_lock)) return;
    if (g.running && !EngineDisabled()) ExperimentOnEvent(6);
    if (EngineDisabled()) {
        if (g.running) StopLocked();
        g_captureQueue.clear();
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    // Queued captures (one-off; file writing here causes a brief hitch, acceptable for diagnostics).
    if (!g_captureQueue.empty()) {
        std::vector<std::string> queue;
        queue.swap(g_captureQueue);
        bool opened = OpenLog();
        for (const auto& name : queue) {
            std::string why;
            std::string res = CaptureRenderTarget(name.c_str(), why);
            Log("CAPTURE %s on render thread %lu: %s", name.c_str(), GetCurrentThreadId(),
                res.empty() ? ("failed: " + why).c_str() : res.c_str());
        }
        if (opened) CloseLog();
    }
    if (g.running) {
        if (g.thread == 0) {
            g.thread = GetCurrentThreadId();
            Log("SHARE first frame on render thread %lu", g.thread);
        }
        const ULONGLONG now = GetTickCount64();
        for (auto& t : g.targets) {
            if (now >= t.nextCheck) {
                t.nextCheck = now + kRevalidateMs;
                Revalidate(t);
                if (EngineDisabled()) break;
            }
            if (!t.src || !t.shared) continue;
            DWORD code = 0;
            HRESULT acquire = S_OK;
            if (SehCopy(t, &code, &acquire)) {
                ++t.copies;
                LARGE_INTEGER q;
                QueryPerformanceCounter(&q);
                Slot(t).lastCopyQpc = q.QuadPart;
                InterlockedIncrement64(&Slot(t).frameCounter);
            } else if (code) {
                DisableEngine("shared copy", code);
                break;
            } else {
                ++t.skipped;
                if (acquire != static_cast<HRESULT>(WAIT_TIMEOUT) && t.skipped < 5) {
                    Log("SHARE %s AcquireSync returned 0x%08lX", t.name.c_str(), static_cast<unsigned long>(acquire));
                }
            }
        }
        if (!EngineDisabled()) ExperimentOnEndFrame();  // after captures and copies
        const ULONGLONG tick = GetTickCount64();
        if (!g_probes.empty() && tick >= g_probeAt && !EngineDisabled()) {
            g_probeAt = tick + 1000;
            for (auto& p : g_probes) {
                DWORD code = 0;
                const int r = SehIsRtExists(g.renderer, p.name.c_str(), &code);
                if (r < 0) {
                    DisableEngine("isRenderTargetExists", code);
                    break;
                }
                p.exists = r;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

std::string StatusLocked() {
    char head[160];
    _snprintf_s(head, _countof(head), _TRUNCATE, "%s render-thread=%lu callbacks=%llu", g.running ? "running" : "stopped",
                RenderThreadId(), RenderCallbackCount());
    std::string s = head;
    for (const auto& t : g.targets) {
        char buf[200];
        _snprintf_s(buf, _countof(buf), _TRUNCATE, " | %s gen=%ld %ux%u copies=%llu skipped=%llu %s", t.name.c_str(),
                    t.generation, t.desc.Width, t.desc.Height, t.copies, t.skipped, t.src ? "live" : "idle");
        s += buf;
    }
    return s;
}

// D3D11 objects may be released from any thread (the device is free-threaded); the render thread
// cannot be using them concurrently because it only touches them while holding g_lock.
void StopLocked() {
    if (!g.running && !g.header) return;
    Log("SHARE stopping: %s", StatusLocked().c_str());
    g_exp.active = false;
    ExperimentRelease();
    for (auto& t : g.targets) {
        PublishInactive(t);
        SafeRelease(t.src);
        ReleaseShared(t);
    }
    g.targets.clear();
    g_probes.clear();
    if (g.header) {
        g.header->magic = 0;
        UnmapViewOfFile(g.header);
        g.header = nullptr;
    }
    if (g.mapping) {
        CloseHandle(g.mapping);
        g.mapping = nullptr;
    }
    g.dev = nullptr;
    g.ctx = nullptr;
    g.running = false;
    g.thread = 0;
    if (g.logHeld) {
        CloseLog();
        g.logHeld = false;
    }
}

struct Exclusive {
    Exclusive() { AcquireSRWLockExclusive(&g_lock); }
    ~Exclusive() { ReleaseSRWLockExclusive(&g_lock); }
};

void PublishCompatLocked() {
    if (!g.header) return;
    const CompatResult r = CompatCheck();
    auto& c = g.header->compat;
    c.state = static_cast<uint32_t>(r.state);
    strncpy_s(c.dcsVersion, r.dcsVersion.c_str(), _TRUNCATE);
    strncpy_s(c.verifiedVersion, kVerifiedBuild, _TRUNCATE);
    strncpy_s(c.detail, r.state == CompatState::Incompatible ? r.detail.c_str() : "", _TRUNCATE);
}

// Creates (or re-initialises) the mapping and its header: everything zero, producer identity, compat.
std::string InitMappingLocked() {
    if (!g.mapping) {
        g.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(lsoshare::Header),
                                       lsoshare::kMappingName);
        if (!g.mapping) return "CreateFileMapping failed: " + std::to_string(GetLastError());
    }
    if (!g.header) {
        g.header = static_cast<lsoshare::Header*>(MapViewOfFile(g.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(lsoshare::Header)));
        if (!g.header) {
            CloseHandle(g.mapping);
            g.mapping = nullptr;
            return "MapViewOfFile failed";
        }
    }
    g.header->magic = 0;
    MemoryBarrier();
    memset(g.header, 0, sizeof(*g.header));
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g.header->qpcFrequency = f.QuadPart;
    g.header->producerPid = GetCurrentProcessId();
    g.header->version = lsoshare::kVersion;
    PublishCompatLocked();
    return {};
}

}  // namespace

std::string ShareStart(const std::vector<std::string>& names) {
    Exclusive lock;
    if (g.running) return {};
    std::string why = ResolveRenderer(g.renderer);
    if (!why.empty()) return why;
    if (!RendererDevice(g.renderer, &g.dev, &g.ctx)) return "renderer device/context unavailable";
    if (names.empty() || names.size() > static_cast<size_t>(lsoshare::kMaxTargets)) return "need 1.." + std::to_string(lsoshare::kMaxTargets) + " target names";

    // Keyed-mutex shared NT handles need ID3D11Device1 (D3D11.1). QueryInterface is thread-safe.
    ID3D11Device1* dev1 = nullptr;
    if (FAILED(g.dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1)))) {
        return "DCS device does not support ID3D11Device1 (needed for NT-handle sharing)";
    }
    dev1->Release();

    why = EnsureRenderCallback(&RenderFrame);
    if (!why.empty()) return "render-thread callback unavailable: " + why;

    why = InitMappingLocked();
    if (!why.empty()) return why;

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(g.dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) &&
        SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&ad))) {
        g.header->adapterLuid = ad.AdapterLuid;
    }
    SafeRelease(adapter);
    SafeRelease(dxgiDev);
    g.header->targetCount = static_cast<uint32_t>(names.size());
    MemoryBarrier();
    g.header->magic = lsoshare::kMagic;

    g.targets.clear();
    for (size_t i = 0; i < names.size(); ++i) {
        Target t;
        t.name = names[i];
        t.slot = static_cast<int>(i);
        strncpy_s(g.header->targets[i].name, t.name.c_str(), _TRUNCATE);
        g.targets.push_back(std::move(t));
    }
    g.thread = 0;  // set by the first render-thread frame
    g.running = true;
    g.logHeld = OpenLog();  // keep the log open while sharing so render-thread events can be logged cheaply
    Log("SHARE started (requested from thread %lu; copies run on the render thread), adapter '%S' LUID %08lX:%08lX, "
        "mapping %S, %zu targets",
        GetCurrentThreadId(), ad.Description, static_cast<unsigned long>(g.header->adapterLuid.HighPart),
        static_cast<unsigned long>(g.header->adapterLuid.LowPart), lsoshare::kMappingName, names.size());
    return {};
}

std::string ShareAnnounce() {
    Exclusive lock;
    if (g.running) {
        PublishCompatLocked();
        return {};
    }
    std::string why = InitMappingLocked();
    if (!why.empty()) return why;
    MemoryBarrier();
    g.header->magic = lsoshare::kMagic;
    Log("SHARE announce only (DCS %s): no feeds", g.header->compat.dcsVersion);
    return {};
}

std::string QueueCapture(const std::string& name) {
    {
        Renderer r;
        std::string why = ResolveRenderer(r);
        if (!why.empty()) return why;
    }
    std::string why = EnsureRenderCallback(&RenderFrame);
    if (!why.empty()) return "render-thread callback unavailable: " + why;
    Exclusive lock;
    g_captureQueue.push_back(name);
    return {};
}

std::string QueueMarker(int frames) {
    (void)frames;
    Exclusive lock;
    if (!g.running) return "sharing not running";
    ExperimentRelease();
    g_exp = Experiment{};
    g_exp.events = {6, 1, 7, 8, 0, 2, 3, 4, 5};
    g_exp.tallies.assign(g_exp.events.size(), {});
    g_exp.firesDuringPaint.assign(g_exp.events.size(), {0, 0});
    g_exp.framesLeft = kPaintFrames;
    g_exp.seqFrames = 3;
    g_exp.active = true;
    Log("EXPERIMENT started: events 6,1,7,8,0,2,3,4,5 x (%d paint + %d rest) frames", kPaintFrames, kRestFrames);
    return {};
}

void ShareFrame() {}  // kept for script compatibility; work now happens on the render thread

void ShareSetStatusText(const char* text) {
    lsoshare::Header* h = g.header;
    if (!h || !text) return;
    if (strncmp(h->status, text, sizeof(h->status)) == 0) return;
    char tmp[sizeof(h->status)] = {};
    strncpy_s(tmp, text, _TRUNCATE);
    memcpy(h->status, tmp, sizeof(tmp));
}

// Carrier list: "name|type|x|z" records separated by '\n'; selected = index or -1.
void ShareSetCarriers(const char* list, int selected) {
    lsoshare::Header* h = g.header;
    if (!h || !list) return;
    uint32_t n = 0;
    const char* p = list;
    while (*p && n < _countof(h->carriers)) {
        const char* e = strchr(p, '\n');
        const std::string rec(p, e ? static_cast<size_t>(e - p) : strlen(p));
        p = e ? e + 1 : p + strlen(p);
        const size_t a = rec.find('|'), b = a == std::string::npos ? a : rec.find('|', a + 1),
                     c = b == std::string::npos ? b : rec.find('|', b + 1);
        if (c == std::string::npos) continue;
        auto& out = h->carriers[n++];
        strncpy_s(out.name, rec.substr(0, a).c_str(), _TRUNCATE);
        strncpy_s(out.type, rec.substr(a + 1, b - a - 1).c_str(), _TRUNCATE);
        out.x = static_cast<float>(atof(rec.substr(b + 1, c - b - 1).c_str()));
        out.z = static_cast<float>(atof(rec.substr(c + 1).c_str()));
    }
    h->carrierCount = n;
    h->selected = selected >= 0 && static_cast<uint32_t>(selected) < n ? selected : -1;
}

// PLAT overlay values: "key=value" lines (keys as in lsoshare::Header::Symbology).
void ShareSetSymbology(const char* text) {
    lsoshare::Header* h = g.header;
    if (!h || !text) return;
    auto& y = h->sym;
    const char* p = text;
    while (*p) {
        const char* e = strchr(p, '\n');
        const std::string line(p, e ? static_cast<size_t>(e - p) : strlen(p));
        p = e ? e + 1 : p + strlen(p);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto str = [&](char* dst, size_t n) { strncpy_s(dst, n, v.c_str(), _TRUNCATE); };
        if (k == "date") str(y.date, sizeof(y.date));
        else if (k == "time") str(y.time, sizeof(y.time));
        else if (k == "status") str(y.status, sizeof(y.status));
        else if (k == "id") str(y.carrierId, sizeof(y.carrierId));
        else if (k == "wind") str(y.deckWind, sizeof(y.deckWind));
        else if (k == "speed") str(y.speed, sizeof(y.speed));
        else if (k == "distance") str(y.distance, sizeof(y.distance));
        else if (k == "sink") str(y.sink, sizeof(y.sink));
        else if (k == "statusVisible") y.statusVisible = atoi(v.c_str()) ? 1 : 0;
        else if (k == "waveoff") y.waveoff = atoi(v.c_str()) ? 1 : 0;
        else if (k == "night") y.night = atoi(v.c_str()) ? 1 : 0;
        else if (k == "dcsOverlay") y.dcsOverlay = atoi(v.c_str()) ? 1 : 0;
        else if (k == "valid") y.valid = atoi(v.c_str()) ? 1 : 0;
        else if (k == "gateFov") y.gateFovDeg = static_cast<float>(atof(v.c_str()));
        else if (k == "gateDistance") y.gateDistance = static_cast<float>(atof(v.c_str()));
        else if (k == "gateDeltaH") y.gateDeltaH = static_cast<float>(atof(v.c_str()));
        else if (k == "st_valid") h->station.valid = atoi(v.c_str()) ? 1 : 0;
        else if (k == "st_heading") str(h->station.heading, sizeof(h->station.heading));
        else if (k == "st_list") str(h->station.list, sizeof(h->station.list));
        else if (k == "st_trim") str(h->station.trim, sizeof(h->station.trim));
        else if (k == "st_cross") str(h->station.cross, sizeof(h->station.cross));
        else if (k == "st_crossSide") h->station.crossSide = atoi(v.c_str());
        else if (k == "st_foul") h->station.foul = atoi(v.c_str());
        else if (k == "st_acType") str(h->station.acType, sizeof(h->station.acType));
        else if (k == "st_acHE") str(h->station.acHE, sizeof(h->station.acHE));
        else if (k == "st_acBA") str(h->station.acBA, sizeof(h->station.acBA));
        else if (k == "st_htr") h->station.htrFt = static_cast<float>(atof(v.c_str()));
        else if (k == "st_htd") h->station.htdFt = static_cast<float>(atof(v.c_str()));
        else if (k == "own_aircraft") h->ownAircraft = static_cast<uint32_t>(atoi(v.c_str()));  // v8: 1 in an aircraft, 2 none
        else if (k == "pass_epoch") h->passEpoch = static_cast<uint32_t>(strtoul(v.c_str(), nullptr, 10));
        else if (k == "mission_name") {  // UTF-8: cut at a character boundary
            size_t n = std::min(v.size(), sizeof(h->missionName) - 1);
            if (n < v.size())
                while (n > 0 && (static_cast<unsigned char>(v[n]) & 0xC0) == 0x80) --n;
            memcpy(h->missionName, v.data(), n);
            h->missionName[n] = 0;
        }
        else if (k == "passes") {  // comes before the passN lines
            memset(h->passes, 0, sizeof(h->passes));
            memset(h->passFlags, 0, sizeof(h->passFlags));
            h->passCount = std::min<uint32_t>(static_cast<uint32_t>(atoi(v.c_str())), _countof(h->passes));
        } else if (k.size() == 5 && k.rfind("pass", 0) == 0 && k[4] >= '1' && k[4] <= '4') {
            // "id|active|result|range|type|board|pilot|carrier[|case|callsign|grade|wire[|flags]]"
            auto& ps = h->passes[k[4] - '1'];
            std::vector<std::string> f;
            size_t s0 = 0, bar;
            while ((bar = v.find('|', s0)) != std::string::npos) {
                f.push_back(v.substr(s0, bar - s0));
                s0 = bar + 1;
            }
            f.push_back(v.substr(s0));
            if (f.size() >= 13) h->passFlags[k[4] - '1'] = static_cast<uint32_t>(atoi(f[12].c_str()));  // v8: whose pass
            if (f.size() >= 12) {  // v4: case | callsign | grade | wire
                ps.caseNo = atoi(f[8].c_str());
                strncpy_s(ps.callsign, f[9].c_str(), _TRUNCATE);
                strncpy_s(ps.grade, f[10].c_str(), _TRUNCATE);
                strncpy_s(ps.wire, f[11].c_str(), _TRUNCATE);
            }
            if (f.size() >= 8) {
                ps.id = static_cast<uint32_t>(strtoul(f[0].c_str(), nullptr, 10));
                ps.active = atoi(f[1].c_str()) ? 1 : 0;
                ps.result = atoi(f[2].c_str());
                ps.range = static_cast<float>(atof(f[3].c_str()));
                strncpy_s(ps.type, f[4].c_str(), _TRUNCATE);
                strncpy_s(ps.board, f[5].c_str(), _TRUNCATE);
                strncpy_s(ps.pilot, f[6].c_str(), _TRUNCATE);
                strncpy_s(ps.carrier, f[7].c_str(), _TRUNCATE);
            }
        }
    }
    MemoryBarrier();
    InterlockedIncrement(&y.seq);
}

// Cue panel rows: "status|type|board|pilot|fuelLb|x|z|yaw" lines and one "case3=a|b|c" line.
void ShareSetCue(const char* text) {
    lsoshare::Header* h = g.header;
    if (!h || !text) return;
    auto& c = h->cue;
    uint32_t n = 0;
    memset(c.case3, 0, sizeof(c.case3));
    const char* p = text;
    while (*p) {
        const char* e = strchr(p, '\n');
        const std::string line(p, e ? static_cast<size_t>(e - p) : strlen(p));
        p = e ? e + 1 : p + strlen(p);
        std::vector<std::string> f;
        size_t s0 = line.rfind("case3=", 0) == 0 ? 6 : 0, k;
        while ((k = line.find('|', s0)) != std::string::npos) {
            f.push_back(line.substr(s0, k - s0));
            s0 = k + 1;
        }
        f.push_back(line.substr(s0));
        if (line.rfind("case3=", 0) == 0) {
            for (size_t i = 0; i < f.size() && i < 3; ++i) strncpy_s(c.case3[i], f[i].c_str(), _TRUNCATE);
            continue;
        }
        if (f.size() < 8 || n >= _countof(c.rows)) continue;
        auto& r = c.rows[n++];
        r.status = atoi(f[0].c_str());
        strncpy_s(r.type, f[1].c_str(), _TRUNCATE);
        strncpy_s(r.board, f[2].c_str(), _TRUNCATE);
        strncpy_s(r.pilot, f[3].c_str(), _TRUNCATE);
        r.fuelLb = static_cast<float>(atof(f[4].c_str()));
        r.x = static_cast<float>(atof(f[5].c_str()));
        r.z = static_cast<float>(atof(f[6].c_str()));
        r.yaw = static_cast<float>(atof(f[7].c_str()));
    }
    c.count = n;
    MemoryBarrier();
    InterlockedIncrement(&c.seq);
}

// The viewer's latest carrier request: sequence number and unit name ("" if none / no mapping).
long ShareReadControl(std::string* name) {
    lsoshare::Header* h = g.header;
    if (!h) return 0;
    const long seq = h->control.selectSeq;
    MemoryBarrier();
    char tmp[sizeof(h->control.selectName) + 1] = {};
    memcpy(tmp, h->control.selectName, sizeof(h->control.selectName));
    *name = tmp;
    return seq;
}

// Extra cameras: "name|ldr|hdr|available|enabled|aspect" lines.
void ShareSetCameras(const char* text) {
    lsoshare::Header* h = g.header;
    if (!h || !text) return;
    uint32_t n = 0;
    const char* p = text;
    while (*p && n < _countof(h->cameras)) {
        const char* e = strchr(p, '\n');
        const std::string line(p, e ? static_cast<size_t>(e - p) : strlen(p));
        p = e ? e + 1 : p + strlen(p);
        std::vector<std::string> f;
        size_t s0 = 0, bar;
        while ((bar = line.find('|', s0)) != std::string::npos) {
            f.push_back(line.substr(s0, bar - s0));
            s0 = bar + 1;
        }
        f.push_back(line.substr(s0));
        if (f.size() < 6) continue;
        auto& c = h->cameras[n++];
        strncpy_s(c.name, f[0].c_str(), _TRUNCATE);
        strncpy_s(c.ldr, f[1].c_str(), _TRUNCATE);
        strncpy_s(c.hdr, f[2].c_str(), _TRUNCATE);
        c.available = atoi(f[3].c_str()) ? 1 : 0;
        c.enabled = atoi(f[4].c_str()) ? 1 : 0;
        c.aspect = static_cast<float>(atof(f[5].c_str()));
    }
    for (uint32_t i = n; i < _countof(h->cameras); ++i) memset(&h->cameras[i], 0, sizeof(h->cameras[i]));
    MemoryBarrier();
    h->cameraCount = n;
}

// Camera views (v6): the viewer's request for camera i, and the hook's list of views and the one shown.
long ShareReadCameraView(int i, int* request, int* trackMode) {
    lsoshare::Header* h = g.header;
    *request = -1;
    if (trackMode) *trackMode = 0;
    if (!h || i < 0 || i >= lsoshare::kMaxCameras) return 0;
    const long seq = h->views[i].requestSeq;
    MemoryBarrier();
    *request = h->views[i].request;
    if (trackMode) *trackMode = h->ptz[i].trackMode;
    return seq;
}

void ShareSetCameraViews(int i, int current, const char* names) {
    lsoshare::Header* h = g.header;
    if (!h || i < 0 || i >= lsoshare::kMaxCameras || !names) return;
    auto& v = h->views[i];
    uint32_t n = 0;
    for (const char* p = names; *p && n < static_cast<uint32_t>(lsoshare::kMaxViews);) {
        const char* e = strchr(p, '|');
        const size_t len = e ? static_cast<size_t>(e - p) : strlen(p);
        // "NAME\tH": flags after a tab - 'H' = a hangar position, kept in the name's last byte (share_protocol.h)
        const char* tab = static_cast<const char*>(memchr(p, '\t', len));
        const size_t nameLen = tab ? static_cast<size_t>(tab - p) : len;
        const bool hangar = tab && memchr(tab, 'H', len - nameLen) != nullptr;
        memset(v.names[n], 0, sizeof(v.names[n]));
        memcpy(v.names[n], p, std::min(nameLen, sizeof(v.names[n]) - 2));
        if (hangar) v.names[n][sizeof(v.names[n]) - 1] = lsoshare::kViewHangar;
        ++n;
        p = e ? e + 1 : p + len;
    }
    for (uint32_t k = n; k < static_cast<uint32_t>(lsoshare::kMaxViews); ++k) memset(v.names[k], 0, sizeof(v.names[k]));
    v.current = n ? current : -1;
    MemoryBarrier();
    v.count = n;
}

// The viewer's camera request: sequence number and mask (bit i = cameras[i] wanted).
long ShareReadCameraControl(unsigned* mask) {
    lsoshare::Header* h = g.header;
    *mask = 0;
    if (!h) return 0;
    const long seq = h->control.cameraSeq;
    MemoryBarrier();
    *mask = h->control.cameraMask;
    return seq;
}

bool ShareReadPtz(int i, float* pan, float* tilt, float* zoom, long* resetSeq, int* pictureType) {
    lsoshare::Header* h = g.header;
    *pan = *tilt = *zoom = 0;
    *resetSeq = 0;
    *pictureType = 0;
    if (!h || i < 0 || i >= lsoshare::kMaxCameras) return false;
    const auto& c = h->ptz[i];
    *resetSeq = c.resetSeq;
    *pictureType = c.pictureType >= 0 && c.pictureType <= 15 ? c.pictureType : 0;
    const ULONGLONG tick = static_cast<ULONGLONG>(c.tick);
    if (tick && GetTickCount64() - tick < 300) {
        auto clamp = [](float v) { return v != v ? 0.0f : v < -1 ? -1.0f : v > 1 ? 1.0f : v; };
        *pan = clamp(c.pan), *tilt = clamp(c.tilt), *zoom = clamp(c.zoom);
    }
    return true;
}

void ShareWritePtzState(int i, float panDeg, float tiltDeg, float zoom, bool manual) {
    lsoshare::Header* h = g.header;
    if (!h || i < 0 || i >= lsoshare::kMaxCameras) return;
    auto& s = h->ptzState[i];
    s.panDeg = panDeg, s.tiltDeg = tiltDeg, s.zoom = zoom;
    s.manual = manual ? 1 : 0;
}

// v9: camera i's pose (the hook's 'set'), written only when it changed (seq + 1 after).
void ShareSetCameraPose(int i, const char* name, const double* pos, const double* look, double fovDeg) {
    lsoshare::Header* h = g.header;
    if (!h || i < 0 || i >= lsoshare::kMaxCameras || !name) return;
    lsoshare::Header::CameraPose p = {};
    strncpy_s(p.name, name, _TRUNCATE);
    for (int k = 0; k < 3; ++k) p.pos[k] = static_cast<float>(pos[k]), p.look[k] = static_cast<float>(look[k]);
    p.fovDeg = static_cast<float>(fovDeg);
    auto& d = h->camPose[i];
    if (d.valid && !memcmp(d.name, p.name, sizeof(p.name)) && !memcmp(d.pos, p.pos, sizeof(p.pos)) && !memcmp(d.look, p.look, sizeof(p.look)) &&
        d.fovDeg == p.fovDeg)
        return;
    memcpy(d.name, p.name, sizeof(p.name));
    memcpy(d.pos, p.pos, sizeof(p.pos));
    memcpy(d.look, p.look, sizeof(p.look));
    d.fovDeg = p.fovDeg;
    MemoryBarrier();
    d.valid = 1;
    InterlockedIncrement(&d.seq);
}

void ShareSetMishap(const char* text) {
    lsoshare::Header* h = g.header;
    if (!h || !text) return;
    char tmp[sizeof(h->mishap.text)] = {};
    strncpy_s(tmp, text, _TRUNCATE);
    memcpy(h->mishap.text, tmp, sizeof(tmp));
    MemoryBarrier();
    InterlockedIncrement(&h->mishap.seq);
}

// Shares another DCS render target through slot `slot` (the old one is released; consumers reopen).
std::string ShareRetarget(int slot, const char* name) {
    Exclusive lock;
    if (!g.running || !name) return "sharing not running";
    for (auto& t : g.targets) {
        if (t.slot != slot) continue;
        if (t.name == name) return {};
        PublishInactive(t);
        SafeRelease(t.src);
        ReleaseShared(t);
        Log("SHARE slot %d: %s -> %s", slot, t.name.c_str(), name);
        t.name = name;
        t.nextCheck = 0;
        t.failures = 0;
        strncpy_s(g.header->targets[slot].name, name, _TRUNCATE);
        return {};
    }
    return "no such slot";
}

long long ShareFrameCount() { return g_frameCount; }

// 1 / 0: DCS has a render target of this name (as of the render thread's last check, at most a second
// ago); -1 = not known yet. The first call for a name only registers it.
int ShareTargetExists(const char* name) {
    Exclusive lock;
    if (!g.running || !name) return -1;
    for (const auto& p : g_probes)
        if (p.name == name) return p.exists;
    if (g_probes.size() < 16) g_probes.push_back(ProbeName{name, -1});
    return -1;
}

std::string ShareStatus() {
    Exclusive lock;
    return StatusLocked();
}

void ShareStop() {
    Exclusive lock;
    StopLocked();
}

}  // namespace lsodiag
