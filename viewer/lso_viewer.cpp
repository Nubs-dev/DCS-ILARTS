// DCS ILARTS viewer (ILARTS.exe): shows DCS's PLAT camera and LSO station screens (shared by ILARTS.dll), records them.
//
//   ILARTS.exe [feed 0-2] [--auto|--no-auto] [--mode pipeline|levels|off] [--stops N] [--gamma G] [--debug]
//
// Interface: the console (console.inc: ui/ilarts-console.js in a WebView2), the only one. Without the WebView2 runtime
// ILARTS says so, offers Microsoft's download page and closes; if WebView2 fails while running, it says so and closes.
// Keys: [keys] in ILARTS.ini (kKeyActions; the settings dialog rebinds them), the source keys 1-9 (monitor A) and
// the numeric keypad's + / - (exposure). In a replay: Space pause, Left/Right 2 s (paused: one frame), , and . one
// frame, Up/Down speed, Home start, S save the replay, Esc back to live. Ctrl+Shift+F9 record, Ctrl+Shift+F10
// instant replay, Ctrl+Shift+F11 save the black box and Ctrl+Shift+F8 still also work while DCS has the keyboard.
// Pass recordings are named by [video] filename (date, callsign, case, the LSO's grade). Every extra camera
// that is rendering is recorded alongside on its own channel. The black box (live.h) keeps the last minutes
// as H.264 in memory and saves them on a mishap reported by the hook (Header::mishap); the same encoder feeds
// the RTMP stream (rtmp.h, [stream] url / key).
// Videos (MP4, H.264) go to the "DCS ILARTS" folder in Videos, or to "folder" in the [video] section of
// ILARTS.ini. Automatic mode records every approach the PLAT sees (the hook reports passes), from just
// before the aircraft comes into the picture to a few seconds after the trap, bolter or wave-off, named
// after the aircraft and the result. [video] settings: width, height, fps, replay_seconds, preroll, postroll,
// bitrate_kbps, quality (replay buffer JPEG 0-100), encoder (auto / hardware / software).
// Settings persist in ILARTS.ini next to the executable.
//
// Sharing protocol: src/share_protocol.h. Every shared target is opened and kept in sync (copied into
// the viewer's own texture under the keyed mutex, key 0, short hold). When the DLL reports that the running
// DCS is not compatible (Header::compat), the console's monitors say so instead.
//
// PLAT processing (feeds 1 and 2 share it):
//   Pipeline - rebuilt from hdrBufferMFD32 (clean scene-linear HDR camera image):
//       k   = DCS's exposure, measured live (median of -ln(1-lso)/hdr over mid-tones)
//       cam = levels(1 - exp(-k * 2^stops * lum(hdr)))
//       sym = DCS symbology mask from LSOFRAME vs the predicted camera value
//       out = lerp(cam, lso, sym)
//   Levels   - levels applied to DCS's LSOFRAME directly
//   Off      - DCS output as-is
// Feeds 2 and 3 without a station visit: DCS has no LSOFINALTARGET / LSOSCREEN2; the viewer draws the station
// screen and the cue panel itself (plat_station.h) from the values the hook publishes (Header::station, ::cue)
// and composites the PLAT into the station screen the same way.
// Feed 2 (LSOFINALTARGET, LSO station screen): the station composites LSOFRAME into the rectangle
// x 192..831, y 150..629 of its 1024x768 frame with tint (R,G,B) = (0, 1.005, 0.752) (measured
// 2026-11-08). The viewer replaces that rectangle with tint * processed PLAT and leaves the rest.
// Auto-levels: GPU histogram (1024 sqrt-scale bins) of the camera image (symbology excluded), async
// readback, 0.5th / 99th percentile, max gain 25x, 0.4 s smoothing.

#include <winsock2.h>  // before windows.h (rtmp.h)

#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW (excluded by WIN32_LEAN_AND_MEAN)

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <objbase.h>
#include <shlobj.h>    // SHGetKnownFolderPath, SHCreateDirectoryExW
#include <shobjidl.h>  // IFileDialog (settings: folder, export, import)
#include <wincodec.h>  // PNG encoder
#include <dcomp.h>     // the console: DirectComposition
#include <shlwapi.h>   // SHCreateStreamOnFileEx (console tests)
#include <mfvirtualcamera.h>  // virtual cameras (outputs.inc; the API itself is looked up at run time: Windows 11)
#include <sddl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../src/share_protocol.h"
#include "../src/bench_protocol.h"
#include "ilarts_version.h"  // <build>/generated, from src/version.h.in
// A development build (built without -DDCS_ILARTS_VERSION): the only one with the Developer settings (camera tuning).
inline bool DevBuild() { return wcscmp(ILARTS_VERSION_W, L"0.0.0") == 0; }
#include "plat_station.h"
#include "video.h"
#include "live.h"
#include "rtmp.h"
#include "spout_out.h"
#include "../vcam/vcam_shared.h"
#include "WebView2.h"  // third_party/webview2 (the console interface, console.inc)

namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// The product name everywhere the user sees it (title, videos folder, messages).
constexpr wchar_t kAppName[] = L"DCS ILARTS";

constexpr UINT kBins = 1024;
constexpr UINT kRing = 3;
constexpr float kLog2KMin = 0.0f, kLog2KMax = 16.0f;
constexpr int kResizeBorder = 6;  // px along the window edges that resize it
// LSO station composite (LSOFINALTARGET 1024x768): PLAT rectangle and tint, measured 2026-11-08.
constexpr float kStationW = 1024, kStationH = 768;
constexpr float kRectX = 192, kRectY = 150, kRectW = 640, kRectH = 480;
constexpr float kTintR = 0.0f, kTintG = 1.005f, kTintB = 0.752f;

const char kShader[] = R"(
Texture2D texMain : register(t0);   // selected feed
Texture2D texHdr  : register(t1);   // hdrBufferMFD32
Texture2D texLso  : register(t2);   // LSOFRAME
Texture2D texOvl  : register(t3);   // PLAT overlay drawn by the viewer
SamplerState smp : register(s0);
cbuffer Params : register(b0) {
    float black; float white; float contrast; float brightness;
    float invGamma; float view; float proc; float kDcs;     // view: 0 plain, 1 PLAT, 2 station; proc: 0 off, 1 levels, 2 pipeline
    float expMul; float symOn; float2 pad;
    float4 rect;    // PLAT rectangle in the station frame (normalised x, y, w, h)
    float4 tint;    // station tint
    float4 dcsAdj;  // in-DCS LSO_PLAT.hlsl settings: stops, gamma, contrast, brightness
    float4 dcsAdj2; // symbology-keep level, enabled (1 = the hook is installed and LSO_ENABLE 1)
    float4 extra;   // x: station drawn by the viewer (texMain = premultiplied layer), y: PLAT overlay in the
                    // station's PLAT rectangle, z: its ink (1 white / 0 black), w: 1 = no LDR picture (an extra
                    // camera that only has its HDR image: exposure from the HDR image alone, no symbology)
    float4 cctv;    // CCTV look. x: strength (0 = off), y: video frame number (noise seed), z: height of the
                    // camera picture in pixels, w: 1 = anti-aliasing of the camera picture
    float4 cctv2;   // amounts at strength 1: grain / static, scan lines, lens distortion, vignette
};
static const float3 LUM = float3(0.2126, 0.7152, 0.0722);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float levels(float x) {
    x = (x - black) / max(white - black, 1e-4);
    x = x * contrast + brightness;
    return pow(saturate(x), invGamma);
}
// DCS symbology coverage c (0..1), unmixed from LSOFRAME against pred, the value DCS's camera image
// has without symbology. Anti-aliased edges get partial coverage instead of being cut off by a threshold.
//   Day   (setNightMode(false), black ink - capture 2026-11-09 06:14:25): lso = pred*(1-c)
//   Night (setNightMode(true), bright ink): lso = pred*(1-c) + ink*c, ink ~ 1; also gated on absolute
//         brightness (robust when the in-DCS LSO_PLAT shader has already lifted LSOFRAME's mid-tones).
// A small dead zone absorbs prediction error. dark = 1 for black ink.
// Undoes the in-DCS PLAT adjustment (deploy/shaders LSO_PLAT.hlsl, LsoAdjust) so LSOFRAME can be compared
// with DCS's stock tone curve; identity when that shader is not installed / disabled.
float unadjust(float c) {
    if (dcsAdj2.y < 0.5 || c >= dcsAdj2.x) return c;
    c = (c - 0.5 - dcsAdj.w) / max(dcsAdj.z, 1e-3) + 0.5;
    c = pow(saturate(c), dcsAdj.y);
    float t = pow(c, 2.2);
    t = 1 - pow(saturate(1 - t), exp2(-dcsAdj.x));
    return pow(t, 1 / 2.2);
}
float symCover(float lso, float h, out float dark) {
    lso = unadjust(lso);
    float pred = 1 - exp(-kDcs * h);
    float d = lso - pred;
    const float dead = 0.05;
    dark = 0;
    if (d > 0.08) return saturate((d - 0.08) / max(1 - pred - 0.08, 0.05)) * smoothstep(0.5, 0.8, lso);
    if (d < -dead && pred > 0.08) {
        dark = 1;
        return saturate((-d - dead) / max(pred - dead, 0.03));
    }
    return 0;
}
float symMask(float lso, float h) {
    float dark;
    return symCover(lso, h, dark);
}
// DCS's own in-shader adjustment (LsoAdjust), so the rebuilt camera image looks like DCS's PLAT at 0 EV.
float adjustDcs(float c) {
    if (dcsAdj2.y < 0.5) return c;
    float t = pow(saturate(c), 2.2);
    t = 1 - pow(saturate(1 - t), exp2(dcsAdj.x));
    c = pow(pow(t, 1 / 2.2), 1 / dcsAdj.y);
    return saturate((c - 0.5) * dcsAdj.z + 0.5 + dcsAdj.w);
}
float camera(float h) { return adjustDcs(1 - exp(-kDcs * expMul * h)); }

// ---- CCTV look: what a deck camera and its analogue video chain do to the picture. Kept subtle.
float hash12(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}
// Lens: slight barrel distortion (the corners stay where they are, the centre is magnified a little).
float2 cctvLens(float2 uv) {
    float k = 0.10 * cctv2.z * cctv.x;
    if (k <= 0) return uv;
    float2 d = uv - 0.5;
    return 0.5 + d * (1 + k * dot(d, d)) / (1 + k * 0.5);
}
// Signal: vignette, scan lines, noise (stronger in the dark), a few streaks of static, a faint hum bar.
float cctvSignal(float v, float2 uv) {
    float s = cctv.x;
    if (s <= 0) return v;
    float2 d = uv - 0.5;
    v *= 1 - 0.40 * cctv2.w * s * dot(d, d);
    float lines = clamp(cctv.z / 3.0, 60.0, 262.0);   // a field's lines, never finer than 3 pixels each
    float depth = min(0.14 * cctv2.y * s, 0.9);       // the lines darken, the gain keeps the average brightness
    v *= (1 - depth * (0.5 - 0.5 * cos(6.2831853 * uv.y * lines))) / (1 - 0.5 * depth);
    float frame = cctv.y;
    float2 cell = floor(uv * float2(lines * 2.6667, lines * 2.0));
    float n = hash12(cell + frame * float2(37.0, 17.0)) + hash12(cell.yx * 1.7 + frame * float2(11.0, 53.0)) - 1.0;
    v += n * 0.045 * cctv2.x * s * (0.35 + 0.65 * (1 - v));
    float row = floor(uv.y * lines);
    if (hash12(float2(row, frame * 0.731)) > 0.9970) {
        float x0 = hash12(float2(row * 1.3, frame));
        float t = (uv.x - x0) / (0.02 + 0.08 * hash12(float2(frame, row)));
        if (t > 0 && t < 1) v += 0.30 * cctv2.x * s * (1 - t) * hash12(float2(floor(uv.x * 400), frame));
    }
    v += 0.010 * cctv2.x * s * sin(6.2831853 * (uv.y * 1.3 + frame * 0.0113));
    return saturate(v);
}

// ---- Anti-aliasing of the camera picture (DCS renders these views without any): FXAA on the tone-mapped
// camera value, before levels and the CCTV look. src: 0 = the HDR image through the camera curve, 1 = the LDR
// picture. cctv.w = 1 switches it on.
float camTap(float2 uv, float src) {
    return src < 0.5 ? camera(dot(texHdr.SampleLevel(smp, uv, 0).rgb, LUM)) : dot(texLso.SampleLevel(smp, uv, 0).rgb, LUM);
}
float camAA(float2 uv, float src) {
    float m = camTap(uv, src);
    if (cctv.w < 0.5) return m;
    float w, h;
    if (src < 0.5) texHdr.GetDimensions(w, h); else texLso.GetDimensions(w, h);
    float2 px = 1.0 / float2(w, h);
    float nw = camTap(uv + float2(-0.5, -0.5) * px, src), ne = camTap(uv + float2(0.5, -0.5) * px, src);
    float sw = camTap(uv + float2(-0.5, 0.5) * px, src), se = camTap(uv + float2(0.5, 0.5) * px, src);
    float lo = min(m, min(min(nw, ne), min(sw, se))), hi = max(m, max(max(nw, ne), max(sw, se)));
    if (hi - lo < max(0.02, hi * 0.08)) return m;
    float2 dir = float2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * 0.03125, 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -6.0, 6.0) * px;
    float a = 0.5 * (camTap(uv + dir * (1.0 / 3.0 - 0.5), src) + camTap(uv + dir * (2.0 / 3.0 - 0.5), src));
    float b = a * 0.5 + 0.25 * (camTap(uv + dir * -0.5, src) + camTap(uv + dir * 0.5, src));
    return (b < lo || b > hi) ? a : b;
}

float platValue(float2 uv) {
    bool noLdr = extra.w > 0.5;
    float2 cuv = cctvLens(uv);
    // DCS's symbology is taken from the same (lens-distorted) point as the camera picture: sampled at uv instead,
    // dark aircraft edges that the mask reads as black ink landed beside the distorted aircraft (a black copy
    // offset toward the centre with the CCTV look on). Like text in a real CCTV signal, it bends with the picture.
    float lso = noLdr ? 0 : dot(texLso.Sample(smp, cuv).rgb, LUM);
    if (proc > 1.5) {
        float h = dot(texHdr.Sample(smp, cuv).rgb, LUM);
        float cv = camAA(cuv, 0);
        if (cctv.x > 0) {   // limited bandwidth: a little softer along the scan lines
            float w, hh;
            texHdr.GetDimensions(w, hh);
            float2 dx = float2(0.9 * min(cctv.x, 1.5) / w, 0);
            cv = 0.5 * cv + 0.25 * (camTap(cuv - dx, 0) + camTap(cuv + dx, 0));
        }
        float dark = 0;
        float c = noLdr ? 0 : symCover(lso, h, dark) * symOn;
        float cam = cctvSignal(levels(cv), uv);
        // black ink: composite black with coverage c over the adjusted camera image (exact for edges);
        // bright ink: keep DCS's own ink colour
        return dark > 0.5 ? cam * (1 - c) : lerp(cam, lso, c);
    }
    if (proc > 0.5) return cctvSignal(levels(camAA(cuv, 1)), uv);
    return lso;
}

// PLAT text overlay drawn by the viewer (texOvl: r = ink coverage, g = halo coverage; pad.x = ink 1 white / 0 black).
// Premultiplied output; the halo is black (DCS's blurColor).
float4 psOverlay(VSOut i) : SV_Target {
    float4 o = texOvl.Sample(smp, i.uv);
    float ink = o.r * symOn;
    float halo = o.g * symOn;
    return float4((pad.x * ink).xxx, saturate(ink + halo));
}

// Replay frames (already processed) and the REC / REPLAY badges (premultiplied).
float4 psCopy(VSOut i) : SV_Target { return float4(texMain.Sample(smp, i.uv).rgb, 1); }
float4 psBadge(VSOut i) : SV_Target { return texMain.Sample(smp, i.uv); }

float4 ps(VSOut i) : SV_Target {
    if (view > 0.5 && view < 1.5) return float4(platValue(i.uv).xxx, 1);
    if (view > 1.5 && (proc > 0.5 || extra.x > 0.5)) {
        float2 p = (i.uv - rect.xy) / rect.zw;
        float4 st = texMain.Sample(smp, i.uv);
        if (all(p >= 0) && all(p < 1)) {
            float v = platValue(p);
            if (extra.y > 0.5) {
                float4 o = texOvl.Sample(smp, p);
                float ink = o.r * symOn, halo = o.g * symOn;
                v = v * (1 - saturate(ink + halo)) + extra.z * ink;
            }
            float3 c = tint.rgb * v;
            if (extra.x > 0.5) c = c * (1 - st.a) + st.rgb;
            return float4(c, 1);
        }
        if (extra.x > 0.5) return float4(st.rgb, 1);
    }
    return float4(texMain.Sample(smp, i.uv).rgb, 1);
}

// Histogram of the camera image (pre-levels), symbology excluded. Runs over LSOFRAME's pixels.
RWBuffer<uint> hist : register(u0);
[numthreads(16, 16, 1)]
void csCam(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    texLso.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    bool noLdr = extra.w > 0.5;   // texLso is then the HDR image itself (for its size)
    float lso = noLdr ? 0 : dot(texLso.Load(int3(id.xy, 0)).rgb, LUM);
    float2 uv = (id.xy + 0.5) / float2(w, h);
    float v;
    if (proc > 1.5) {
        float hh = dot(texHdr.SampleLevel(smp, uv, 0).rgb, LUM);
        if (!noLdr && symMask(lso, hh) > 0.5) return;
        v = camera(hh);
    } else {
        if (lso >= 0.9) return;
        v = lso;
    }
    InterlockedAdd(hist[min(BINS - 1, (uint)(sqrt(saturate(v)) * BINS))], 1);
}

// Histogram of log2(k) = log2(-ln(1-lso)/hdr) over mid-tone pixels: DCS's exposure.
[numthreads(16, 16, 1)]
void csK(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    texLso.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    // Without an LDR picture: the exposure that puts each pixel at 0.45; the median then exposes the lit
    // part of the picture for a middle grey, as a camera's automatic exposure would.
    float lso = extra.w > 0.5 ? 0.45 : unadjust(dot(texLso.Load(int3(id.xy, 0)).rgb, LUM));
    float hh = dot(texHdr.SampleLevel(smp, (id.xy + 0.5) / float2(w, h), 0).rgb, LUM);
    if (lso < 0.08 || lso > 0.7 || hh < 1e-5) return;
    float t = saturate((log2(-log(1 - lso) / hh) - KMIN) / (KMAX - KMIN));
    InterlockedAdd(hist[min(BINS - 1, (uint)(t * BINS))], 1);
}
)";

struct Params {
    float black, white, contrast, brightness, invGamma, view, proc, kDcs, expMul, symOn, pad[2];
    float rect[4], tint[4];
    float dcsAdj[4], dcsAdj2[4];
    float extra[4];
    float cctv[4], cctv2[4];
};

enum class Proc { Off = 0, Levels = 1, Pipeline = 2 };

struct Settings {
    float brightness = 0.0f, contrast = 1.0f, gamma = 1.0f, stops = 0.0f;
    bool autoLevels = false, symbology = true;
    bool enabled = true;  // master switch: false = DCS image untouched (mode and values kept)
    Proc proc = Proc::Pipeline;
};

struct Histogram {
    ID3D11Buffer* buf = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11Buffer* staging[kRing] = {};
    bool pending[kRing] = {};
    UINT write = 0;
    bool create(ID3D11Device* dev) {
        D3D11_BUFFER_DESC hb{kBins * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0};
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kBins;
        D3D11_BUFFER_DESC sb{kBins * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        if (FAILED(dev->CreateBuffer(&hb, nullptr, &buf)) || FAILED(dev->CreateUnorderedAccessView(buf, &ud, &uav))) return false;
        for (auto*& s : staging)
            if (FAILED(dev->CreateBuffer(&sb, nullptr, &s))) return false;
        return true;
    }
    void release() {
        for (auto*& s : staging) SafeRelease(s);
        SafeRelease(uav);
        SafeRelease(buf);
        reset();
    }
    void reset() {
        for (bool& p : pending) p = false;
    }
    void dispatch(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, UINT w, UINT h) {
        if (pending[write]) return;
        const UINT zero[4] = {};
        ctx->ClearUnorderedAccessViewUint(uav, zero);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
        ID3D11UnorderedAccessView* none = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
        ctx->CopyResource(staging[write], buf);
        pending[write] = true;
        write = (write + 1) % kRing;
    }
    bool poll(ID3D11DeviceContext* ctx, UINT* out) {
        bool got = false;
        for (UINT k = 0; k < kRing; ++k) {
            UINT slot = (write + k) % kRing;
            if (!pending[slot]) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (ctx->Map(staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) break;
            memcpy(out, m.pData, kBins * 4);
            ctx->Unmap(staging[slot], 0);
            pending[slot] = false;
            got = true;
        }
        return got;
    }
};

// Exposure and level measurements of one camera picture: the PLAT (index 0) or extra camera i (index 1 + i).
struct Analysis {
    Histogram histCam, histK;
    float autoBlack = 0.0f, autoWhite = 1.0f, targetBlack = 0.0f, targetWhite = 1.0f;
    bool haveAutoTarget = false;
    float kDcs = 100.0f, kTarget = 100.0f;
    bool haveK = false;
    double lastT = 0;
};
// Cameras as sources (logical cameras): ISLAND and each deck position. Each has its own levels (Analysis).
constexpr int kMaxLogical = 16;
constexpr int kAnalyses = 1 + kMaxLogical;

struct Target {
    char name[32] = {};
    LONG generation = -1;
    ID3D11Texture2D* shared = nullptr;
    IDXGIKeyedMutex* mutex = nullptr;
    ID3D11Texture2D* local = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0;
    LONG64 lastFrame = -1;
    bool fresh = false;
    float fps = 0;  // new pictures per second (console: the previews' readouts)
    int fpsCount = 0;
    ULONGLONG fpsStart = 0;
    void close() {
        SafeRelease(srv);
        SafeRelease(local);
        SafeRelease(mutex);
        SafeRelease(shared);
        generation = -1;
        lastFrame = -1;
        fps = 0;
        fpsCount = 0;
        fpsStart = 0;
    }
};

struct App {
    HWND hwnd = nullptr;
    HANDLE mapping = nullptr;
    const lsoshare::Header* header = nullptr;
    lsoshare::Header* headerW = nullptr;  // same view when it could be mapped writable (carrier choice)
    LUID deviceLuid{};
    ID3D11Device* dev = nullptr;
    ID3D11Device1* dev1 = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ComputeShader* csCam = nullptr;
    ID3D11ComputeShader* csK = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11Buffer* params = nullptr;
    Analysis an[kAnalyses];
    Target targets[lsoshare::kMaxTargets];
    int selected = 0;
    LONG64 framesThisSecond = 0;
    ULONGLONG secondStart = 0;
    double sourceFps = 0;
    Settings set;
    // in-DCS PLAT adjustment (LSO_PLAT.hlsl), re-read every few seconds
    float dcsAdj[4] = {0, 1, 1, 0}, dcsKeep = 0.97f;
    bool dcsAdjOn = false;
    ULONGLONG dcsAdjRead = 0;
    LARGE_INTEGER qpf{};
    std::wstring source = L"waiting for DCS";
    std::wstring iniPath;
    int presentedWithImage = 0;  // frames presented with a picture (the tests wait for the first)
    bool debug = false;  // --debug / [viewer] debug=1: WebView2 dev tools, extra detail in the source text
} app;

// Video recording and instant replay (video.h). Each recorded frame is the picture as shown, drawn again
// into a fixed-size canvas, read back through a small ring of staging textures and handed to the engine.
// One recorded picture: its canvas on the GPU, the read-back ring, the engine that writes its files and the
// live encoder (black box, stream). The main channel records the picture as shown; the others record a fixed
// source in the background (an extra camera, or the PLAT while an extra camera is shown), so every camera
// gets its own file of the same pass.
struct Channel {
    int source = -1;      // -1 = whatever is shown; else that source (0 PLAT, 3 + i extra camera i)
    std::wstring suffix;  // added to this channel's file names ("ISLAND")
    video::Engine eng;
    bool ok = false;      // engine running
    live::Live live;
    // GPU side (recreated after a device loss)
    bool gpu = false;
    ID3D11Texture2D* canvas = nullptr;
    ID3D11RenderTargetView* canvasRtv = nullptr;
    static constexpr int kStaging = 4;
    ID3D11Texture2D* staging[kStaging] = {};
    double stagingT[kStaging] = {};
    std::deque<int> inFlight;
    double nextCapture = 0, lastFrame = 0;  // lastFrame: when its picture was last drawn (it is alive)
    std::vector<uint8_t>* keep = nullptr;    // tests: the last picture read back is copied here
    std::function<void(const std::vector<uint8_t>&, int, int)> out;  // each picture read back (a virtual camera)
    std::wstring still;     // a still asked for (StillTake): the first picture drawn from stillAfter on goes to this file
    double stillAfter = 0;
};

struct RecState {
    Channel main;
    std::vector<std::unique_ptr<Channel>> extra;
    // Composite mode ([video] composite, the VTR's COMP key): recordings and stills take one 1920 x 1080 picture of
    // the sources with REC on (comp, tiled like a composite stream) instead of a file per source.
    bool composite = false;
    Channel comp;
    std::vector<int> compTiles;  // its sources, in tile order (kept while a recording or still uses it)
    bool ok = false;  // main engine running
    std::wstring folder, nameFormat;  // folder: the recordings folder (each mission has its own in it: recfiles.inc)
    double preroll = 3.0, postroll = 4.0, replayDefault = 30.0, autoMaxS = 240.0;
    bool autoMode = false;
    int autoFilter = 3;  // whose passes automatic mode records: AutoFilter ([video] auto_filter)
    // GPU objects shared by the channels and the replay (recreated after a device loss)
    bool gpu = false;
    ID3D11PixelShader* psCopy = nullptr;
    ID3D11PixelShader* psBadge = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11DeviceContext1* ctx1 = nullptr;
    ID3D11Texture2D* replayTex = nullptr;
    ID3D11ShaderResourceView* replaySrv = nullptr;
    // recordings: the manual one (key 0) and one per pass (key = pass id), any number at the same time
    struct Recording {
        bool manual = false;
        int key = 0;
        uint32_t pass = 0;
        std::wstring path, stamp, base;   // base: file name without folder and extension
        std::wstring folder;              // its mission's folder (the files stay there when they are renamed)
        std::vector<std::wstring> paths;  // files being written (the storage limit leaves them alone)
        double start = 0, stopAt = 0;
        std::vector<Channel*> extra;      // channels recording the same thing from their camera
    };
    std::vector<Recording> recs;
    // Finished pass recordings, kept for two minutes: the LSO's grade can arrive after the file was named.
    struct Named {
        uint32_t pass = 0;
        std::wstring stamp, base;
        double t = 0;
        size_t expected = 0;              // files of this pass (channels)
        std::vector<std::wstring> paths;  // as saved
    };
    std::vector<Named> named;
    // approaches reported by the hook (Header::passes), recent ones
    struct PassInfo {
        uint32_t id = 0;
        bool active = false;
        double start = 0, end = 0;
        int result = 0, caseNo = 0;
        uint32_t flags = 0;  // Header::passFlags (v8): your aircraft / section / flight
        std::string type, board, pilot, carrier, callsign, grade, wire;
    };
    std::vector<PassInfo> passes;
    bool passInit = false, passesPrimed = false;
    DWORD passPid = 0;
    uint32_t passEpoch = 0;
    // replay
    bool replay = false, paused = false;
    std::vector<video::FramePtr> frames;
    double pos = 0, speed = 1, lastTick = 0;
    int shown = -1;
    std::wstring replayLabel;
    video::Jpeg jpeg;  // UI-thread decoder
    std::vector<uint8_t> decoded;
    // a short message for the VTR display
    std::wstring status;
    ULONGLONG statusUntil = 0;
} g_rec;
video::Engine::Config g_engCfg;  // the main channel's settings (background channels: same, short replay ring)

// Whose passes automatic mode records (RecState::autoFilter), in the order the ACFT key steps through them.
enum AutoFilter { AF_OWN, AF_SECTION, AF_FLIGHT, AF_ALL, AF_COUNT };
const wchar_t* const kAutoFilterKey[AF_COUNT] = {L"own", L"section", L"flight", L"all"};        // ini, console state
const wchar_t* const kAutoFilterName[AF_COUNT] = {L"My aircraft", L"My section", L"My flight", L"All aircraft"};
const uint32_t kAutoFilterBit[AF_COUNT] = {lsoshare::kPassOwn, lsoshare::kPassSection, lsoshare::kPassFlight, 0};
int AutoFilterFromKey(const wchar_t* k) {
    for (int i = 0; i < AF_COUNT; ++i)
        if (_wcsicmp(k, kAutoFilterKey[i]) == 0) return i;
    return AF_ALL;
}
// 0 = DCS has not said (no producer, or a hook older than the DLL), 1 = you are in an aircraft, 2 = you have none.
uint32_t OwnAircraft();
// Automatic mode records this pass. Without word from the hook about your aircraft every pass counts (as before
// the filter existed); without an aircraft only ALL ACFT records.
bool AutoWantsPass(uint32_t flags) {
    if (g_rec.autoFilter == AF_ALL) return true;
    const uint32_t own = OwnAircraft();
    if (own == 0) return true;
    if (own != 1) return false;
    // your aircraft is in your section, your section in your flight (the hook sets all of them; this does not rely on it)
    if (flags & lsoshare::kPassOwn) flags |= lsoshare::kPassSection;
    if (flags & lsoshare::kPassSection) flags |= lsoshare::kPassFlight;
    return (flags & kAutoFilterBit[g_rec.autoFilter]) != 0;
}
void TestLog(const std::wstring& line);

// Text badge drawn on the video pane (REC / REPLAY): GDI into a DIB, uploaded premultiplied.
struct Badge {
    static constexpr int kW = 600, kH = 40;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    void* bits = nullptr;
    HFONT font = nullptr;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    std::wstring text;
    COLORREF color = 0;
} g_badge;

// Viewable feeds: 0-2 are fixed; 3 + i is extra camera i (Header::cameras) when DCS offers it.
// hdrBufferMFD32 is still shared and used by the Pipeline mode, but not shown.
constexpr int kFeeds = 3;
const char* kFeedTargets[kFeeds] = {"LSOFRAME", "LSOFINALTARGET", "LSOSCREEN2"};
// The PLAT is 4:3: DCS renders it with a 4:3 projection (17 deg across, aspect 1.333) into its 512 x 512 LSOFRAME
// and stretches that to 640 x 480 at its own station - the symbology too, drawn for 512 and meant to be seen
// stretched. Shown square it is 25 % too narrow. (Since 2026-11-14; before, the viewer showed the texture square.)
constexpr float kPlatAspect = 4.0f / 3.0f;

// Picture settings per picture: group 0 = the PLAT (and the LSO station screens built on it), 1 + i = extra
// camera i (each camera has its own levels). app.set holds the group of the source on program - every
// control and key acts on it -; the other groups wait in g_groupSet. Saved in [LSOFRAME] / [camera<i>_picture].
Settings g_groupSet[kAnalyses];
int g_curGroup = 0;
int GroupOf(int source) { return source >= kFeeds ? 1 + (source - kFeeds) : 0; }
const Settings& SetFor(int group) { return group == g_curGroup ? app.set : g_groupSet[group]; }
std::wstring GroupSection(int g) { return g == 0 ? std::wstring(L"LSOFRAME") : L"camera" + std::to_wstring(g - 1) + L"_picture"; }
// The program changes to a picture of another group: park the current settings, take that group's.
void UseGroup(int g) {
    if (g == g_curGroup || g < 0 || g >= kAnalyses) return;
    const Proc proc = app.set.proc;  // the processing mode is global ([viewer] mode)
    g_groupSet[g_curGroup] = app.set;
    g_curGroup = g;
    app.set = g_groupSet[g];
    app.set.proc = proc;
    app.set.symbology = true;
}

// CCTV look (shader: cctvLens / cctvSignal), on the PLAT picture and the extra cameras.
struct Cctv {
    bool on = false;
    float strength = 1.0f;                                          // the slider: 0 .. 2
    float grain = 1.0f, scan = 1.0f, distort = 1.0f, vignette = 1.0f;  // [cctv] in the ini: each effect's share
    bool recOnly = false;  // the look goes into recordings, the black box and the stream only; the window stays clean
} g_cctv;
bool g_antialias = true;  // [viewer] antialias: FXAA on the camera picture (shader camAA)
// FLIR (experimental, [experimental] flir): a camera's PTZ panel gets a FLIR key that asks DCS for its infrared
// picture (protocol v5 ptz[i].pictureType = 4). Costly in DCS frame rate, so every camera starts each session in
// its normal picture (g_flirOn is not saved).
bool g_expFlir = false;
bool g_flirOn[16] = {};  // per logical camera (kMaxLogical)

// Black box: the live encoder keeps the last minutes in memory; a mishap reported by DCS (or the Save
// button / B) writes them to a file.
struct BlackBox {
    bool on = false;
    double minutes = 5, before = 60, after = 15;
    LONG seq = 0;
    DWORD pid = 0;
    bool init = false;
    struct Pending {
        double at, t0, t1;
        std::wstring base;
    };
    std::vector<Pending> pending;
} g_bb;

// RTMP streams: up to four targets ([stream] is the first, [stream2] .. [stream4] the others: url, key, source,
// tiles), each with its own source - "a" monitor A as shown (the default: the main channel's encoder, no extra
// encode), "b" monitor B, a source ("plat", "lso1", "island" ...), or "composite": up to four of those ([streamN]
// tiles = "a,b") in one 1920 x 1080 picture, arranged by their number (one whole, two side by side, three or four
// in a 2 x 2 grid). Every source but "a" costs another encode on the graphics card and another upload.
// [stream] bitrate_kbps (one source), composite_kbps and audio are for all of them. (Until 2026-11-15: one stream,
// of monitor A.)
constexpr int kStreams = 4;
constexpr int kCompositeW = 1920, kCompositeH = 1080, kMaxTiles = 4;
struct StreamTarget {
    bool on = false;
    std::string url, key;              // as started
    std::wstring source = L"a";        // as started
    std::vector<std::wstring> tiles;   // composite
    rtmp::Publisher pub;
    Channel ch;                        // its own picture and encoder: every source but "a"
    std::vector<uint8_t> snap;         // --test-stream: its last picture (written next to the test log)
    long long rateBytes = 0;           // the upload rate (StreamRate)
    double rateT = 0, rate = 0;
    bool own() const { return source != L"a"; }
    bool composite() const { return source == L"composite"; }
};
StreamTarget g_streams[kStreams];
bool g_streamAudio = true;
bool g_streamSnap = false;  // --test-stream: keep each stream's last picture
int g_liveBitrate = 2500000;       // black box and stream
int g_compositeBitrate = 6000000;  // a composite stream
std::wstring StreamSection(int n) { return n ? L"stream" + std::to_wstring(n + 1) : std::wstring(L"stream"); }
std::wstring StreamIni(int n, const wchar_t* key, const wchar_t* def = L"") {
    wchar_t b[512] = {};
    GetPrivateProfileStringW(StreamSection(n).c_str(), key, def, b, 512, app.iniPath.c_str());
    return b;
}
bool StreamConfigured(int n) { return !StreamIni(n, L"url").empty(); }
bool StreamsOn() {
    for (const auto& t : g_streams)
        if (t.on) return true;
    return false;
}
std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c);
    return s;
}

// Extra cameras: which ones this viewer asked DCS to render. A camera is only remembered in the ini once it
// has delivered pictures for 10 s, so a camera that crashed DCS is not switched on again by itself.
struct CamCtl {
    uint32_t want = 0;
    DWORD pid = 0;
    bool sent = false;
    double okSince[kMaxLogical] = {};
    bool confirmed[kMaxLogical] = {};
    int slotPlus1[kMaxLogical] = {};  // a deck position: its render slot (physical camera) + 1; 0 = none
    uint32_t testMask = 0;  // --test-cam
    ULONGLONG viewAskedAt[lsoshare::kMaxCameras] = {};  // when the view wanted was last asked for (v6 views)
    LONG seqWritten = 0;  // the cameraSeq this viewer last wrote (only it writes it: anything else = DCS cleared the block)
    // (FLIR - DCS's infrared picture type, protocol v5 ptz[i].pictureType = 4: removed on 2026-11-14 as too costly,
    // back on 2026-11-15 as an experimental feature, off unless [experimental] flir = 1: g_expFlir, g_flirOn.)
} g_cam;

// The console interface (console.inc): the page of ui/ composed over the pictures with WebView2.
struct ConsoleRect {
    float x = 0, y = 0, w = 0, h = 0;
};
constexpr int kConsoleMinW = 960, kConsoleMinH = 600;  // at 96 dpi
struct ConsoleHost {
    bool on = false;      // the console has started (ConsoleActivate)
    bool failed = false;  // WebView2 failed: ILARTS says so and closes
    bool failNoticed = false;
    bool ready = false;   // the page has loaded and asked for the state
    IDCompositionDevice* dcomp = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual *root = nullptr, *videoVisual = nullptr, *webVisual = nullptr;
    ICoreWebView2CompositionController* comp = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* web = nullptr;
    HCURSOR cursor = nullptr;
    bool tracking = false;                    // mouse-leave tracking armed
    std::map<std::wstring, ConsoleRect> rects;  // the page's monitors and title bar, CSS pixels
    float dpr = 1.0f;                         // CSS pixel -> window pixel
    std::wstring lastState;
    ULONGLONG lastPush = 0;
    ID3D11ShaderResourceView* canvasSrv = nullptr;  // the main recording canvas (replay monitor)
    ID3D11Texture2D* canvasFor = nullptr;
    // the settings dialog (the page shows it while settingsOpen; formRev changes when its values must be re-read)
    bool settingsOpen = false, formRestart = false;
    bool pageKeys = false;  // a pop-up or dialog is open in the page: the keys are its own, no shortcuts
    int formRev = 0;
    std::wstring formMessage;
    // the monitor wall ([viewer] console_layout = 1 / 2 / q): one monitor, two (A = the program, B = secondKey,
    // [viewer] console_second), or one with the other sources as small previews
    int wall = 1;  // 0 = one, 1 = two, 2 = one + 4
    std::wstring secondKey = L"lso1";
    // the 1 + 4 previews' sources ([viewer] console_previews = "lso1,lso2,island,"; "" = none); prevInit = read or set
    std::wstring prev[4];
    bool prevInit = false;
    std::wstring lastDeck;  // the deck position last shown (the DECK key brings it back)
    bool progOff = false;   // monitor A switched off (its lit source key pressed again): NO SOURCE until a key is pressed
} g_con;
// The source monitor A (the program) shows: app.selected, or -1 while the console has it switched off.
int ProgramSource() { return g_con.progOff ? -1 : app.selected; }
int ConsoleSecond();  // the source monitor B shows (-1: none / not on the wall); console.inc
void ParseKnownCams(const std::wstring& t);  // the cameras the last DCS session offered
bool g_neutral = false;  // drawing a console preview: no adjustments, levels or CCTV look
void ConsoleDropSwap();
bool ConsoleCreateSwap(IDXGIFactory2* factory);
void ConsoleRender();
void ConsoleResize();
void ConsolePush(bool force = false);
void ConsoleSavePlacement();

// ---------------------------------------------------------------- settings

void LoadSettings() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    app.iniPath = exe;
    const std::wstring dir = app.iniPath.substr(0, app.iniPath.find_last_of(L"\\/") + 1);
    app.iniPath = dir + L"ILARTS.ini";
    // settings of the viewer under its old name (lso_viewer.ini), carried over once
    if (GetFileAttributesW(app.iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        CopyFileW((dir + L"lso_viewer.ini").c_str(), app.iniPath.c_str(), TRUE);
    auto get = [](const wchar_t* key, float def) {
        wchar_t buf[64];
        GetPrivateProfileStringW(L"LSOFRAME", key, L"", buf, 64, app.iniPath.c_str());
        return buf[0] ? static_cast<float>(_wtof(buf)) : def;
    };
    app.set.brightness = std::clamp(get(L"brightness", 0.0f), -0.5f, 0.5f);
    app.set.contrast = std::clamp(get(L"contrast", 1.0f), 0.25f, 16.0f);
    app.set.gamma = std::clamp(get(L"gamma", 1.0f), 0.3f, 4.0f);
    app.set.stops = std::clamp(get(L"exposure_stops", 0.0f), -4.0f, 8.0f);
    app.set.autoLevels = get(L"auto_levels", 0.0f) > 0.5f;
    app.set.symbology = true;  // the switch is gone (2026-11-14): DCS's PLAT symbology always shows
    // Always Pipeline (the viewer falls back to Levels by itself when DCS does not share the HDR image). The
    // mode is no longer in the UI; [viewer] mode=levels / off overrides it for troubleshooting.
    app.set.proc = Proc::Pipeline;
    wchar_t pm[16] = {};
    GetPrivateProfileStringW(L"viewer", L"mode", L"", pm, 16, app.iniPath.c_str());
    if (_wcsicmp(pm, L"levels") == 0) app.set.proc = Proc::Levels;
    if (_wcsicmp(pm, L"off") == 0) app.set.proc = Proc::Off;
    app.selected = 0;  // the app always starts with the PLAT on monitor A and the island camera on B (user, 2026-11-14)
    app.set.enabled = get(L"adjustments", 1.0f) > 0.5f;
    app.debug = GetPrivateProfileIntW(L"viewer", L"debug", 0, app.iniPath.c_str()) != 0;
    {
        wchar_t wl[8] = {}, sk[64] = {};
        GetPrivateProfileStringW(L"viewer", L"console_layout", L"2", wl, 8, app.iniPath.c_str());
        g_con.wall = wl[0] == L'1' ? 0 : (wl[0] == L'q' || wl[0] == L'Q') ? 2 : 1;
        GetPrivateProfileStringW(L"viewer", L"console_second", L"lso1", sk, 64, app.iniPath.c_str());
        g_con.secondKey = L"island";  // monitor B always starts on the island camera (user, 2026-11-14)
        wchar_t pv[256] = {}, ld[64] = {};
        if (GetPrivateProfileStringW(L"viewer", L"console_previews", L"\x1", pv, 256, app.iniPath.c_str()) && pv[0] != 1) {
            std::wstring t = pv;
            for (int k = 0, a = 0; k < 4; ++k) {
                const size_t e = t.find(L',', a);
                g_con.prev[k] = a <= static_cast<int>(t.size()) ? t.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a) : L"";
                a = e == std::wstring::npos ? static_cast<int>(t.size()) + 1 : static_cast<int>(e) + 1;
            }
            g_con.prevInit = true;
        }
        GetPrivateProfileStringW(L"viewer", L"console_deck", L"", ld, 64, app.iniPath.c_str());
        g_con.lastDeck = ld;
    }
    for (int g = 1; g < kAnalyses; ++g) {  // each camera's own levels
        const std::wstring sec = GroupSection(g);
        auto gget = [&](const wchar_t* key, float def) {
            wchar_t buf[64];
            GetPrivateProfileStringW(sec.c_str(), key, L"", buf, 64, app.iniPath.c_str());
            return buf[0] ? static_cast<float>(_wtof(buf)) : def;
        };
        Settings& s = g_groupSet[g];
        s.brightness = std::clamp(gget(L"brightness", 0.0f), -0.5f, 0.5f);
        s.contrast = std::clamp(gget(L"contrast", 1.0f), 0.25f, 16.0f);
        s.gamma = std::clamp(gget(L"gamma", 1.0f), 0.3f, 4.0f);
        s.stops = std::clamp(gget(L"exposure_stops", 0.0f), -4.0f, 8.0f);
        s.autoLevels = gget(L"auto_levels", 0.0f) > 0.5f;
        s.enabled = gget(L"adjustments", 1.0f) > 0.5f;
        s.symbology = true;
        s.proc = app.set.proc;
    }
    g_rec.autoMode = GetPrivateProfileIntW(L"video", L"auto", 0, app.iniPath.c_str()) != 0;
    {
        wchar_t af[32] = {};
        GetPrivateProfileStringW(L"video", L"auto_filter", L"all", af, 32, app.iniPath.c_str());
        g_rec.autoFilter = AutoFilterFromKey(af);
    }
    {
        wchar_t kc[4096] = {};  // 6 cameras x 16 positions x up to 22 characters
        GetPrivateProfileStringW(L"cameras", L"known", L"", kc, 4096, app.iniPath.c_str());
        ParseKnownCams(kc);
    }
    auto sec = [](const wchar_t* section, const wchar_t* key, double def) {
        wchar_t buf[64];
        GetPrivateProfileStringW(section, key, L"", buf, 64, app.iniPath.c_str());
        return buf[0] ? _wtof(buf) : def;
    };
    auto text = [](const wchar_t* section, const wchar_t* key) {
        wchar_t buf[512] = {};
        GetPrivateProfileStringW(section, key, L"", buf, 512, app.iniPath.c_str());
        std::string s;
        for (const wchar_t* p = buf; *p; ++p) s += static_cast<char>(*p);
        return s;
    };
    g_cctv.on = sec(L"cctv", L"on", 1) > 0.5;  // on unless switched off
    g_cctv.recOnly = sec(L"cctv", L"recordings_only", 0) > 0.5;
    g_antialias = sec(L"viewer", L"antialias", 1) > 0.5;
    g_expFlir = sec(L"experimental", L"flir", 0) > 0.5;
    g_cctv.strength = static_cast<float>(std::clamp(sec(L"cctv", L"strength", 1), 0.0, 2.0));
    g_cctv.grain = static_cast<float>(std::clamp(sec(L"cctv", L"grain", 1), 0.0, 4.0));
    g_cctv.scan = static_cast<float>(std::clamp(sec(L"cctv", L"scanlines", 1), 0.0, 4.0));
    g_cctv.distort = static_cast<float>(std::clamp(sec(L"cctv", L"distortion", 1), 0.0, 4.0));
    g_cctv.vignette = static_cast<float>(std::clamp(sec(L"cctv", L"vignette", 1), 0.0, 4.0));
    g_bb.on = sec(L"blackbox", L"on", 1) > 0.5;  // on unless switched off
    g_bb.minutes = std::clamp(sec(L"blackbox", L"minutes", 5), 1.0, 30.0);
    g_bb.before = std::clamp(sec(L"blackbox", L"mishap_before", 60), 5.0, g_bb.minutes * 60);
    g_bb.after = std::clamp(sec(L"blackbox", L"mishap_after", 15), 0.0, 120.0);
    g_liveBitrate = static_cast<int>(std::clamp(sec(L"stream", L"bitrate_kbps", 2500), 300.0, 20000.0) * 1000);
    g_compositeBitrate = static_cast<int>(std::clamp(sec(L"stream", L"composite_kbps", 6000), 1000.0, 30000.0) * 1000);
    g_streamAudio = sec(L"stream", L"audio", 1) > 0.5;
}

// ---------------------------------------------------------------- presets
// A slot stores the complete look of every picture (since 2026-11-14): for each settings group - the PLAT
// (with the LSO station screens) and each extra camera - auto-levels, exposure, brightness, contrast and gamma.
// The master Adjustments switch, the processing mode and the symbology are not part of a preset. Slot 0 is the
// built-in Night preset (fixed, not saved to the ini; the same look for every picture); slots 1-3 are the
// user's, stored in [preset1]..[preset3] with an optional name that fits its button: the PLAT's values under
// their plain keys (as before), camera i's under camera<i>_<key> with camera<i>_saved = 1. A camera a slot has
// no values for (a slot saved before cameras were part of presets) keeps its own levels when it is loaded.
constexpr int kPresets = 4;
constexpr int kNightPreset = 0;
struct Preset {
    bool saved = false;
    Settings s[kAnalyses];
    bool has[kAnalyses] = {};  // the slot holds this group's look
    std::wstring name;
};
Preset g_presets[kPresets];
std::wstring g_flash;          // short confirmation line (the console's preset panel)
ULONGLONG g_flashUntil = 0;

void Flash(const std::wstring& text, ULONGLONG ms = 2500) {
    g_flash = text;
    g_flashUntil = GetTickCount64() + ms;
}

// The Night look, set by the user on 2026-11-13 (night mission, PLAT feed).
Settings NightSettings() {
    Settings n;
    n.proc = Proc::Pipeline;
    n.symbology = true;
    n.autoLevels = true;
    n.stops = 0.91f;
    n.brightness = 0.13f;
    n.contrast = 1.5f;
    n.gamma = 1.2f;
    return n;
}

std::wstring PresetSection(int i) { return L"preset" + std::to_wstring(i); }  // user slots 1-3
std::wstring DefaultPresetName(int i) { return i == kNightPreset ? L"Night" : std::to_wstring(i); }
std::wstring PresetName(int i) { return g_presets[i].name.empty() ? DefaultPresetName(i) : g_presets[i].name; }
// A group's key prefix inside a preset section: "" for the PLAT, "camera<i>_" for camera i.
std::wstring PresetPrefix(int g) { return g == 0 ? std::wstring() : L"camera" + std::to_wstring(g - 1) + L"_"; }

void LoadPresets() {
    Preset& night = g_presets[kNightPreset];
    night.saved = true;
    night.name = L"Night";
    for (int g = 0; g < kAnalyses; ++g) night.s[g] = NightSettings(), night.has[g] = true;
    for (int i = 1; i < kPresets; ++i) {
        std::wstring sec = PresetSection(i);
        Preset& pr = g_presets[i];
        for (int g = 0; g < kAnalyses; ++g) {
            const std::wstring pre = PresetPrefix(g);
            auto get = [&](const wchar_t* key, float def) {
                wchar_t buf[64];
                GetPrivateProfileStringW(sec.c_str(), (pre + key).c_str(), L"", buf, 64, app.iniPath.c_str());
                return buf[0] ? static_cast<float>(_wtof(buf)) : def;
            };
            if (g == 0) pr.saved = get(L"saved", 0.0f) > 0.5f;
            pr.has[g] = g == 0 || get(L"saved", 0.0f) > 0.5f;
            Settings& s = pr.s[g];
            s.brightness = std::clamp(get(L"brightness", 0.0f), -0.5f, 0.5f);
            s.contrast = std::clamp(get(L"contrast", 1.0f), 0.25f, 16.0f);
            s.gamma = std::clamp(get(L"gamma", 1.0f), 0.3f, 4.0f);
            s.stops = std::clamp(get(L"exposure_stops", 0.0f), -4.0f, 8.0f);
            s.autoLevels = get(L"auto_levels", 0.0f) > 0.5f;
            s.symbology = true;
            s.proc = Proc::Pipeline;
        }
        wchar_t nm[64] = {};
        GetPrivateProfileStringW(sec.c_str(), L"name", L"", nm, 64, app.iniPath.c_str());
        pr.name = nm;
    }
}

int CameraCount();
void SavePreset(int i) {
    if (i == kNightPreset) return;
    Preset& pr = g_presets[i];
    pr.saved = true;
    std::wstring sec = PresetSection(i);
    for (int g = 0; g < std::min(kAnalyses, 1 + CameraCount()); ++g) {
        pr.s[g] = SetFor(g);
        pr.has[g] = true;
        const std::wstring pre = PresetPrefix(g);
        auto put = [&](const wchar_t* key, float v) {
            wchar_t buf[64];
            swprintf_s(buf, L"%.3f", v);
            WritePrivateProfileStringW(sec.c_str(), (pre + key).c_str(), buf, app.iniPath.c_str());
        };
        const Settings& s = pr.s[g];
        put(L"saved", 1.0f);
        put(L"brightness", s.brightness);
        put(L"contrast", s.contrast);
        put(L"gamma", s.gamma);
        put(L"exposure_stops", s.stops);
        put(L"auto_levels", s.autoLevels ? 1.0f : 0.0f);
    }
    Flash(L"Saved to " + PresetName(i));
}

void ClearPreset(int i) {
    if (i == kNightPreset) return;
    g_presets[i].saved = false;
    WritePrivateProfileStringW(PresetSection(i).c_str(), L"saved", L"0", app.iniPath.c_str());
    Flash(L"Cleared " + PresetName(i));
}

void RenamePreset(int i, std::wstring name) {
    if (i == kNightPreset) return;
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    while (!name.empty() && name.back() == L' ') name.pop_back();
    if (name == DefaultPresetName(i)) name.clear();
    g_presets[i].name = name;
    WritePrivateProfileStringW(PresetSection(i).c_str(), L"name", name.empty() ? nullptr : name.c_str(), app.iniPath.c_str());
    Flash(L"Renamed to " + PresetName(i));
}

// True when every picture the preset holds looks as it says (ignoring the master switch).
bool PresetActive(int i) {
    const Preset& pr = g_presets[i];
    if (!pr.saved) return false;
    auto eq = [](float a, float b) { return std::fabs(a - b) < 0.005f; };
    for (int g = 0; g < kAnalyses; ++g) {
        if (!pr.has[g]) continue;
        const Settings& a = SetFor(g);
        const Settings& b = pr.s[g];
        if (!(a.autoLevels == b.autoLevels && eq(a.stops, b.stops) && eq(a.brightness, b.brightness) &&
              eq(a.contrast, b.contrast) && eq(a.gamma, b.gamma)))
            return false;
    }
    return true;
}

// One settings group's levels to its ini section ([LSOFRAME] / [camera<i>_picture]).
void SaveGroupSettings(int g, const Settings& set) {
    const std::wstring gs = GroupSection(g);
    auto putg = [&](const wchar_t* key, float v) {
        wchar_t buf[64];
        swprintf_s(buf, L"%.3f", v);
        WritePrivateProfileStringW(gs.c_str(), key, buf, app.iniPath.c_str());
    };
    putg(L"brightness", set.brightness);
    putg(L"contrast", set.contrast);
    putg(L"gamma", set.gamma);
    putg(L"exposure_stops", set.stops);
    putg(L"auto_levels", set.autoLevels ? 1.0f : 0.0f);
    putg(L"adjustments", set.enabled ? 1.0f : 0.0f);
}

void SaveSettings() {
    auto put = [](const wchar_t* key, float v) {
        wchar_t buf[64];
        swprintf_s(buf, L"%.3f", v);
        WritePrivateProfileStringW(L"LSOFRAME", key, buf, app.iniPath.c_str());
    };
    SaveGroupSettings(g_curGroup, app.set);  // the picture on program: its own section
    put(L"mode", static_cast<float>(app.set.proc));
    put(L"feed", static_cast<float>(std::min(app.selected, kFeeds - 1)));  // a camera feed is not restored
    WritePrivateProfileStringW(L"video", L"auto", g_rec.autoMode ? L"1" : L"0", app.iniPath.c_str());
    WritePrivateProfileStringW(L"video", L"composite", g_rec.composite ? L"1" : L"0", app.iniPath.c_str());
    WritePrivateProfileStringW(L"video", L"auto_filter", kAutoFilterKey[g_rec.autoFilter], app.iniPath.c_str());
    wchar_t st[32];
    swprintf_s(st, L"%.2f", g_cctv.strength);
    WritePrivateProfileStringW(L"cctv", L"on", g_cctv.on ? L"1" : L"0", app.iniPath.c_str());
    WritePrivateProfileStringW(L"cctv", L"strength", st, app.iniPath.c_str());
    WritePrivateProfileStringW(L"cctv", L"recordings_only", g_cctv.recOnly ? L"1" : L"0", app.iniPath.c_str());
    WritePrivateProfileStringW(L"blackbox", L"on", g_bb.on ? L"1" : L"0", app.iniPath.c_str());
}

// ---------------------------------------------------------------- device

void ResetAuto() {
    for (auto& a : app.an) a.haveAutoTarget = false;
}

void CloseTargets() {
    for (auto& t : app.targets) t.close();
    for (auto& a : app.an) {
        a.histCam.reset();
        a.histK.reset();
    }
    ResetAuto();
}

void OverlayRelease();
void StationRelease();
void RecRelease();

void CloseDevice() {
    CloseTargets();
    for (auto& a : app.an) {
        a.histCam.release();
        a.histK.release();
    }
    OverlayRelease();
    StationRelease();
    RecRelease();
    ConsoleDropSwap();
    SafeRelease(app.params);
    SafeRelease(app.rtv);
    SafeRelease(app.swap);
    SafeRelease(app.sampler);
    SafeRelease(app.csK);
    SafeRelease(app.csCam);
    SafeRelease(app.ps);
    SafeRelease(app.vs);
    SafeRelease(app.ctx);
    SafeRelease(app.dev1);
    SafeRelease(app.dev);
}

bool HeaderValid() {
    return app.header && app.header->magic == lsoshare::kMagic && app.header->version == lsoshare::kVersion;
}

uint32_t OwnAircraft() { return HeaderValid() ? app.header->ownAircraft : 0; }

// ---- cameras
// Physical cameras: what DCS renders (Header::cameras: ISLAND, then the deck camera's render slots DECK, DECK 2 ...
// - each with the deck positions as its views) - or, while DCS is not running or has not listed them yet, the ones
// the last session offered ([cameras] known = "ISLAND;DECK=BOW BUBBLE|...|HANGAR FWD\tH|...;DECK 2=..."; "\tH" after
// a name: a hangar deck position, as the hook marks it).
// Logical cameras - the sources the user picks: each camera without views (ISLAND), then each deck position (BOW
// BUBBLE, CATAPULT 1 ...). A position renders in a free deck slot only while it is wanted (a monitor on screen
// shows it); its slot goes back to the pool when it is not.
struct KnownCam {
    std::string name;
    std::vector<std::string> views;  // as remembered: "HANGAR FWD\tH"
};
std::vector<KnownCam> g_known;
std::wstring g_knownText;
void ParseKnownCams(const std::wstring& t) {
    g_knownText = t;
    g_known.clear();
    std::string a;
    for (wchar_t c : t) a += static_cast<char>(c);
    for (size_t p = 0; p < a.size() && g_known.size() < static_cast<size_t>(lsoshare::kMaxCameras);) {
        size_t e = a.find(';', p);
        if (e == std::string::npos) e = a.size();
        const std::string item = a.substr(p, e - p);
        p = e + 1;
        if (item.empty()) continue;
        KnownCam k;
        const size_t eq = item.find('=');
        k.name = item.substr(0, eq);
        if (eq != std::string::npos)
            for (size_t q = eq + 1; q <= item.size();) {
                size_t b = item.find('|', q);
                if (b == std::string::npos) b = item.size();
                if (b > q && k.views.size() < static_cast<size_t>(lsoshare::kMaxViews)) k.views.push_back(item.substr(q, b - q));
                q = b + 1;
            }
        g_known.push_back(k);
    }
}
int HeaderCameraCount() {
    if (!HeaderValid()) return 0;
    int n = 0;
    const int count = static_cast<int>(std::min<uint32_t>(app.header->cameraCount, lsoshare::kMaxCameras));
    while (n < count && app.header->cameras[n].available) ++n;
    return n;
}
bool CamLive() { return HeaderCameraCount() > 0; }  // the list is DCS's own
int PhysCount() { return CamLive() ? HeaderCameraCount() : static_cast<int>(g_known.size()); }
std::string PhysName(int p) {
    if (p < 0 || p >= PhysCount()) return {};
    if (!CamLive()) return g_known[p].name;
    const char* n = app.header->cameras[p].name;
    return std::string(n, strnlen(n, sizeof(app.header->cameras[p].name)));
}
int PhysViewCount(int p) {
    if (p < 0 || p >= PhysCount()) return 0;
    if (!CamLive()) return static_cast<int>(g_known[p].views.size());
    return static_cast<int>(std::min<uint32_t>(app.header->views[p].count, lsoshare::kMaxViews));
}
std::string PhysViewName(int p, int k) {
    if (k < 0 || k >= PhysViewCount(p)) return {};
    if (!CamLive()) return g_known[p].views[k].substr(0, g_known[p].views[k].find('\t'));
    const char* n = app.header->views[p].names[k];
    return std::string(n, strnlen(n, sizeof(app.header->views[p].names[k])));
}
bool PhysViewHangar(int p, int k) {  // a hangar deck position (the hook's hangar = true): grouped apart, never tracks
    if (k < 0 || k >= PhysViewCount(p)) return false;
    if (!CamLive()) return g_known[p].views[k].find("\tH") != std::string::npos;
    return app.header->views[p].names[k][sizeof(app.header->views[p].names[k]) - 1] == lsoshare::kViewHangar;
}
int PhysCurrentView(int p) { return CamLive() && p >= 0 && p < HeaderCameraCount() ? app.header->views[p].current : -1; }

struct LogicalCam {
    std::string name;
    int phys = -1;  // a fixed camera's physical camera
    int pos = -1;   // a deck position: its index in the deck camera's views
    bool hangar = false;  // a hangar deck position
};
std::vector<LogicalCam> g_lcams;
std::vector<int> g_pool;  // the deck camera's render slots (physical cameras with views)
int g_firstPos = 0;       // logical index of the first deck position
std::string g_camSig;
// The logical list from the physical one (when that changes; called every loop and at start).
void SyncCams() {
    std::string sig;
    const int n = PhysCount();
    for (int p = 0; p < n; ++p) {
        sig += PhysName(p) + "=";
        for (int k = 0; k < PhysViewCount(p); ++k) sig += PhysViewName(p, k) + (PhysViewHangar(p, k) ? "\tH|" : "|");
        sig += ";";
    }
    if (sig == g_camSig) return;
    g_camSig = sig;
    g_lcams.clear();
    g_pool.clear();
    int viewsOf = -1;
    for (int p = 0; p < n; ++p) {
        if (PhysViewCount(p) > 0) {
            g_pool.push_back(p);
            if (viewsOf < 0) viewsOf = p;
        } else if (g_lcams.size() < static_cast<size_t>(kMaxLogical)) {
            g_lcams.push_back(LogicalCam{PhysName(p), p, -1});
        }
    }
    g_firstPos = static_cast<int>(g_lcams.size());
    for (int k = 0; viewsOf >= 0 && k < PhysViewCount(viewsOf) && g_lcams.size() < static_cast<size_t>(kMaxLogical); ++k)
        g_lcams.push_back(LogicalCam{PhysViewName(viewsOf, k), -1, k, PhysViewHangar(viewsOf, k)});
    for (auto& x : g_cam.slotPlus1) x = 0;  // positions get their slots afresh
}
int CameraCount() { return static_cast<int>(g_lcams.size()); }
bool IsPosition(int i) { return i >= 0 && i < CameraCount() && g_lcams[i].pos >= 0; }
bool IsHangar(int i) { return IsPosition(i) && g_lcams[i].hangar; }  // a hangar deck position: no AUTO
int PhysOf(int i) {  // the physical camera that renders logical camera i (-1: none now)
    if (i < 0 || i >= CameraCount()) return -1;
    const LogicalCam& c = g_lcams[i];
    if (c.pos < 0) return c.phys < PhysCount() ? c.phys : -1;
    return g_cam.slotPlus1[i] - 1;
}
bool CameraRendering(int i) {
    const int p = PhysOf(i);
    return CamLive() && p >= 0 && p < HeaderCameraCount() && app.header->cameras[p].enabled != 0;
}
std::wstring CameraName(int i) {  // "Island", "Waist bubble"
    std::wstring w;
    if (i < 0 || i >= CameraCount()) return w;
    const std::string& n = g_lcams[i].name;
    for (size_t k = 0; k < n.size(); ++k)
        w += static_cast<wchar_t>(static_cast<unsigned char>(k == 0 ? n[k] : tolower(static_cast<unsigned char>(n[k]))));
    return w;
}
std::wstring CameraTag(int i) {  // "ISLAND", "WAIST BUBBLE"
    std::wstring w = CameraName(i);
    for (auto& c : w) c = static_cast<wchar_t>(towupper(c));
    return w;
}
int FeedCount() { return kFeeds + CameraCount(); }

// A deck position's siblings (the monitors' view selector): all the deck positions.
int CameraViewCount(int i) { return IsPosition(i) ? CameraCount() - g_firstPos : 0; }
std::wstring CameraViewNameAt(int i, int k) { return IsPosition(i) && k >= 0 && k < CameraViewCount(i) ? CameraTag(g_firstPos + k) : L""; }
bool CameraViewHangarAt(int i, int k) { return IsPosition(i) && k >= 0 && k < CameraViewCount(i) && IsHangar(g_firstPos + k); }
int CameraViewIndex(int i, const std::wstring& name) {
    for (int k = 0; k < CameraViewCount(i); ++k)
        if (CameraViewNameAt(i, k) == name) return k;
    return -1;
}
std::wstring CameraViewName(int i) { return IsPosition(i) ? CameraTag(i) : L""; }
int PositionSource(int k) { return kFeeds + g_firstPos + k; }  // deck position k as a source

// Sources by name: "plat", "lso1", "lso2", then the cameras ("island", "bow_bubble": lower case, anything but
// letters and digits as '_'); tags for file names ("... WAIST BUBBLE.mp4").
std::wstring SourceKey(int s) {
    static const wchar_t* kFixed[kFeeds] = {L"plat", L"lso1", L"lso2"};
    if (s < kFeeds) return kFixed[s];
    std::wstring n = CameraName(s - kFeeds);
    for (auto& c : n) c = iswalnum(c) ? static_cast<wchar_t>(towlower(c)) : L'_';
    return n.empty() ? L"camera" + std::to_wstring(s - kFeeds) : n;
}
std::wstring SourceTag(int s) {
    static const wchar_t* kFixed[kFeeds] = {L"PLAT", L"LSO 1", L"LSO 2"};
    return s < kFeeds ? std::wstring(kFixed[s]) : CameraTag(s - kFeeds);
}
bool IsDeckSource(int s) { return s >= kFeeds && IsPosition(s - kFeeds); }

// Asks render slot (physical camera) p to show deck position k.
void CameraAskView(int p, int k) {
    if (!app.headerW || p < 0 || p >= lsoshare::kMaxCameras) return;
    auto& v = app.headerW->views[p];
    v.request = k;
    MemoryBarrier();
    InterlockedIncrement(&v.requestSeq);
    g_cam.viewAskedAt[p] = GetTickCount64();
}

// What each camera follows automatically, and whether that is armed (AUTO) - per camera as a source
// ([cameras] track_<source> = target, auto_<source> = 0 / 1). Targets (PtzControl::trackMode): 2 the recovery
// (landing area), 3 the catapult nearest the camera, 4 the active catapult, 5..8 catapult 1..4. Defaults: the island
// camera follows the recovery; a deck camera the catapult nearest it, not armed. A hangar position is never armed
// (whatever the file says).
constexpr int kTrackFirst = 2, kTrackLast = 8;
const wchar_t* TrackTargetName(int t) {
    static const wchar_t* k[] = {L"", L"", L"RECOVERY", L"NEAREST CAT", L"ACTIVE CAT", L"CAT 1", L"CAT 2", L"CAT 3", L"CAT 4"};
    return t >= kTrackFirst && t <= kTrackLast ? k[t] : L"";
}
struct CamTrack {
    int target = 0;
    bool armed = false;
};
std::map<std::wstring, CamTrack> g_camTrack;
CamTrack& TrackOf(int i) {  // logical camera i
    const std::wstring key = SourceKey(kFeeds + i);
    auto it = g_camTrack.find(key);
    if (it != g_camTrack.end()) {
        if (IsHangar(i)) it->second.armed = false;
        return it->second;
    }
    CamTrack t;
    const bool deck = IsPosition(i);
    t.target = GetPrivateProfileIntW(L"cameras", (L"track_" + key).c_str(), deck ? 3 : 2, app.iniPath.c_str());
    if (t.target < kTrackFirst || t.target > kTrackLast) t.target = deck ? 3 : 2;
    t.armed = !IsHangar(i) && GetPrivateProfileIntW(L"cameras", (L"auto_" + key).c_str(), deck ? 0 : 1, app.iniPath.c_str()) != 0;
    return g_camTrack[key] = t;
}
void SaveTrack(int i) {
    const std::wstring key = SourceKey(kFeeds + i);
    const CamTrack& t = TrackOf(i);
    WritePrivateProfileStringW(L"cameras", (L"track_" + key).c_str(), std::to_wstring(t.target).c_str(), app.iniPath.c_str());
    WritePrivateProfileStringW(L"cameras", (L"auto_" + key).c_str(), t.armed ? L"1" : L"0", app.iniPath.c_str());
}

// The VTR's REC switch of each source ([video] record_<source>): its picture gets its own file in every
// recording. On by default for the PLAT and the cameras.
std::map<std::wstring, bool> g_recArm;
std::wstring RecKey(int s) { return IsDeckSource(s) ? std::wstring(L"deck") : SourceKey(s); }
// bench.inc (SETTINGS > Benchmark): while a run is under way it decides which cameras render and which sources record.
bool BenchActive();
bool BenchWantsCamera(int i);
bool BenchRecArmed(int s);
void BenchEncoded(const video::Event& e);
constexpr int kBenchRecKey = -7;  // the engine's recording key of the benchmark's null-sink recordings
bool RecArmed(int s) {
    if (s < 0) return false;
    if (BenchActive()) return BenchRecArmed(s);  // the user's switches stay as they are
    const std::wstring k = RecKey(s);
    const auto it = g_recArm.find(k);
    if (it != g_recArm.end()) return it->second;
    const bool on = GetPrivateProfileIntW(L"video", (L"record_" + k).c_str(), s == 1 || s == 2 ? 0 : 1, app.iniPath.c_str()) != 0;
    g_recArm[k] = on;
    return on;
}

bool CreateBackbufferView() {
    SafeRelease(app.rtv);
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(app.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return false;
    HRESULT hr = app.dev->CreateRenderTargetView(bb, nullptr, &app.rtv);
    bb->Release();
    return SUCCEEDED(hr);
}

HRESULT Compile(const char* entry, const char* target, ID3DBlob** out) {
    char bins[16], kmin[16], kmax[16];
    sprintf_s(bins, "%u", kBins);
    sprintf_s(kmin, "%.1f", kLog2KMin);
    sprintf_s(kmax, "%.1f", kLog2KMax);
    const D3D_SHADER_MACRO defs[] = {{"BINS", bins}, {"KMIN", kmin}, {"KMAX", kmax}, {nullptr, nullptr}};
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "lso_viewer", defs, nullptr, entry, target, 0, 0, out, &err);
    if (FAILED(hr) && err) OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
    SafeRelease(err);
    return hr;
}

bool CreatePipeline() {
    ID3DBlob *vsb = nullptr, *psb = nullptr, *c1 = nullptr, *c2 = nullptr;
    HRESULT hr = Compile("vs", "vs_5_0", &vsb);
    if (SUCCEEDED(hr)) hr = Compile("ps", "ps_5_0", &psb);
    if (SUCCEEDED(hr)) hr = Compile("csCam", "cs_5_0", &c1);
    if (SUCCEEDED(hr)) hr = Compile("csK", "cs_5_0", &c2);
    if (SUCCEEDED(hr)) hr = app.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &app.vs);
    if (SUCCEEDED(hr)) hr = app.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &app.ps);
    if (SUCCEEDED(hr)) hr = app.dev->CreateComputeShader(c1->GetBufferPointer(), c1->GetBufferSize(), nullptr, &app.csCam);
    if (SUCCEEDED(hr)) hr = app.dev->CreateComputeShader(c2->GetBufferPointer(), c2->GetBufferSize(), nullptr, &app.csK);
    SafeRelease(vsb);
    SafeRelease(psb);
    SafeRelease(c1);
    SafeRelease(c2);
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = app.dev->CreateSamplerState(&smp, &app.sampler);
    D3D11_BUFFER_DESC cb{sizeof(Params), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    if (SUCCEEDED(hr)) hr = app.dev->CreateBuffer(&cb, nullptr, &app.params);
    bool ok = SUCCEEDED(hr);
    for (auto& a : app.an) ok = ok && a.histCam.create(app.dev) && a.histK.create(app.dev);
    return ok;
}

bool CreateDevice(LUID luid) {
    CloseDevice();
    IDXGIFactory2* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)))) return false;
    IDXGIAdapter1* chosen = nullptr;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
            chosen = a;
            break;
        }
        a->Release();
    }
    if (!chosen) {
        wchar_t s[128];
        swprintf_s(s, L"DCS adapter (LUID %08lX:%08lX) not found", luid.HighPart, luid.LowPart);
        app.source = s;
        factory->Release();
        return false;
    }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                   D3D11_SDK_VERSION, &app.dev, nullptr, &app.ctx);
    chosen->Release();
    if (SUCCEEDED(hr)) hr = app.dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&app.dev1));
    if (SUCCEEDED(hr)) hr = ConsoleCreateSwap(factory) ? S_OK : E_FAIL;
    factory->Release();
    if (FAILED(hr) || !CreateBackbufferView() || !CreatePipeline()) {
        wchar_t s[128];
        swprintf_s(s, L"device/pipeline creation failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
        app.source = s;
        CloseDevice();
        return false;
    }
    app.deviceLuid = luid;
    return true;
}

bool OpenTarget(Target& t, const lsoshare::TargetSlot& s) {
    t.close();
    HRESULT hr = app.dev1->OpenSharedResourceByName(s.handleName, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                    __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t.shared));
    if (SUCCEEDED(hr)) hr = t.shared->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&t.mutex));
    D3D11_TEXTURE2D_DESC d{};
    if (SUCCEEDED(hr)) {
        t.shared->GetDesc(&d);
        d.MiscFlags = 0;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = app.dev->CreateTexture2D(&d, nullptr, &t.local);
    }
    if (SUCCEEDED(hr)) hr = app.dev->CreateShaderResourceView(t.local, nullptr, &t.srv);
    if (FAILED(hr)) {
        t.close();
        return false;
    }
    t.w = d.Width;
    t.h = d.Height;
    strncpy_s(t.name, s.name, _TRUNCATE);
    return true;
}

Target* FindTarget(const char* name) {
    for (auto& t : app.targets)
        if (t.srv && strcmp(t.name, name) == 0) return &t;
    return nullptr;
}

// ---------------------------------------------------------------- analysis

float BinToSqrtValue(double bin) {
    double s = (bin + 0.5) / kBins;
    return static_cast<float>(s * s);
}

void ConsumeCamHistogram(Analysis& a, const UINT* h) {
    unsigned long long total = 0;
    for (UINT i = 0; i < kBins; ++i) total += h[i];
    if (total <= 100) return;
    const unsigned long long lo = total * 5 / 1000, hi = total * 990 / 1000;
    unsigned long long acc = 0;
    int loBin = -1, hiBin = kBins - 1;
    for (UINT i = 0; i < kBins; ++i) {
        acc += h[i];
        if (loBin < 0 && acc > lo) loBin = static_cast<int>(i);
        if (acc >= hi) {
            hiBin = static_cast<int>(i);
            break;
        }
    }
    float black = BinToSqrtValue(std::max(loBin, 0)) * 0.9f;
    float white = std::max(BinToSqrtValue(hiBin + 1), black + 0.04f);
    a.targetBlack = black;
    a.targetWhite = std::min(white, 1.0f);
    if (!a.haveAutoTarget) {
        a.autoBlack = a.targetBlack;
        a.autoWhite = a.targetWhite;
    }
    a.haveAutoTarget = true;
}

void ConsumeKHistogram(Analysis& a, const UINT* h) {
    unsigned long long total = 0;
    for (UINT i = 0; i < kBins; ++i) total += h[i];
    if (total < 200) return;
    unsigned long long acc = 0;
    for (UINT i = 0; i < kBins; ++i) {
        acc += h[i];
        if (acc * 2 >= total) {
            a.kTarget = std::exp2(kLog2KMin + (i + 0.5f) / kBins * (kLog2KMax - kLog2KMin));
            if (!a.haveK) a.kDcs = a.kTarget;
            a.haveK = true;
            return;
        }
    }
}

void Smooth(Analysis& a) {
    const double now = video::Now();
    const double dt = a.lastT > 0 ? now - a.lastT : 0.0;
    a.lastT = now;
    float f = static_cast<float>(1.0 - std::exp(-std::min(dt, 0.5) / 0.4));
    const bool autoLevels = SetFor(static_cast<int>(&a - app.an)).autoLevels;
    if (autoLevels && a.haveAutoTarget) {
        a.autoBlack += (a.targetBlack - a.autoBlack) * f;
        a.autoWhite += (a.targetWhite - a.autoWhite) * f;
    } else if (!autoLevels) {
        a.autoBlack = 0.0f;
        a.autoWhite = 1.0f;
    }
    if (a.haveK) a.kDcs *= std::pow(a.kTarget / a.kDcs, f);
}

// ---------------------------------------------------------------- per frame

// What the current feed needs. view: 0 plain, 1 PLAT, 2 station. proc follows the settings, degraded
// when a source is missing (pipeline needs HDR; station processing needs a 1024x768 station frame).
struct ViewState {
    int view = 0;
    Proc proc = Proc::Off;
    Target* main = nullptr;
    Target* lso = nullptr;
    Target* hdr = nullptr;
    bool synth = false;  // main = the station screen drawn by the viewer
    bool synthCue = false;  // main = the cue panel drawn by the viewer
    int cam = 0;         // extra camera i + 1 (0 = not one)
    int an = 0;          // which Analysis the picture uses
    bool noLdr = false;  // an extra camera with only its HDR image (lso = hdr, for its size)
};

// DCS installation folder, from the producer's executable (<DCS>\bin\DCS.exe or bin-mt\DCS.exe);
// the environment variable LSO_DCS_ROOT overrides it.
std::wstring DcsRoot() {
    std::wstring root;
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"LSO_DCS_ROOT", env, MAX_PATH) > 0) return env;   // override / tests
    if (!HeaderValid()) return root;
    if (HANDLE pr = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, app.header->producerPid)) {
        wchar_t exe[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(pr, 0, exe, &n)) {
            root = exe;
            for (int up = 0; up < 2; ++up) {
                const size_t k = root.find_last_of(L"\\/");
                if (k != std::wstring::npos) root.resize(k);
            }
        }
        CloseHandle(pr);
    }
    return root;
}

// ---------------------------------------------------------------- station screen drawn by the viewer
struct StationSynth {
    station::Renderer* r = nullptr;
    ID3D11Texture2D* tex = nullptr;      // LSO 1 layer
    ID3D11Texture2D* cueTex = nullptr;   // LSO 2 layer
    Target target, cueTarget;
    LONG seq = -1, cueSeqSt = -1, cueSeq = -1;
    DWORD pid = 0;
    bool failed = false;
} g_st;

void StationRelease() {
    SafeRelease(g_st.target.srv);
    SafeRelease(g_st.cueTarget.srv);
    SafeRelease(g_st.tex);
    SafeRelease(g_st.cueTex);
    delete g_st.r;
    g_st.r = nullptr;
    g_st.seq = g_st.cueSeqSt = g_st.cueSeq = -1;
    g_st.failed = false;
}

bool MakeLayer(int w, int h, ID3D11Texture2D** tex, Target& t, const char* name) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(app.dev->CreateTexture2D(&td, nullptr, tex)) || FAILED(app.dev->CreateShaderResourceView(*tex, nullptr, &srv))) return false;
    t.srv = srv;
    t.w = w;
    t.h = h;
    strcpy_s(t.name, name);
    return true;
}

// Renderer and both layers, created once per DCS session; false while that is not possible.
bool ScreensReady() {
    if (!HeaderValid() || !app.dev) return false;
    if (g_st.pid != app.header->producerPid) {
        StationRelease();
        g_st.pid = app.header->producerPid;
    }
    if (g_st.failed) return false;
    if (g_st.r) return true;
    const std::wstring root = DcsRoot();
    if (root.empty()) return false;
    g_st.r = new station::Renderer();
    const std::string why = g_st.r->init(root, app.dev, app.ctx);
    if (!why.empty()) OutputDebugStringA(("lso_viewer station: " + why + "\n").c_str());
    if (!MakeLayer(station::kW, station::kH, &g_st.tex, g_st.target, "LSO station (viewer)") ||
        !MakeLayer(station::kCueW, station::kCueH, &g_st.cueTex, g_st.cueTarget, "LSO cue panel (viewer)")) {
        g_st.failed = true;
        return false;
    }
    return true;
}

void PutText(char* dst, size_t n, const char* src, size_t sn) {
    if (!src[0]) return;
    const size_t k = std::min(n - 1, strnlen(src, sn));
    memcpy(dst, src, k);
    dst[k] = 0;
}

// The viewer's station screen (LSO 1) as a feed, or null (no data yet / it could not be built).
Target* StationTarget() {
    if (!app.header || !app.header->station.valid || !ScreensReady()) return nullptr;
    const LONG seq = app.header->sym.seq, cseq = app.header->cue.seq;
    if (seq != g_st.seq || cseq != g_st.cueSeqSt) {
        MemoryBarrier();
        lsoshare::Header::Station s;
        memcpy(&s, &app.header->station, sizeof(s));
        station::Values v;
        PutText(v.heading, sizeof(v.heading), s.heading, sizeof(s.heading));
        PutText(v.list, sizeof(v.list), s.list, sizeof(s.list));
        PutText(v.trim, sizeof(v.trim), s.trim, sizeof(s.trim));
        PutText(v.deckWind, sizeof(v.deckWind), app.header->sym.deckWind, sizeof(app.header->sym.deckWind));
        PutText(v.cross, sizeof(v.cross), s.cross, sizeof(s.cross));
        PutText(v.acType, sizeof(v.acType), s.acType, sizeof(s.acType));
        PutText(v.acHE, sizeof(v.acHE), s.acHE, sizeof(s.acHE));
        PutText(v.acBA, sizeof(v.acBA), s.acBA, sizeof(s.acBA));
        for (int i = 0; i < 3; ++i) PutText(v.case3[i], sizeof(v.case3[i]), app.header->cue.case3[i], sizeof(app.header->cue.case3[i]));
        v.crossSide = s.crossSide;
        v.foul = s.foul;
        if (s.htrFt != 0) v.htrFt = s.htrFt;
        if (s.htdFt != 0) v.htdFt = s.htdFt;
        g_st.r->compose(v);
        app.ctx->UpdateSubresource(g_st.tex, 0, nullptr, g_st.r->pixels(), station::kW * 4, 0);
        g_st.seq = seq;
        g_st.cueSeqSt = cseq;
    }
    return &g_st.target;
}

// The viewer's cue panel (LSO 2) as a feed, or null.
Target* CueTarget() {
    if (!app.header || !app.header->station.valid || !ScreensReady()) return nullptr;
    const LONG cseq = app.header->cue.seq;
    if (cseq != g_st.cueSeq) {
        MemoryBarrier();
        const auto& c = app.header->cue;
        station::CueValues v;
        v.count = static_cast<int>(std::min<uint32_t>(c.count, 16));
        for (int i = 0; i < v.count; ++i) {
            const auto& s = c.rows[i];
            station::CueRow& r = v.rows[i];
            r.status = s.status;
            PutText(r.type, sizeof(r.type), s.type, sizeof(s.type));
            PutText(r.board, sizeof(r.board), s.board, sizeof(s.board));
            PutText(r.cols[0], sizeof(r.cols[0]), s.board, sizeof(s.board));     // board
            PutText(r.cols[1], sizeof(r.cols[1]), s.pilot, sizeof(s.pilot));     // pilot
            if (s.fuelLb >= 0) _snprintf_s(r.cols[3], _countof(r.cols[3]), _TRUNCATE, "%0.1f/00", s.fuelLb * 0.001);   // fuel
            r.x = s.x, r.z = s.z, r.yaw = s.yaw, r.fuel = s.fuelLb;
        }
        g_st.r->composeCue(v);
        app.ctx->UpdateSubresource(g_st.cueTex, 0, nullptr, g_st.r->cuePixels(), station::kCueW * 4, 0);
        g_st.cueSeq = cseq;
    }
    return &g_st.cueTarget;
}

// The picture of a source: 0 PLAT, 1 LSO 1, 2 LSO 2, 3 + i extra camera i.
ViewState ViewFor(int source) {
    ViewState v;
    if (source >= kFeeds) {
        const int i = source - kFeeds;
        if (!CamLive() || i >= CameraCount()) return v;
        const int p = PhysOf(i);  // the render slot showing it
        if (p < 0 || p >= HeaderCameraCount()) return v;
        const auto& c = app.header->cameras[p];
        // Only while DCS renders it (the hook's enabled) and this viewer wants it: a camera switched off leaves its
        // render target behind, still copied every frame - the last picture, frozen, looking live. A deck slot
        // only once DCS has put it at this position (not the picture of the one before).
        if (!c.enabled || !((g_cam.want >> i) & 1)) return v;
        if (IsPosition(i) && app.header->views[p].current != g_lcams[i].pos) return v;
        char ldr[sizeof(c.ldr) + 1] = {}, hdr[sizeof(c.hdr) + 1] = {};
        memcpy(ldr, c.ldr, sizeof(c.ldr));
        memcpy(hdr, c.hdr, sizeof(c.hdr));
        v.cam = p + 1;  // the physical camera (its aspect)
        v.an = 1 + i;   // the logical one (its levels)
        v.hdr = hdr[0] ? FindTarget(hdr) : nullptr;
        v.lso = ldr[0] ? FindTarget(ldr) : nullptr;
        if (!v.lso && v.hdr) {
            v.lso = v.hdr;
            v.noLdr = true;
        }
        v.main = v.lso;
        if (!v.main) return v;
        v.view = 1;
        const Settings& S = SetFor(v.an);
        v.proc = S.enabled ? S.proc : Proc::Off;
        if (v.noLdr) v.proc = Proc::Pipeline;  // nothing else can show an HDR image
        if (v.proc == Proc::Pipeline && !v.hdr) v.proc = Proc::Levels;
        return v;
    }
    v.main = FindTarget(kFeedTargets[source]);
    v.lso = FindTarget("LSOFRAME");
    v.hdr = FindTarget("hdrBufferMFD32");
    if (!v.main && source == 1 && v.lso) {  // no station visit yet: the viewer's own station screen
        v.main = StationTarget();
        v.synth = v.main != nullptr;
        if (v.synth) v.main->fresh = v.lso->fresh;
    }
    if (!v.main && source == 2) {  // no station visit yet: the viewer's own cue panel
        v.main = CueTarget();
        if (v.main) {
            v.main->fresh = v.lso && v.lso->fresh;
            v.synthCue = true;
        }
    }
    if (!v.main || !v.lso) return v;
    if (source == 0) v.view = 1;
    if (source == 1 && v.main->w == static_cast<UINT>(kStationW) && v.main->h == static_cast<UINT>(kStationH)) v.view = 2;
    if (v.view == 0) return v;
    v.proc = SetFor(0).enabled ? SetFor(0).proc : Proc::Off;
    if (v.proc == Proc::Pipeline && !v.hdr) v.proc = Proc::Levels;
    return v;
}

ViewState CurrentView() { return ProgramSource() >= 0 ? ViewFor(app.selected) : ViewState{}; }

// Width / height a picture is shown at (an extra camera's render target may be square).
float ViewAspect(const ViewState& v) {
    if (v.cam && HeaderValid()) {
        const float a = app.header->cameras[v.cam - 1].aspect;
        if (a > 0.2f && a < 5.0f) return a;
    }
    if (v.view == 1 && !v.cam) return kPlatAspect;  // the PLAT (LSOFRAME): 4:3 in a square texture
    return v.main && v.main->h ? static_cast<float>(v.main->w) / v.main->h : 1.0f;
}

// The in-DCS PLAT adjustment (deploy/shaders: Bazar/shaders/PostEffects/LSO_PLAT.hlsl, hooked from BWTV.fx)
// changes LSOFRAME before the viewer sees it. Its #define values are read here so the shader can undo
// them (exposure measurement and symbology coverage compare against DCS's stock curve). DCS compiles the
// file at startup, so an edit made while DCS runs is only right after DCS restarts.
void ReadDcsAdjustment() {
    ULONGLONG now = GetTickCount64();
    if (app.dcsAdjRead && now - app.dcsAdjRead < 5000) return;
    app.dcsAdjRead = now;
    const std::wstring root = DcsRoot();
    if (root.empty()) return;
    const std::wstring dir = root + L"\\Bazar\\shaders\\PostEffects\\";
    auto slurp = [](const std::wstring& path) {
        std::string text;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);
        }
        return text;
    };
    const std::string hook = slurp(dir + L"BWTV.fx"), cfg = slurp(dir + L"LSO_PLAT.hlsl");
    auto define = [&cfg](const char* name, float def) {
        std::string key = std::string("#define ") + name;
        size_t at = cfg.find(key);
        if (at == std::string::npos) return def;
        return static_cast<float>(atof(cfg.c_str() + at + key.size()));
    };
    bool on = hook.find("LSO_PLAT") != std::string::npos && !cfg.empty() && define("LSO_ENABLE", 0) > 0.5f;
    app.dcsAdj[0] = define("LSO_EXPOSURE_STOPS", 0);
    app.dcsAdj[1] = define("LSO_GAMMA", 1);
    app.dcsAdj[2] = define("LSO_CONTRAST", 1);
    app.dcsAdj[3] = define("LSO_BRIGHTNESS", 0);
    app.dcsKeep = define("LSO_SYMBOLOGY_KEEP", 0.97f);
    app.dcsAdjOn = on;
}

// The PLAT text goes into the viewer's station screen when DCS has no overlay of its own.
bool StationOverlay(const ViewState& v) {
    return v.synth && HeaderValid() && app.set.symbology && app.header->sym.valid && !app.header->sym.dcsOverlay;
}

// pixelH: height in pixels the picture is drawn at (the CCTV look's scan lines follow it); 0 = not drawing.
// recording: the picture goes to a recording canvas (recordings, black box, stream), not to the window.
void WriteParams(const ViewState& v, float pixelH = 0, bool recording = false) {
    ReadDcsAdjustment();
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(app.ctx->Map(app.params, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    const Analysis& a = app.an[v.an];
    const bool cctv = !g_neutral && g_cctv.on && v.view != 0 && v.proc != Proc::Off && (recording || !g_cctv.recOnly);
    const bool n = g_neutral;
    const float camH = pixelH * (v.view == 2 ? kRectH / kStationH : 1.0f);
    const float frame = static_cast<float>(std::fmod(std::floor(video::Now() * 30.0), 4096.0));
    const Settings& S = SetFor(v.an);  // the picture's own levels (each camera has its own)
    Params p{n ? 0.0f : a.autoBlack, n ? 1.0f : a.autoWhite, n ? 1.0f : S.contrast, n ? 0.0f : S.brightness,
             n ? 1.0f : 1.0f / S.gamma, static_cast<float>(v.view), static_cast<float>(v.proc), a.kDcs,
             n ? 1.0f : std::exp2(S.stops),
             S.symbology && !v.cam ? 1.0f : 0.0f, {HeaderValid() && app.header->sym.night ? 1.0f : 0.0f, 0},
             {kRectX / kStationW, kRectY / kStationH, kRectW / kStationW, kRectH / kStationH},
             {kTintR, kTintG, kTintB, 1},
             {app.dcsAdj[0], app.dcsAdj[1], app.dcsAdj[2], app.dcsAdj[3]},
             {app.dcsKeep, app.dcsAdjOn ? 1.0f : 0.0f, 0, 0},
             {v.synth ? 1.0f : 0.0f, StationOverlay(v) ? 1.0f : 0.0f, HeaderValid() && app.header->sym.night ? 1.0f : 0.0f,
              v.noLdr ? 1.0f : 0.0f},
             {cctv ? g_cctv.strength : 0.0f, frame, camH, g_antialias ? 1.0f : 0.0f},
             {g_cctv.grain, g_cctv.scan, g_cctv.distort, g_cctv.vignette}};
    memcpy(m.pData, &p, sizeof(p));
    app.ctx->Unmap(app.params, 0);
}

void BindSources(const ViewState& v) {
    ID3D11ShaderResourceView* srvs[3] = {v.main ? v.main->srv : nullptr, v.hdr ? v.hdr->srv : nullptr,
                                         v.lso ? v.lso->srv : nullptr};
    app.ctx->PSSetShaderResources(0, 3, srvs);
    app.ctx->CSSetShaderResources(0, 3, srvs);
    app.ctx->CSSetConstantBuffers(0, 1, &app.params);
    app.ctx->PSSetConstantBuffers(0, 1, &app.params);
    app.ctx->CSSetSamplers(0, 1, &app.sampler);
}

void UnbindSources() {
    ID3D11ShaderResourceView* none[3] = {};
    app.ctx->PSSetShaderResources(0, 3, none);
    app.ctx->CSSetShaderResources(0, 3, none);
}

// [stream] autostart: the streams start once per DCS connection, when a source has a picture. Armed at start and
// whenever DCS goes away, so streams turned off by hand stay off until DCS comes back.
bool g_streamAutoArmed = true;
void SetStream(bool on);
void StreamAutoStart() {
    if (!g_streamAutoArmed || !g_rec.ok) return;
    bool ready = false;
    for (int s = 0; s < FeedCount() && !ready; ++s) ready = ViewFor(s).main != nullptr;
    if (!ready) return;
    g_streamAutoArmed = false;
    if (GetPrivateProfileIntW(L"stream", L"autostart", 0, app.iniPath.c_str()) && !StreamsOn()) SetStream(true);
}

void Update() {
    if (!HeaderValid()) {
        CloseTargets();
        app.source = L"waiting for DCS";
        g_streamAutoArmed = true;
        return;
    }
    if (app.selected >= FeedCount()) {  // the camera that was shown is gone
        app.selected = 0;
        UseGroup(0);
    }
    if (app.header->compat.state == 3) {  // DCS not compatible: nothing is shared; the video pane explains
        if (app.dev) CloseDevice();
        app.source = L"DCS version not compatible";
        return;
    }
    // GPU driver reset (TDR) or driver update: the device is gone for good. Drop it and build a new one;
    // targets, overlay and station layers are all recreated on demand. Retry at most once a second while
    // the driver is still recovering.
    static ULONGLONG s_retryAt = 0;
    if (app.dev && FAILED(app.dev->GetDeviceRemovedReason())) {
        CloseDevice();
        s_retryAt = 0;
    }
    if (!app.dev || app.deviceLuid.LowPart != app.header->adapterLuid.LowPart ||
        app.deviceLuid.HighPart != app.header->adapterLuid.HighPart) {
        const ULONGLONG now = GetTickCount64();
        if (now < s_retryAt) return;
        if (!CreateDevice(app.header->adapterLuid)) {
            s_retryAt = now + 1000;
            return;
        }
    }
    const UINT count = std::min<UINT>(app.header->targetCount, lsoshare::kMaxTargets);
    for (UINT i = 0; i < count; ++i) {
        const lsoshare::TargetSlot& s = app.header->targets[i];
        Target& t = app.targets[i];
        t.fresh = false;
        if (!s.active || s.generation == 0) {
            t.close();
            continue;
        }
        if (s.generation != t.generation) {
            if (!OpenTarget(t, s)) continue;
            t.generation = s.generation;
        }
        LONG64 frame = s.frameCounter;
        if (frame != t.lastFrame && t.mutex->AcquireSync(0, 5) == S_OK) {
            app.ctx->CopyResource(t.local, t.shared);
            t.mutex->ReleaseSync(0);
            t.lastFrame = frame;
            t.fresh = true;
            ++t.fpsCount;
        }
    }
    {
        const ULONGLONG ms = GetTickCount64();
        for (UINT i = 0; i < count; ++i) {
            Target& t = app.targets[i];
            if (!t.fpsStart) t.fpsStart = ms;
            if (ms - t.fpsStart >= 1000) {
                t.fps = t.fpsCount * 1000.0f / static_cast<float>(ms - t.fpsStart);
                t.fpsCount = 0;
                t.fpsStart = ms;
            }
        }
    }
    ViewState v = CurrentView();
    // exposure / levels of every camera picture in use: the one shown and those recorded in the background
    bool analysed[kAnalyses] = {};
    auto analyse = [&](const ViewState& x) {
        if (!x.main || x.view == 0 || x.proc == Proc::Off || analysed[x.an]) return;
        analysed[x.an] = true;
        Analysis& a = app.an[x.an];
        if (x.lso->fresh || (x.hdr && x.hdr->fresh)) {
            WriteParams(x);
            BindSources(x);
            if (x.proc == Proc::Pipeline) a.histK.dispatch(app.ctx, app.csK, x.lso->w, x.lso->h);
            if (SetFor(x.an).autoLevels) a.histCam.dispatch(app.ctx, app.csCam, x.lso->w, x.lso->h);
            UnbindSources();
        }
        UINT h[kBins];
        if (a.histK.poll(app.ctx, h)) ConsumeKHistogram(a, h);
        if (a.histCam.poll(app.ctx, h)) ConsumeCamHistogram(a, h);
        Smooth(a);
    };
    analyse(v);
    if (const int s2 = ConsoleSecond(); s2 >= 0) analyse(ViewFor(s2));  // monitor B
    for (const auto& ch : g_rec.extra)
        if (ch->source != ProgramSource()) analyse(ViewFor(ch->source));
    for (int i = 0; i < CameraCount(); ++i) {  // the camera previews go through the camera curve: DCS's exposure of each
        ViewState x = ViewFor(kFeeds + i);
        if (x.main && x.proc == Proc::Off) x.proc = x.hdr ? Proc::Pipeline : Proc::Levels;
        analyse(x);
    }
    StreamAutoStart();
    if (!v.main) {
        app.source = L"waiting for the picture from DCS";
        return;
    }
    if (v.main->fresh) ++app.framesThisSecond;
    ULONGLONG now = GetTickCount64();
    if (now - app.secondStart >= 1000) {
        app.sourceFps = app.framesThisSecond * 1000.0 / static_cast<double>(now - app.secondStart);
        app.framesThisSecond = 0;
        app.secondStart = now;
    }
    wchar_t s[96];
    swprintf_s(s, (app.debug && (v.synth || v.synthCue)) ? L"%ux%u  %.0f fps  (drawn by the viewer)" : L"%ux%u  %.0f fps", v.main->w,
               v.main->h, app.sourceFps);
    app.source = s;
}

// The whole window via PrintWindow (tests).
void SaveWindowCapture(const std::wstring& path) {
        RECT wr;
        GetWindowRect(app.hwnd, &wr);
        int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        HDC sdc = GetDC(nullptr);
        HDC mdc = CreateCompatibleDC(sdc);
        HBITMAP b = CreateCompatibleBitmap(sdc, ww, wh);
        HGDIOBJ old = SelectObject(mdc, b);
        PrintWindow(app.hwnd, mdc, PW_RENDERFULLCONTENT);
        BITMAPINFOHEADER bi{sizeof(BITMAPINFOHEADER), ww, wh, 1, 24, BI_RGB};
        UINT stride = (ww * 3 + 3) & ~3u;
        std::vector<unsigned char> px(static_cast<size_t>(stride) * wh);
        GetDIBits(mdc, b, 0, wh, px.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
        BITMAPFILEHEADER fh{0x4D42, static_cast<DWORD>(sizeof(fh) + sizeof(bi) + px.size()), 0, 0, sizeof(fh) + sizeof(bi)};
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&bi, sizeof(bi), 1, f);
            fwrite(px.data(), 1, px.size(), f);
            fclose(f);
        }
        SelectObject(mdc, old);
        DeleteObject(b);
        DeleteDC(mdc);
        ReleaseDC(nullptr, sdc);
    }

// The back buffer as a BMP (call after drawing, before Present).
void SaveBackbuffer(const std::wstring& path) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(app.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return;
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(app.dev->CreateTexture2D(&d, nullptr, &st))) {
        app.ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(app.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            UINT stride = (d.Width * 3 + 3) & ~3u;
            BITMAPFILEHEADER fh{0x4D42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + stride * d.Height), 0, 0,
                                sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
            BITMAPINFOHEADER ih{sizeof(BITMAPINFOHEADER), static_cast<LONG>(d.Width), static_cast<LONG>(d.Height), 1, 24, BI_RGB};
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                std::string row(stride, '\0');
                for (int y = static_cast<int>(d.Height) - 1; y >= 0; --y) {
                    const unsigned char* src = static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                    for (UINT x = 0; x < d.Width; ++x) {
                        row[x * 3] = static_cast<char>(src[x * 4 + 2]);
                        row[x * 3 + 1] = static_cast<char>(src[x * 4 + 1]);
                        row[x * 3 + 2] = static_cast<char>(src[x * 4]);
                    }
                    fwrite(row.data(), 1, stride, f);
                }
                fclose(f);
            }
            app.ctx->Unmap(st, 0);
        }
        st->Release();
    }
    bb->Release();
}

std::wstring g_shotPath;  // tests: save the next frame's back buffer here

// ---------------------------------------------------------------- video folder

const std::wstring& VideoFolder() {
    if (g_rec.folder.empty()) {
        wchar_t buf[MAX_PATH] = {};
        GetPrivateProfileStringW(L"video", L"folder", L"", buf, MAX_PATH, app.iniPath.c_str());
        g_rec.folder = buf;
        while (g_rec.folder.size() > 3 && (g_rec.folder.back() == L'\\' || g_rec.folder.back() == L'/')) g_rec.folder.pop_back();
        if (g_rec.folder.empty()) {
            PWSTR vids = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &vids)) && vids)
                g_rec.folder = std::wstring(vids) + L"\\" + kAppName;
            CoTaskMemFree(vids);
        }
        if (g_rec.folder.empty()) g_rec.folder = app.iniPath.substr(0, app.iniPath.find_last_of(L"\\/") + 1) + L"videos";
    }
    SHCreateDirectoryExW(nullptr, g_rec.folder.c_str(), nullptr);
    return g_rec.folder;
}

// Each mission's folder in it, stills and the storage limit: recfiles.inc.

// ---------------------------------------------------------------- PLAT text overlay
//
// Without a visit to the LSO station DCS never creates Supercarrier's PLAT overlay (PLATCameraUI.dlg),
// so the picture has no text. The viewer then draws the same overlay from the values the hook
// publishes (Header::sym): same font (DCS's AnonymousPro), same layout (PLATCameraUI.lua's
// setPLATCameraBounds / adjustGate on its 1174 x 828 dialog, stretched over the PLAT image as DCS does).
constexpr int kOvlW = 1174, kOvlH = 828;
struct Overlay {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11BlendState* blend = nullptr;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    void* bits = nullptr;
    HFONT font = nullptr;
    DWORD fontPid = 0;
    LONG seq = -1;
    bool ready = false;
} g_ovl;

void OverlayRelease() {
    SafeRelease(g_ovl.srv);
    SafeRelease(g_ovl.tex);
    SafeRelease(g_ovl.ps);
    SafeRelease(g_ovl.blend);
    g_ovl.seq = -1;
    g_ovl.ready = false;
}

// DCS's own font file, found from the producer's executable (<DCS>\bin\DCS.exe or bin-mt).
void OverlayFont() {
    if (!HeaderValid()) return;
    const DWORD pid = app.header->producerPid;
    if (g_ovl.font && g_ovl.fontPid == pid) return;
    g_ovl.fontPid = pid;
    const wchar_t* face = L"Consolas";
    const std::wstring root = DcsRoot();
    if (!root.empty()) {
        const std::wstring ttf = root + L"\\dxgui\\skins\\fonts\\AnonymousPro-Regular.ttf";
        if (AddFontResourceExW(ttf.c_str(), FR_PRIVATE, nullptr) > 0) face = L"Anonymous Pro";
    }
    if (g_ovl.font) DeleteObject(g_ovl.font);
    g_ovl.font = CreateFontW(-90, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH, face);
    g_ovl.seq = -1;
}

bool OverlayEnsure() {
    if (g_ovl.ready) return true;
    if (!app.dev) return false;
    if (!g_ovl.dc) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = kOvlW;
        bi.bmiHeader.biHeight = -kOvlH;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g_ovl.dc = CreateCompatibleDC(nullptr);
        g_ovl.bmp = CreateDIBSection(g_ovl.dc, &bi, DIB_RGB_COLORS, &g_ovl.bits, nullptr, 0);
        if (!g_ovl.dc || !g_ovl.bmp || !g_ovl.bits) return false;
        g_ovl.oldBmp = SelectObject(g_ovl.dc, g_ovl.bmp);
        SetBkMode(g_ovl.dc, TRANSPARENT);
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kOvlW;
    td.Height = kOvlH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(app.dev->CreateTexture2D(&td, nullptr, &g_ovl.tex))) return false;
    if (FAILED(app.dev->CreateShaderResourceView(g_ovl.tex, nullptr, &g_ovl.srv))) return false;
    ID3DBlob* b = nullptr;
    if (FAILED(Compile("psOverlay", "ps_5_0", &b))) return false;
    HRESULT hr = app.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_ovl.ps);
    SafeRelease(b);
    if (FAILED(hr)) return false;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(app.dev->CreateBlendState(&bd, &g_ovl.blend))) return false;
    g_ovl.ready = true;
    return true;
}

// One text field (PLATCameraUI.dlg Static: left aligned, 5 px inset, clipped to its box). Red = ink;
// at night a 2 px black blur around it (green = halo coverage).
void OverlayText(const char* text, int x, int y, int w, int h, bool night) {
    wchar_t t[32];
    int n = 0;
    for (; text[n] && n < 31; ++n) t[n] = static_cast<wchar_t>(static_cast<unsigned char>(text[n]));
    t[n] = 0;
    if (!n) return;
    const UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    if (night) {
        SetTextColor(g_ovl.dc, RGB(0, 255, 0));
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                if (dx * dx + dy * dy > 5 || (!dx && !dy)) continue;
                RECT r{x + 5 + dx, y + dy, x + w + dx, y + h + dy};
                DrawTextW(g_ovl.dc, t, n, &r, fmt);
            }
    }
    SetTextColor(g_ovl.dc, RGB(255, 0, 0));
    RECT r{x + 5, y, x + w, y + h};
    DrawTextW(g_ovl.dc, t, n, &r, fmt);
}

void OverlayRedraw(const lsoshare::Header::Symbology& y) {
    memset(g_ovl.bits, 0, static_cast<size_t>(kOvlW) * kOvlH * 4);
    HGDIOBJ oldFont = SelectObject(g_ovl.dc, g_ovl.font);
    const bool night = y.night != 0;
    // setPLATCameraBounds(0, 0, 1174, 828)
    const int w = kOvlW, h = kOvlH, symbol = 57;
    const int col0 = w / 2 - 440, col1 = w / 2 + 3 * symbol;
    const int row0 = 10, row1 = row0 + 70, row3 = h - 136;
    OverlayText(y.date, col0, row0, 400, 79, night);
    OverlayText(y.time, col0, row1, 400, 90, night);
    OverlayText(y.speed, col0, row3, 256, 90, night);
    if (y.statusVisible) OverlayText(y.status, col1, row0, 90, 90, night);
    OverlayText(y.carrierId, col1 + 3 * symbol, row0, 100, 90, night);
    OverlayText(y.deckWind, col1 + 72, row1, 100, 90, night);
    if (y.waveoff) OverlayText("W", col0 + 5 * symbol, row3, 100, 90, night);
    OverlayText(y.distance, w / 2, row3, 256, 90, night);
    OverlayText(y.sink, col1 + 3 * symbol, row3, 256, 90, night);
    SelectObject(g_ovl.dc, oldFont);
    // adjustGate(fov, distance, optimal_glissade_delta_h): the crosshair with its gap
    const double pi = 3.14159265358979323846;
    const double fov = y.gateFovDeg > 1 ? y.gateFovDeg : 17.0;
    const double dist = y.gateDistance < 35 ? 35.0 : y.gateDistance;
    const double tanHalf = std::tan(fov * 0.5 * pi / 180.0);
    const double wGate = w * std::tan(0.5 * pi / 180.0) / tanHalf;
    const double tanHalfV = tanHalf * h / w;
    const double dPix = h * (y.gateDeltaH / dist) / (2 * tanHalfV);
    const double hGate = wGate * h / w, centre = h / 2.0 - dPix;
    const int sz = 4;
    HBRUSH ink = CreateSolidBrush(RGB(255, 0, 0));
    auto box = [&](double x0, double y0, double bw, double bh) {
        RECT r{static_cast<LONG>(std::lround(x0)), static_cast<LONG>(std::lround(y0)), static_cast<LONG>(std::lround(x0 + bw)),
               static_cast<LONG>(std::lround(y0 + bh))};
        FillRect(g_ovl.dc, &r, ink);
    };
    box(0, centre - sz / 2.0, w / 2.0 - wGate / 2, sz);
    box(w / 2.0 + wGate / 2, centre - sz / 2.0, w / 2.0 - wGate / 2, sz);
    box(w / 2.0 - sz / 2.0, 0, sz, centre - hGate / 2);
    box(w / 2.0 - sz / 2.0, centre + hGate / 2, sz, h - centre - hGate / 2);
    DeleteObject(ink);
    GdiFlush();
    app.ctx->UpdateSubresource(g_ovl.tex, 0, nullptr, g_ovl.bits, kOvlW * 4, 0);
}

// True when the viewer should draw the overlay on the PLAT feed now.
bool OverlayWanted(const ViewState& v) {
    if (v.view != 1 || v.cam || !HeaderValid() || !app.set.symbology) return false;
    const auto& y = app.header->sym;
    return y.valid && !y.dcsOverlay;
}

bool OverlayPrepare() {
    OverlayFont();
    if (!g_ovl.font || !OverlayEnsure()) return false;
    const auto& y = app.header->sym;
    const LONG seq = y.seq;
    if (seq != g_ovl.seq) {
        MemoryBarrier();
        lsoshare::Header::Symbology copy;
        memcpy(&copy, const_cast<const lsoshare::Header::Symbology*>(&y), sizeof(copy));
        OverlayRedraw(copy);
        g_ovl.seq = seq;
    }
    return true;
}

void OverlayDraw() {
    if (!OverlayPrepare()) return;
    const float bf[4] = {0, 0, 0, 0};
    app.ctx->OMSetBlendState(g_ovl.blend, bf, 0xFFFFFFFF);
    app.ctx->PSSetShader(g_ovl.ps, nullptr, 0);
    app.ctx->PSSetShaderResources(3, 1, &g_ovl.srv);
    app.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    app.ctx->PSSetShaderResources(3, 1, &none);
    app.ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
}

// ---------------------------------------------------------------- carrier choice (written to the producer)

int CarrierCount() {
    if (!HeaderValid()) return 0;
    return static_cast<int>(std::min<uint32_t>(app.header->carrierCount, 8));
}

// The last request, until the producer has acted on it (it confirms once a second through `selected`):
// quick N presses step on from here instead of from the not yet updated selection.
int g_pendingCarrier = -1;
ULONGLONG g_pendingTick = 0;
// The carrier chosen in this viewer, for this DCS process: chosen again when a new mission clears the shared block
// (selectSeq is only written here, so any other value means it was cleared).
struct CarrierPick {
    std::string name;
    LONG seqWritten = 0;
    DWORD pid = 0;
} g_carrierPick;

void ChooseCarrier(int i) {
    if (!app.headerW || i < 0 || i >= CarrierCount()) return;
    const bool pending = g_pendingCarrier >= 0 && GetTickCount64() - g_pendingTick < 2500 && app.header->selected != g_pendingCarrier;
    if (i == (pending ? g_pendingCarrier : app.header->selected)) return;   // already there / on its way
    g_pendingCarrier = i;
    g_pendingTick = GetTickCount64();
    auto& c = app.headerW->control;
    memcpy(c.selectName, app.header->carriers[i].name, sizeof(c.selectName));
    MemoryBarrier();
    g_carrierPick.seqWritten = InterlockedIncrement(&c.selectSeq);
    g_carrierPick.name.assign(app.header->carriers[i].name, strnlen(app.header->carriers[i].name, sizeof(app.header->carriers[i].name)));
    g_carrierPick.pid = app.header->producerPid;
}

// A new mission in the same DCS cleared the shared block, the carrier chosen with it: choose it again as soon as
// DCS lists it (another DCS process: the choice is forgotten).
void CarrierTick() {
    if (!HeaderValid() || !app.headerW) return;
    if (app.header->producerPid != g_carrierPick.pid) {
        g_carrierPick = CarrierPick{};
        g_carrierPick.pid = app.header->producerPid;
        g_carrierPick.seqWritten = app.header->control.selectSeq;
        return;
    }
    if (g_carrierPick.name.empty() || app.header->control.selectSeq == g_carrierPick.seqWritten) return;
    for (int i = 0; i < CarrierCount(); ++i) {
        const auto& k = app.header->carriers[i];
        if (g_carrierPick.name != std::string(k.name, strnlen(k.name, sizeof(k.name)))) continue;
        auto& c = app.headerW->control;
        memcpy(c.selectName, k.name, sizeof(c.selectName));
        MemoryBarrier();
        g_carrierPick.seqWritten = InterlockedIncrement(&c.selectSeq);
        g_pendingCarrier = i;
        g_pendingTick = GetTickCount64();
        return;
    }
}

void StepCarrier(int dir) {
    const int n = CarrierCount();
    if (n <= 0) return;
    const bool pending = g_pendingCarrier >= 0 && GetTickCount64() - g_pendingTick < 2500 && app.header->selected != g_pendingCarrier;
    const int cur = pending ? g_pendingCarrier : app.header->selected;
    ChooseCarrier(((cur < 0 ? 0 : cur + dir) % n + n) % n);
}

// The picture of feed v into rtv at vp. The window and the recording canvas both draw through this.
void DrawFeed(const ViewState& v, ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, bool recording = false) {
    app.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    WriteParams(v, vp.Height, recording);
    app.ctx->RSSetViewports(1, &vp);
    app.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    app.ctx->IASetInputLayout(nullptr);
    app.ctx->VSSetShader(app.vs, nullptr, 0);
    BindSources(v);
    if (StationOverlay(v) && OverlayPrepare()) app.ctx->PSSetShaderResources(3, 1, &g_ovl.srv);
    app.ctx->PSSetSamplers(0, 1, &app.sampler);
    app.ctx->PSSetShader(app.ps, nullptr, 0);
    app.ctx->Draw(3, 0);
    UnbindSources();
    ID3D11ShaderResourceView* none = nullptr;
    app.ctx->PSSetShaderResources(3, 1, &none);
    if (OverlayWanted(v)) OverlayDraw();
}

// An iw x ih picture centred in a cw x ch area, aspect kept.
D3D11_VIEWPORT Fit(float cw, float ch, float iw, float ih) {
    const float scale = std::min(cw / iw, ch / ih);
    return D3D11_VIEWPORT{(cw - iw * scale) / 2, (ch - ih * scale) / 2, iw * scale, ih * scale, 0, 1};
}

// ---------------------------------------------------------------- video: GPU side

void ChannelRelease(Channel& c) {
    SafeRelease(c.canvasRtv);
    SafeRelease(c.canvas);
    for (auto*& t : c.staging) SafeRelease(t);
    c.inFlight.clear();
    c.gpu = false;
}

void OutputsRelease();  // outputs.inc
void FeedsRelease();    // console.inc
void RecRelease() {
    ChannelRelease(g_rec.main);
    FeedsRelease();
    for (auto& c : g_rec.extra) ChannelRelease(*c);
    ChannelRelease(g_rec.comp);
    for (auto& t : g_streams) ChannelRelease(t.ch);
    OutputsRelease();
    SafeRelease(g_rec.psCopy);
    SafeRelease(g_rec.psBadge);
    SafeRelease(g_rec.blend);
    SafeRelease(g_rec.ctx1);
    SafeRelease(g_rec.replaySrv);
    SafeRelease(g_rec.replayTex);
    g_rec.shown = -1;
    SafeRelease(g_badge.srv);
    SafeRelease(g_badge.tex);
    g_badge.text.clear();
    g_rec.gpu = false;
}

D3D11_TEXTURE2D_DESC CanvasDesc(const video::Engine::Config& c) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = c.w;
    td.Height = c.h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return td;
}

// A channel's picture size: its recording engine's, else (a stream's own picture) its encoder's.
void ChannelSize(const Channel& ch, int* w, int* h) {
    *w = ch.ok ? ch.eng.config().w : ch.live.config().w;
    *h = ch.ok ? ch.eng.config().h : ch.live.config().h;
}
bool ChannelUsed(const Channel& ch) { return ch.ok || ch.live.wanted() || ch.out != nullptr; }

// A channel's canvas and read-back textures.
bool ChannelEnsureGpu(Channel& ch) {
    if (ch.gpu) return true;
    if (!app.dev || !ChannelUsed(ch)) return false;
    video::Engine::Config size;
    ChannelSize(ch, &size.w, &size.h);
    const D3D11_TEXTURE2D_DESC td = CanvasDesc(size);
    bool ok = SUCCEEDED(app.dev->CreateTexture2D(&td, nullptr, &ch.canvas)) &&
              SUCCEEDED(app.dev->CreateRenderTargetView(ch.canvas, nullptr, &ch.canvasRtv));
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto*& t : ch.staging) ok = ok && SUCCEEDED(app.dev->CreateTexture2D(&sd, nullptr, &t));
    if (!ok) {
        ChannelRelease(ch);
        return false;
    }
    ch.gpu = true;
    return true;
}

// The objects the channels and the replay share: shaders, blend state, the replay texture.
bool RecEnsureGpu() {
    if (g_rec.gpu) return true;
    if (!app.dev || !g_rec.ok) return false;
    D3D11_TEXTURE2D_DESC rd = CanvasDesc(g_rec.main.eng.config());
    rd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bool ok = SUCCEEDED(app.dev->CreateTexture2D(&rd, nullptr, &g_rec.replayTex)) &&
              SUCCEEDED(app.dev->CreateShaderResourceView(g_rec.replayTex, nullptr, &g_rec.replaySrv));
    for (auto [entry, ps] : {std::pair<const char*, ID3D11PixelShader**>{"psCopy", &g_rec.psCopy}, {"psBadge", &g_rec.psBadge}}) {
        ID3DBlob* b = nullptr;
        ok = ok && SUCCEEDED(Compile(entry, "ps_5_0", &b)) &&
             SUCCEEDED(app.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, ps));
        SafeRelease(b);
    }
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(app.dev->CreateBlendState(&bd, &g_rec.blend));
    ok = ok && SUCCEEDED(app.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_rec.ctx1)));
    if (!ok) {
        RecRelease();
        return false;
    }
    g_rec.gpu = true;
    return true;
}

// ---- stills: PNG files, written on a worker thread (as "<path>.part", renamed when complete)
std::atomic<int> g_stillsSaved{0}, g_stillsFailed{0};
bool WritePng(const std::wstring& path, const std::vector<uint8_t>& bgra, int w, int h) {
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);  // 24-bit BGR: the canvas's alpha means nothing
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) memcpy(&rgb[i * 3], &bgra[i * 4], 3);
    IWICImagingFactory* f = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    const std::wstring part = path + L".part";
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->CreateStream(&st);
    if (SUCCEEDED(hr)) hr = st->InitializeFromFilename(part.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, nullptr);
    if (SUCCEEDED(hr)) hr = fr->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = fr->SetSize(w, h);
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&pf);
    if (SUCCEEDED(hr) && pf != GUID_WICPixelFormat24bppBGR) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = fr->WritePixels(h, static_cast<UINT>(w) * 3, static_cast<UINT>(rgb.size()), rgb.data());
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    SafeRelease(fr);
    SafeRelease(enc);
    SafeRelease(st);
    SafeRelease(f);
    if (SUCCEEDED(hr) && MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(part.c_str());
    return false;
}
void StillWrite(const std::wstring& path, const std::vector<uint8_t>& px, int w, int h) {
    std::thread([path, px, w, h] {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        ++(WritePng(path, px, w, h) ? g_stillsSaved : g_stillsFailed);
        if (com) CoUninitialize();
    }).detach();
}

// Once per rendered frame for a channel: collects finished read-backs and, every 1/fps s while it has a picture,
// draws it into the canvas (draw(rtv, w, h)).
template <class Draw>
void ChannelCaptureWith(Channel& ch, bool have, Draw draw) {
    if (!ChannelUsed(ch) || !RecEnsureGpu() || !ChannelEnsureGpu(ch)) return;
    struct {
        int w, h, fps;
    } c{0, 0, ch.ok ? ch.eng.config().fps : ch.live.config().fps};
    ChannelSize(ch, &c.w, &c.h);
    while (!ch.inFlight.empty()) {
        const int i = ch.inFlight.front();
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = app.ctx->Map(ch.staging[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        ch.inFlight.pop_front();
        if (FAILED(hr)) continue;
        std::vector<uint8_t> px(static_cast<size_t>(c.w) * c.h * 4);
        for (int y = 0; y < c.h; ++y)
            memcpy(px.data() + static_cast<size_t>(y) * c.w * 4, static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                   static_cast<size_t>(c.w) * 4);
        app.ctx->Unmap(ch.staging[i], 0);
        if (ch.keep) *ch.keep = px;
        if (ch.out) ch.out(px, c.w, c.h);
        if (!ch.still.empty() && ch.stagingT[i] >= ch.stillAfter) {
            StillWrite(ch.still, px, c.w, c.h);
            ch.still.clear();
        }
        if (ch.live.running()) ch.live.push(px.data(), ch.stagingT[i]);  // black box / stream
        if (ch.ok) ch.eng.push(std::move(px), ch.stagingT[i]);
    }
    const double now = video::Now(), step = 1.0 / c.fps;
    if (!have || now < ch.nextCapture) return;
    ch.nextCapture = now - ch.nextCapture > step ? now + step : ch.nextCapture + step;
    int slot = -1;
    for (int i = 0; i < Channel::kStaging && slot < 0; ++i)
        if (std::find(ch.inFlight.begin(), ch.inFlight.end(), i) == ch.inFlight.end()) slot = i;
    if (slot < 0) return;  // the GPU is behind: skip this frame
    const float black[4] = {0, 0, 0, 1};
    app.ctx->ClearRenderTargetView(ch.canvasRtv, black);
    draw(ch.canvasRtv, c.w, c.h);
    app.ctx->CopyResource(ch.staging[slot], ch.canvas);
    ch.stagingT[slot] = now;
    ch.inFlight.push_back(slot);
    ch.lastFrame = now;
}
// Picture v, fitted into the channel's canvas.
void ChannelCapture(Channel& ch, const ViewState& v) {
    ChannelCaptureWith(ch, v.main != nullptr, [&](ID3D11RenderTargetView* rtv, int w, int h) {
        DrawFeed(v, rtv, Fit(static_cast<float>(w), static_cast<float>(h), ViewAspect(v), 1.0f), true);
    });
}

// ---- streams: their pictures
int ConsoleSourceOf(const std::wstring& id);  // console.inc
// A stream's source name: "a" / "b" = what monitor A / B shows, else that source; -1 = none.
int StreamSource(const std::wstring& k) {
    if (k == L"a") return ProgramSource();
    if (k == L"b") return g_con.secondKey.empty() ? -1 : ConsoleSourceOf(g_con.secondKey);
    return ConsoleSourceOf(k);
}
// A camera source a stream needs rendered (its own picture, or a composite's tile).
bool StreamWants(int s) {
    if (s < 0) return false;
    for (const auto& t : g_streams) {
        if (!t.on || !t.own()) continue;
        if (!t.composite()) {
            if (StreamSource(t.source) == s) return true;
            continue;
        }
        for (const auto& k : t.tiles)
            if (StreamSource(k) == s) return true;
    }
    return false;
}
// Tile k of n in a w x h composite: one whole; two side by side; three or four in a 2 x 2 grid (a third centred
// under the first two).
D3D11_VIEWPORT TileCell(int k, int n, float w, float h) {
    if (n <= 1) return D3D11_VIEWPORT{0, 0, w, h, 0, 1};
    if (n == 2) return D3D11_VIEWPORT{k * w / 2, 0, w / 2, h, 0, 1};
    const float cw = w / 2, chh = h / 2;
    const float x = n == 3 && k == 2 ? w / 4 : (k % 2) * cw;
    return D3D11_VIEWPORT{x, (k / 2) * chh, cw, chh, 0, 1};
}
// The pictures of these sources ("a", "b", "plat" ...; none for a source not there).
std::vector<ViewState> TileViews(const std::vector<std::wstring>& keys) {
    std::vector<ViewState> vs;
    for (const auto& k : keys) {
        const int s = StreamSource(k);
        vs.push_back(s >= 0 ? ViewFor(s) : ViewState{});
    }
    return vs;
}
// Draws them into a w x h target: one fitted to it, or each in its tile (TileCell).
void DrawTiles(const std::vector<ViewState>& vs, bool tiled, ID3D11RenderTargetView* rtv, int w, int h) {
    const int n = static_cast<int>(vs.size());
    for (int k = 0; k < n; ++k) {
        if (!vs[k].main) continue;
        const D3D11_VIEWPORT cell = tiled ? TileCell(k, n, static_cast<float>(w), static_cast<float>(h))
                                          : D3D11_VIEWPORT{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        D3D11_VIEWPORT vp = Fit(cell.Width, cell.Height, ViewAspect(vs[k]), 1.0f);
        vp.TopLeftX += cell.TopLeftX;
        vp.TopLeftY += cell.TopLeftY;
        DrawFeed(vs[k], rtv, vp, true);
    }
}
// The streams with a picture of their own: drawn and handed to their encoders (black while a source has none).
void StreamCapture() {
    for (auto& t : g_streams) {
        if (!t.on || !t.own()) continue;
        const std::vector<ViewState> vs = TileViews(t.composite() ? t.tiles : std::vector<std::wstring>{t.source});
        ChannelCaptureWith(t.ch, true, [&](ID3D11RenderTargetView* rtv, int w, int h) { DrawTiles(vs, t.composite(), rtv, w, h); });
    }
}

// A background channel records its source while something else is shown: an extra camera that is rendering,
// or the PLAT while an extra camera is shown.
bool ChannelActive(const Channel& c) {
    if (!RecArmed(c.source)) return false;
    if (c.source >= kFeeds) return CameraRendering(c.source - kFeeds);
    return true;
}

#include "outputs.inc"

// ---- composite recording: the sources with REC on in one 1920 x 1080 picture (DrawTiles, as a composite stream).
// Its channel runs while composite mode is on (a pass's pre-roll comes from its ring) or a recording or still uses it.
bool CompInUse() {
    for (const auto& r : g_rec.recs)
        if (std::find(r.extra.begin(), r.extra.end(), &g_rec.comp) != r.extra.end()) return true;
    return !g_rec.comp.still.empty();
}
bool CompEnsure() {
    Channel& ch = g_rec.comp;
    if (ch.ok || !g_rec.ok) return ch.ok;
    video::Engine::Config c = g_engCfg;
    c.w = kCompositeW, c.h = kCompositeH;
    c.ringSeconds = g_rec.preroll + 6;  // only the pre-roll comes from this ring
    // the recording bitrate ([video] bitrate_kbps, default 2.2 Mbit/s at 1024 x 768) scaled to the composite's pixels
    const double scale = static_cast<double>(kCompositeW) * kCompositeH / std::max(1.0, static_cast<double>(g_engCfg.w) * g_engCfg.h);
    c.bitrate = static_cast<int>(std::clamp(g_engCfg.bitrate * scale, 1e6, 100e6));
    ch.source = -2;
    ch.suffix = L"COMPOSITE";
    ch.ok = ch.eng.start(c);
    return ch.ok;
}
// The sources it tiles: those a recording would give a file each (REC on, a picture), by source number, the first
// four. Fixed while a recording or still uses the composite, so its layout does not change in the middle.
void CompTilesUpdate() {
    if (CompInUse()) return;
    const double now = video::Now();
    std::vector<int> t;
    for (const auto& c : g_rec.extra)
        if (c->ok && ChannelActive(*c) && now - c->lastFrame <= 2.0) t.push_back(c->source);
    std::sort(t.begin(), t.end());
    if (t.size() > static_cast<size_t>(kMaxTiles)) t.resize(kMaxTiles);
    g_rec.compTiles = t;
}
void CompCapture() {
    const bool on = g_rec.composite || CompInUse();
    if (on) CompEnsure();
    if (!g_rec.comp.ok) return;
    std::vector<ViewState> vs;
    if (on) {
        CompTilesUpdate();
        for (int s : g_rec.compTiles) vs.push_back(ViewFor(s));
    }
    ChannelCaptureWith(g_rec.comp, !vs.empty(), [&](ID3D11RenderTargetView* rtv, int w, int h) { DrawTiles(vs, true, rtv, w, h); });
}

void StillTick();
void FeedsCapture();  // console.inc: monitor B's and the previews' replay rings
// The picture as shown into the main channel, and each background channel's own camera.
void RecCapture(const ViewState& v) {
    StillTick();
    ChannelCapture(g_rec.main, v);
    for (auto& c : g_rec.extra) {
        if (!ChannelActive(*c)) continue;
        const ViewState xv = ViewFor(c->source);
        if (xv.main) ChannelCapture(*c, xv);
    }
    CompCapture();
    StreamCapture();
    OutputsCapture();
    FeedsCapture();
}

// The replay frame at the playback position, fitted into the pane. Advances the playback clock.
bool RecDrawReplay(float cw, float ch, D3D11_VIEWPORT* vp, float ox = 0, float oy = 0) {
    if (!RecEnsureGpu() || g_rec.frames.empty()) return false;
    const auto& c = g_rec.main.eng.config();
    const double now = video::Now();
    const double t0 = g_rec.frames.front()->t, dur = g_rec.frames.back()->t - t0;
    if (!g_rec.paused) g_rec.pos += (now - g_rec.lastTick) * g_rec.speed;
    g_rec.lastTick = now;
    if (g_rec.pos >= dur) {
        g_rec.pos = dur;
        g_rec.paused = true;
    }
    g_rec.pos = std::max(0.0, g_rec.pos);
    const auto it = std::upper_bound(g_rec.frames.begin(), g_rec.frames.end(), t0 + g_rec.pos + 1e-6,
                                     [](double t, const video::FramePtr& f) { return t < f->t; });
    const int idx = std::max(0, static_cast<int>(it - g_rec.frames.begin()) - 1);
    if (idx != g_rec.shown) {
        int w = 0, h = 0;
        if (g_rec.jpeg.decode(g_rec.frames[idx]->jpeg, &g_rec.decoded, &w, &h) && w == c.w && h == c.h)
            app.ctx->UpdateSubresource(g_rec.replayTex, 0, nullptr, g_rec.decoded.data(), static_cast<UINT>(w) * 4, 0);
        g_rec.shown = idx;
    }
    *vp = Fit(cw, ch, static_cast<float>(c.w), static_cast<float>(c.h));
    vp->TopLeftX += ox;
    vp->TopLeftY += oy;
    app.ctx->OMSetRenderTargets(1, &app.rtv, nullptr);
    app.ctx->RSSetViewports(1, vp);
    app.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    app.ctx->IASetInputLayout(nullptr);
    app.ctx->VSSetShader(app.vs, nullptr, 0);
    app.ctx->PSSetShader(g_rec.psCopy, nullptr, 0);
    app.ctx->PSSetShaderResources(0, 1, &g_rec.replaySrv);
    app.ctx->PSSetSamplers(0, 1, &app.sampler);
    app.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    app.ctx->PSSetShaderResources(0, 1, &none);
    return true;
}

std::wstring Clock(double s) {
    const int t = static_cast<int>(std::max(0.0, s));
    wchar_t b[16];
    swprintf_s(b, L"%d:%02d", t / 60, t % 60);
    return b;
}

// Replay speed as shown: 1/8x ... 1x ... 8x.
std::wstring SpeedLabel(double speed) {
    return speed < 1.0 ? L"1/" + std::to_wstring(static_cast<int>(std::lround(1.0 / speed))) + L"x"
                       : std::to_wstring(static_cast<int>(std::lround(speed))) + L"x";
}

bool BadgeEnsure() {
    if (!g_badge.dc) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = Badge::kW;
        bi.bmiHeader.biHeight = -Badge::kH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g_badge.dc = CreateCompatibleDC(nullptr);
        g_badge.bmp = CreateDIBSection(g_badge.dc, &bi, DIB_RGB_COLORS, &g_badge.bits, nullptr, 0);
        if (!g_badge.dc || !g_badge.bmp || !g_badge.bits) return false;
        g_badge.old = SelectObject(g_badge.dc, g_badge.bmp);
        g_badge.font = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SetBkMode(g_badge.dc, TRANSPARENT);
    }
    if (g_badge.tex) return true;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = Badge::kW;
    td.Height = Badge::kH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(app.dev->CreateTexture2D(&td, nullptr, &g_badge.tex)) ||
        FAILED(app.dev->CreateShaderResourceView(g_badge.tex, nullptr, &g_badge.srv))) {
        SafeRelease(g_badge.tex);
        return false;
    }
    g_badge.text.clear();
    return true;
}

// Text in a dark translucent box at (x, y) of the back buffer.
void BadgeDraw(const std::wstring& text, COLORREF color, float x, float y) {
    if (!BadgeEnsure()) return;
    if (text != g_badge.text || color != g_badge.color) {
        g_badge.text = text;
        g_badge.color = color;
        auto* px = static_cast<uint8_t*>(g_badge.bits);
        memset(px, 0, static_cast<size_t>(Badge::kW) * Badge::kH * 4);
        HGDIOBJ of = SelectObject(g_badge.dc, g_badge.font);
        SetTextColor(g_badge.dc, RGB(255, 255, 255));
        RECT r{12, 0, Badge::kW, Badge::kH};
        DrawTextW(g_badge.dc, text.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SIZE sz{};
        GetTextExtentPoint32W(g_badge.dc, text.c_str(), static_cast<int>(text.size()), &sz);
        SelectObject(g_badge.dc, of);
        GdiFlush();
        const int box = std::min<int>(Badge::kW, sz.cx + 24);
        for (int yy = 0; yy < Badge::kH; ++yy)
            for (int xx = 0; xx < Badge::kW; ++xx) {
                uint8_t* p = px + (static_cast<size_t>(yy) * Badge::kW + xx) * 4;
                if (xx >= box) {
                    p[0] = p[1] = p[2] = p[3] = 0;
                    continue;
                }
                const float a = p[2] / 255.0f;  // text coverage (white text)
                p[0] = static_cast<uint8_t>(GetBValue(color) * a);
                p[1] = static_cast<uint8_t>(GetGValue(color) * a);
                p[2] = static_cast<uint8_t>(GetRValue(color) * a);
                p[3] = static_cast<uint8_t>(255 * (a + (1 - a) * 0.6f));
            }
        app.ctx->UpdateSubresource(g_badge.tex, 0, nullptr, g_badge.bits, Badge::kW * 4, 0);
    }
    const D3D11_VIEWPORT bv{std::floor(x), std::floor(y), static_cast<float>(Badge::kW), static_cast<float>(Badge::kH), 0, 1};
    const float bf[4] = {0, 0, 0, 0};
    app.ctx->RSSetViewports(1, &bv);
    app.ctx->OMSetBlendState(g_rec.blend, bf, 0xFFFFFFFF);
    app.ctx->VSSetShader(app.vs, nullptr, 0);
    app.ctx->PSSetShader(g_rec.psBadge, nullptr, 0);
    app.ctx->PSSetShaderResources(0, 1, &g_badge.srv);
    app.ctx->PSSetSamplers(0, 1, &app.sampler);
    app.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    app.ctx->PSSetShaderResources(0, 1, &none);
    app.ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
}

const wchar_t* ResultName(int r) {
    switch (r) {
        case 1: return L"TRAP";
        case 2: return L"BOLTER";
        case 3: return L"WAVE-OFF";
        case 5: return L"NOT AN APPROACH";
        default: return L"INCOMPLETE";
    }
}

// REC / REPLAY badge and the replay progress bar (window only, never in the recording).
void RecDrawBadges(const D3D11_VIEWPORT& vp, bool badge = true) {
    if (!g_rec.gpu) return;
    std::wstring text;
    COLORREF color = 0;
    if (g_rec.replay && !g_rec.frames.empty()) {
        const double dur = g_rec.frames.back()->t - g_rec.frames.front()->t;
        text = L"REPLAY  " + Clock(g_rec.pos) + L" / " + Clock(dur) + L"  " + SpeedLabel(g_rec.speed);
        if (g_rec.paused) text += g_rec.pos >= dur ? L"  END" : L"  PAUSED";
        color = RGB(255, 200, 80);
    }
    if (!text.empty() && badge) BadgeDraw(text, color, vp.TopLeftX + 10, vp.TopLeftY + 10);
    if (g_rec.replay && !g_rec.frames.empty() && g_rec.ctx1) {
        const double dur = std::max(1e-3, g_rec.frames.back()->t - g_rec.frames.front()->t);
        const LONG x0 = static_cast<LONG>(vp.TopLeftX + 10), x1 = static_cast<LONG>(vp.TopLeftX + vp.Width - 10);
        const LONG y1 = static_cast<LONG>(vp.TopLeftY + vp.Height - 10), y0 = y1 - 6;
        const D3D11_RECT track{x0, y0, x1, y1};
        const D3D11_RECT fill{x0, y0, x0 + static_cast<LONG>((x1 - x0) * std::clamp(g_rec.pos / dur, 0.0, 1.0)), y1};
        const float tc[4] = {0.25f, 0.25f, 0.28f, 1}, fc[4] = {1.0f, 0.78f, 0.31f, 1};
        g_rec.ctx1->ClearView(app.rtv, tc, &track, 1);
        if (fill.right > fill.left) g_rec.ctx1->ClearView(app.rtv, fc, &fill, 1);
    }
}

// ---------------------------------------------------------------- text

std::wstring Ascii(const char* p, size_t n) {
    std::wstring w;
    for (size_t i = 0; i < n && p[i]; ++i) w += static_cast<wchar_t>(static_cast<unsigned char>(p[i]));
    return w;
}

std::wstring Utf8W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// "CVN_72 011 Viper" for a pass (the parts that are known).
std::wstring PassLabel(const RecState::PassInfo& p) {
    std::wstring out;
    for (const std::string* s : {&p.carrier, &p.board, &p.pilot}) {
        if (s->empty()) continue;
        if (!out.empty()) out += L' ';
        out += Utf8W(*s);
    }
    return out.empty() ? L"pass" : out;
}
const RecState::PassInfo* LastPass();
bool Recording();
double OldestRecordingStart();

// A stream's upload rate, Mbit/s (measured over a second or more).
double StreamRate(int n) {
    auto& s = g_streams[n];
    const double now = video::Now();
    if (now - s.rateT > 1.0) {
        const long long bytes = s.pub.bytesSent();
        if (s.rateT > 0 && bytes >= s.rateBytes) s.rate = (bytes - s.rateBytes) * 8 / (now - s.rateT) / 1e6;
        s.rateBytes = bytes;
        s.rateT = now;
    }
    return s.rate;
}
// ---------------------------------------------------------------- window

// Resize edges / corners at a screen point of the main window, else HTCAPTION (drag to move). The window draws no
// frame (WM_NCCALCSIZE): the client area is the whole window.
LRESULT FrameHitTest(POINT screen) {
    if (IsZoomed(app.hwnd)) return HTCAPTION;
    RECT wr;
    GetWindowRect(app.hwnd, &wr);
    const bool l = screen.x < wr.left + kResizeBorder, r = screen.x >= wr.right - kResizeBorder;
    const bool t = screen.y < wr.top + kResizeBorder, b = screen.y >= wr.bottom - kResizeBorder;
    if (t && l) return HTTOPLEFT;
    if (t && r) return HTTOPRIGHT;
    if (b && l) return HTBOTTOMLEFT;
    if (b && r) return HTBOTTOMRIGHT;
    if (l) return HTLEFT;
    if (r) return HTRIGHT;
    if (t) return HTTOP;
    if (b) return HTBOTTOM;
    return HTCAPTION;
}

// ---------------------------------------------------------------- actions (shared by keys and the console)

void Changed() {
    SaveSettings();
    ConsolePush();
}

void SetCamera(int i, bool on, bool drop = true);

void SelectFeed(int feed) {
    if (feed < 0 || feed >= FeedCount()) return;
    if (g_con.progOff) {  // monitor A was switched off: any source key turns it on again
        g_con.progOff = false;
        if (feed == app.selected) {
            ResetAuto();
            Changed();
            return;
        }
    }
    if (feed == app.selected) return;
    if (feed >= kFeeds && !((g_cam.want >> (feed - kFeeds)) & 1)) SetCamera(feed - kFeeds, true);  // showing a camera switches it on
    UseGroup(GroupOf(feed));  // the controls now act on this picture's own levels
    app.selected = feed;
    ResetAuto();
    Changed();
}

void LoadPreset(int i) {
    if (!g_presets[i].saved) {
        Flash(PresetName(i) + L" is empty (right-click or Ctrl+F" + std::to_wstring(i + 5) + L" saves)");
        return;
    }
    const Preset& pr = g_presets[i];
    for (int g = 0; g < kAnalyses; ++g) {
        if (!pr.has[g]) continue;  // a slot from before cameras were in presets: the camera keeps its levels
        Settings& t = g == g_curGroup ? app.set : g_groupSet[g];
        const bool enabled = t.enabled;
        const Proc proc = t.proc;  // the processing mode is not part of a preset
        t = pr.s[g];
        t.enabled = enabled;
        t.proc = proc;
        t.symbology = true;
        if (g != g_curGroup) SaveGroupSettings(g, t);  // the one on program is saved by Changed()
    }
    ResetAuto();
    Flash(L"Loaded " + PresetName(i));
    Changed();
}

void StorePreset(int i) {
    if (i == kNightPreset) Flash(L"Night is a fixed preset");
    else SavePreset(i);
}

void ResetAdjustments() {
    Settings def;
    def.autoLevels = app.set.autoLevels;
    def.symbology = app.set.symbology;
    def.proc = app.set.proc;
    def.enabled = app.set.enabled;
    app.set = def;
    Changed();
}

// ---------------------------------------------------------------- recording and instant replay

int g_savedCount = 0, g_discardCount = 0, g_autoStarts = 0, g_bbSaved = 0, g_mishaps = 0, g_renamed = 0;  // for the self-test

void RecStatus(const std::wstring& text, ULONGLONG ms = 6000) {
    g_rec.status = text;
    g_rec.statusUntil = GetTickCount64() + ms;
}

std::wstring Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf_s(b, L"%04u-%02u-%02u %02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::wstring SafeName(std::wstring n) {
    for (auto& c : n)
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    while (!n.empty() && (n.back() == L' ' || n.back() == L'.')) n.pop_back();
    return n;
}

#include "recfiles.inc"

// folder\name.mp4, or folder\name (2).mp4 ... if that file exists. folder: the current mission's if not given. The
// path is listed for the storage limit.
std::wstring UniqueVideoPath(const std::wstring& name, const std::wstring& folder = {}) {
    const std::wstring dir = folder.empty() ? SessionFolder() : folder;
    if (!folder.empty()) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring p = dir + L"\\" + SafeName(name) + L".mp4";
    for (int i = 2; GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && i < 100; ++i)
        p = dir + L"\\" + SafeName(name) + L" (" + std::to_wstring(i) + L").mp4";
    StorageNote(p);
    return p;
}

// The black box ring and the stream's hand-off, on every channel's live encoder.
void LiveApply() {
    const double ring = g_bb.on ? g_bb.minutes * 60 : 0;
    std::vector<rtmp::Publisher*> ofA;  // the streams of monitor A take the main channel's units
    for (auto& t : g_streams)
        if (t.on && !t.own()) ofA.push_back(&t.pub);
    std::function<void(const live::UnitPtr&)> sink;
    if (!ofA.empty()) sink = [ofA](const live::UnitPtr& u) {
        for (auto* p : ofA) p->send(u);
    };
    g_rec.main.live.set(ring, sink);
    for (auto& c : g_rec.extra) c->live.set(ring, nullptr);
}


void RecInit() {
    auto get = [](const wchar_t* key, double def) {
        wchar_t b[64];
        GetPrivateProfileStringW(L"video", key, L"", b, 64, app.iniPath.c_str());
        return b[0] ? _wtof(b) : def;
    };
    video::Engine::Config c;
    c.w = std::clamp(static_cast<int>(get(L"width", 1024)), 320, 3840);
    c.h = std::clamp(static_cast<int>(get(L"height", 768)), 240, 2160);
    c.fps = std::clamp(static_cast<int>(get(L"fps", 30)), 10, 60);
    c.ringSeconds = std::clamp(get(L"replay_seconds", 45), 10.0, 300.0);
    c.quality = static_cast<float>(std::clamp(get(L"quality", 85), 30.0, 100.0) / 100.0);
    c.bitrate = static_cast<int>(std::clamp(get(L"bitrate_kbps", 0), 0.0, 100000.0) * 1000);
    wchar_t enc[32];
    GetPrivateProfileStringW(L"video", L"encoder", L"auto", enc, 32, app.iniPath.c_str());
    c.encoder = _wcsicmp(enc, L"hardware") == 0 ? 1 : (_wcsicmp(enc, L"software") == 0 ? 2 : 0);
    g_rec.preroll = std::clamp(get(L"preroll", 3), 0.0, 20.0);
    g_rec.postroll = std::clamp(get(L"postroll", 4), 0.0, 30.0);
    g_rec.replayDefault = std::clamp(get(L"replay_default", 30), 5.0, c.ringSeconds);
    wchar_t fmt[200];
    GetPrivateProfileStringW(L"video", L"filename", L"{date} {time} {callsign} Case {case} {grade}", fmt, 200, app.iniPath.c_str());
    g_rec.nameFormat = fmt;
    g_rec.composite = GetPrivateProfileIntW(L"video", L"composite", 0, app.iniPath.c_str()) != 0;
    StorageReadSettings();
    g_rec.ok = g_rec.main.ok = g_rec.main.eng.start(c);
    g_engCfg = g_rec.main.eng.config();
    live::Live::Config lc;
    lc.w = g_engCfg.w, lc.h = g_engCfg.h, lc.fps = g_engCfg.fps;
    lc.bitrate = g_liveBitrate;
    lc.encoder = g_engCfg.encoder;
    g_rec.main.live.configure(lc);
    LiveApply();
}

// Recording channels: one per source with its REC switch on (and a picture). The main channel (the picture
// as shown) feeds the instant replay, the black box and the stream; files come from these.
void EnsureChannels() {
    if (!g_rec.ok) return;
    auto ensure = [](int source, const std::wstring& suffix) {
        for (const auto& c : g_rec.extra)
            if (c->source == source) return;
        auto ch = std::make_unique<Channel>();
        ch->source = source;
        ch->suffix = suffix;
        video::Engine::Config c = g_engCfg;
        c.ringSeconds = g_rec.preroll + 6;  // only the pre-roll comes from this ring
        ch->ok = ch->eng.start(c);
        ch->live.configure(g_rec.main.live.config());
        g_rec.extra.push_back(std::move(ch));
        LiveApply();
    };
    for (int s = 0; s < FeedCount(); ++s) {
        if (!RecArmed(s)) continue;
        if (s >= kFeeds ? CameraRendering(s - kFeeds) : ViewFor(s).main != nullptr) ensure(s, SourceTag(s));
    }
}

RecState::PassInfo* FindPass(uint32_t id) {
    for (auto& p : g_rec.passes)
        if (p.id == id) return &p;
    return nullptr;
}

bool Recording() { return !g_rec.recs.empty(); }
bool ManualRecording() {
    for (const auto& r : g_rec.recs)
        if (r.manual) return true;
    return false;
}
double OldestRecordingStart() {
    double t = 1e300;
    for (const auto& r : g_rec.recs) t = std::min(t, r.start);
    return t;
}

// The channels a recording or a still takes now: every source with REC on that is delivering pictures (a file
// each), or in composite mode the composite of them (one file).
std::vector<Channel*> TakeChannels() {
    const double now = video::Now();
    std::vector<Channel*> out;
    if (g_rec.composite) {
        if (g_rec.comp.ok && !g_rec.compTiles.empty() && now - g_rec.comp.lastFrame <= 2.0) out.push_back(&g_rec.comp);
        return out;
    }
    for (auto& c : g_rec.extra)
        if (c->ok && ChannelActive(*c) && now - c->lastFrame <= 2.0) out.push_back(c.get());
    return out;
}

// Starts r on every channel TakeChannels gives.
void RecBegin(RecState::Recording& r) {
    for (Channel* c : TakeChannels()) {
        if (r.folder.empty()) r.folder = SessionFolder();
        const std::wstring path = UniqueVideoPath(r.base + L" " + c->suffix, r.folder);
        if (r.path.empty()) r.path = path;
        r.paths.push_back(path);
        c->eng.beginRecording(r.key, path, r.start);
        r.extra.push_back(c);
    }
}

void RecStartManual() {
    if (!g_rec.ok || ManualRecording()) return;
    if (BenchActive()) {
        RecStatus(L"Benchmark running: not recorded", 8000);
        return;
    }
    RecState::Recording r;
    r.manual = true;
    r.key = 0;
    r.stamp = Stamp();
    r.base = L"ILARTS " + r.stamp;
    r.start = video::Now();
    RecBegin(r);
    if (r.extra.empty()) {
        RecStatus(L"Nothing to record: switch REC on for a source (SOURCE SELECT)", 10000);
        return;
    }
    g_rec.recs.push_back(r);
    StorageRequest();  // room for it (and every other file being written)
}

// A still of every source with REC on: one PNG each of its recorded picture (the next one drawn), named like a
// manual recording plus DCS's mission clock when the hook gives it: "ILARTS 2026-11-16 14-03-22 PLAT DCS 09-30-12.png".
std::set<std::wstring> g_stillPaths;  // handed out (a file may not be on disk yet)
bool StillReady() { return !TakeChannels().empty(); }  // a source with REC on has a picture
void StillTake() {
    if (!g_rec.ok) {
        RecStatus(L"Video unavailable on this PC");
        return;
    }
    for (const auto& c : g_rec.extra)
        if (!c->still.empty()) return;  // the last one is still being taken (a held key)
    if (!g_rec.comp.still.empty()) return;
    std::wstring clock;
    if (HeaderValid() && app.header->sym.valid) {
        clock = Ascii(app.header->sym.time, sizeof(app.header->sym.time));
        for (auto& ch : clock)
            if (ch == L':') ch = L'-';
    }
    const std::wstring stamp = Stamp();
    const double now = video::Now();
    std::wstring names, dir;
    unsigned long long bytes = 0;
    for (Channel* c : TakeChannels()) {
        if (dir.empty()) dir = StillsFolder();
        const std::wstring name = SafeName(L"ILARTS " + stamp + L" " + c->suffix + (clock.empty() ? L"" : L" DCS " + clock));
        std::wstring p = dir + L"\\" + name + L".png";
        for (int i = 2; (g_stillPaths.count(p) || GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) && i < 100; ++i)
            p = dir + L"\\" + name + L" (" + std::to_wstring(i) + L").png";
        g_stillPaths.insert(p);
        StorageNote(p);
        int w = 0, h = 0;
        ChannelSize(*c, &w, &h);
        bytes += static_cast<unsigned long long>(w) * h * 3;  // at most (24-bit, uncompressed)
        c->still = p;
        c->stillAfter = now;
        names += (names.empty() ? L"" : L", ") + c->suffix;
    }
    if (bytes) StorageRequest(bytes);
    RecStatus(names.empty() ? L"No still: switch REC on for a source (SOURCE SELECT)" : L"Still: " + names, names.empty() ? 10000 : 4000);
}
// Every frame: a still whose source stopped drawing is given up; a file that could not be written is reported.
void StillTick() {
    const double now = video::Now();
    auto giveUp = [&](Channel& c) {
        if (!c.still.empty() && now - c.stillAfter > 3.0) {
            c.still.clear();
            RecStatus(L"No still of " + c.suffix + L": no picture", 8000);
        }
    };
    for (auto& c : g_rec.extra) giveUp(*c);
    giveUp(g_rec.comp);
    static int failed = 0, saved = 0;
    if (g_stillsFailed != failed) {
        failed = g_stillsFailed;
        RecStatus(L"Still not saved: cannot write to " + StillsFolder(false), 10000);
    }
    if (g_stillsSaved != saved) {
        saved = g_stillsSaved;
        g_storage.requested = true;
    }
}

// One file per pass (and per camera), from preroll before it started (the ring) until postroll after it ended.
void RecStartPass(const RecState::PassInfo& p) {
    if (!g_rec.ok) return;
    if (BenchActive()) {
        RecStatus(L"Benchmark running: pass not recorded", 8000);
        return;
    }
    for (const auto& r : g_rec.recs)
        if (!r.manual && r.pass == p.id) return;
    RecState::Recording r;
    r.pass = p.id;
    r.key = static_cast<int>(p.id);
    r.stamp = Stamp();
    r.base = L"Pass " + r.stamp + L" " + std::to_wstring(p.id);
    r.start = video::Now() - g_rec.preroll;
    RecBegin(r);
    if (r.extra.empty()) {
        RecStatus(L"Pass not recorded: no source has REC on", 10000);
        return;
    }
    g_rec.recs.push_back(r);
    StorageRequest();
    ++g_autoStarts;
}

// A pass recording's file name, from [video] filename. Tokens: {date} {time} {callsign} {case} {grade}
// {result} {wire} {board} {type} {pilot} {carrier} {pass}. {grade} is the LSO's grade (with the wire) when
// DCS gave one, else the result (TRAP, BOLTER, WAVE-OFF). An unknown case leaves "Case {case}" out.
std::wstring PassFileName(const std::wstring& stamp, const RecState::PassInfo* p, bool ended) {
    const std::wstring result = ResultName(ended && p ? p->result : 0);
    std::wstring grade = result;
    if (p && !p->grade.empty()) {
        grade = Utf8W(p->grade);
        if (!p->wire.empty()) grade += L" wire " + Utf8W(p->wire);
    }
    std::wstring callsign = p ? Utf8W(!p->callsign.empty() ? p->callsign : p->pilot) : std::wstring();
    if (callsign.empty()) callsign = L"pass";
    const int cs = p ? p->caseNo : 0;
    std::wstring out = g_rec.nameFormat;
    auto rep = [&](const std::wstring& token, const std::wstring& value) {
        for (size_t at; (at = out.find(token)) != std::wstring::npos;) out.replace(at, token.size(), value);
    };
    if (cs < 1 || cs > 3) rep(L"Case {case}", L"");
    rep(L"{case}", cs == 1 ? L"I" : cs == 2 ? L"II" : cs == 3 ? L"III" : L"");
    rep(L"{date}", stamp.substr(0, std::min<size_t>(10, stamp.size())));
    rep(L"{time}", stamp.size() > 11 ? stamp.substr(11) : std::wstring());
    rep(L"{callsign}", callsign);
    rep(L"{grade}", grade);
    rep(L"{result}", result);
    rep(L"{wire}", p ? Utf8W(p->wire) : std::wstring());
    rep(L"{board}", p ? Utf8W(p->board) : std::wstring());
    rep(L"{type}", p ? Utf8W(p->type) : std::wstring());
    rep(L"{pilot}", p ? Utf8W(p->pilot) : std::wstring());
    rep(L"{carrier}", p ? Utf8W(p->carrier) : std::wstring());
    rep(L"{pass}", p ? std::to_wstring(p->id) : std::wstring());
    std::wstring tidy;  // single spaces, none at the ends
    for (wchar_t c : out)
        if (c != L' ' || (!tidy.empty() && tidy.back() != L' ')) tidy += c;
    while (!tidy.empty() && tidy.back() == L' ') tidy.pop_back();
    return tidy.empty() ? stamp : tidy;
}

// Manual recordings keep their name; a pass is named by PassFileName. A pass that turned out not to be an
// approach is deleted.
void RecStopAt(size_t i) {
    const RecState::Recording r = g_rec.recs[i];
    g_rec.recs.erase(g_rec.recs.begin() + static_cast<std::ptrdiff_t>(i));
    std::wstring base;
    bool drop = false;
    if (!r.manual) {
        const RecState::PassInfo* p = FindPass(r.pass);
        const bool ended = p && !p->active && p->result != 0;
        if (ended && p->result == 5) {
            drop = true;
            RecStatus(L"Not an approach: recording dropped");
        } else {
            base = PassFileName(r.stamp, p, ended);
        }
    }
    auto final = [&](const std::wstring& suffix) {
        if (drop) return std::wstring(L"-");
        if (base.empty()) return std::wstring();
        return UniqueVideoPath(suffix.empty() ? base : base + L" " + suffix, r.folder);
    };
    for (Channel* c : r.extra) c->eng.endRecording(r.key, final(c->suffix));
    if (!r.manual && !drop) {
        RecState::Named n;
        n.pass = r.pass;
        n.stamp = r.stamp;
        n.base = base;
        n.t = video::Now();
        n.expected = r.extra.size();
        g_rec.named.push_back(n);
    }
}

void RecStopManual() {
    for (size_t i = 0; i < g_rec.recs.size(); ++i)
        if (g_rec.recs[i].manual) return RecStopAt(i);
}

void RecStopAll() {
    while (!g_rec.recs.empty()) RecStopAt(g_rec.recs.size() - 1);
}

void ToggleRecording() {
    if (ManualRecording()) RecStopManual();
    else RecStartManual();
}

void SetAutoMode(bool on) {
    if (g_rec.autoMode == on) return;
    g_rec.autoMode = on;
    RecStatus(on ? (g_rec.autoFilter == AF_ALL ? std::wstring(L"Auto: every approach is recorded")
                                               : L"Auto: approaches of " + std::wstring(kAutoFilterName[g_rec.autoFilter]))
                 : L"Manual: V starts and stops");
    Changed();
}

// Whose passes automatic mode records (the console's ACFT key). Passes under way keep their recording.
void SetAutoFilter(int f) {
    f = std::clamp(f, 0, AF_COUNT - 1);
    if (g_rec.autoFilter == f) return;
    g_rec.autoFilter = f;
    RecStatus(L"Auto records: " + std::wstring(kAutoFilterName[f]));
    Changed();
}

// Separate files (one per source with REC on) or one composite; from the next recording or still on.
void SetRecComposite(bool on) {
    if (g_rec.composite == on) return;
    g_rec.composite = on;
    if (on) CompEnsure();
    std::wstring t = on ? L"Composite: one file" : L"Separate: one file per source";
    if (Recording()) t += L", from the next recording";
    RecStatus(t);
    Changed();
}

// ---- black box
// The ring's footage between t0 and t1 to "<base>.mp4", one file per channel that has any.
void BlackBoxSave(double t0, double t1, const std::wstring& base) {
    if (!g_rec.ok) return;
    double s = 0;
    size_t bytes = 0, all = 0;
    g_rec.main.live.ringState(&s, &bytes);
    all += bytes;
    for (auto& c : g_rec.extra) {
        c->live.ringState(&s, &bytes);
        all += bytes;
    }
    StorageRequest(all);  // at most the whole rings
    g_rec.main.live.save(t0, t1, UniqueVideoPath(base));
    for (auto& c : g_rec.extra) {
        c->live.ringState(&s, &bytes);
        if (bytes) c->live.save(t0, t1, UniqueVideoPath(base + L" " + c->suffix));
    }
}

void BlackBoxSaveNow() {
    if (!g_bb.on) {
        RecStatus(L"The black box is off");
        return;
    }
    const double now = video::Now();
    BlackBoxSave(now - g_bb.minutes * 60, now, Stamp() + L" BLACK BOX");
    RecStatus(L"Saving the black box\x2026");
}

void SetBlackBox(bool on) {
    if (g_bb.on == on) return;
    g_bb.on = on;
    if (!on) g_bb.pending.clear();
    LiveApply();
    wchar_t b[96];
    swprintf_s(b, L"Black box on: keeps the last %.0f min", g_bb.minutes);
    RecStatus(on ? b : L"Black box off");
    Changed();
}

// ---- streams
void CameraReconcile();
// Stream n (0..3) on or off. Its settings are read as it starts (they may have been edited since).
void SetStreamTarget(int n, bool on) {
    if (n < 0 || n >= kStreams || !g_rec.ok) return;
    StreamTarget& t = g_streams[n];
    if (on == t.on) return;
    if (on) {
        t.url = Narrow(StreamIni(n, L"url"));
        t.key = Narrow(StreamIni(n, L"key"));
        if (t.url.empty()) {
            RecStatus(L"Stream " + std::to_wstring(n + 1) + L": no server in the settings", 10000);
            return;
        }
        t.source = StreamIni(n, L"source", L"a");
        if (t.source.empty()) t.source = L"a";
        t.tiles.clear();
        const std::wstring tl = StreamIni(n, L"tiles", L"a,b");
        for (size_t a = 0; a <= tl.size() && static_cast<int>(t.tiles.size()) < kMaxTiles;) {
            size_t e = tl.find(L',', a);
            if (e == std::wstring::npos) e = tl.size();
            if (e > a) t.tiles.push_back(tl.substr(a, e - a));
            a = e + 1;
        }
        if (t.composite() && t.tiles.empty()) t.tiles = {L"a", L"b"};
        live::Live::Config lc = g_rec.main.live.config();
        if (t.composite()) lc.w = kCompositeW, lc.h = kCompositeH, lc.bitrate = g_compositeBitrate;
        rtmp::Publisher::Config pc;
        pc.url = t.url;
        pc.key = t.key;
        pc.w = lc.w, pc.h = lc.h, pc.fps = lc.fps;
        pc.bitrate = lc.bitrate;
        pc.audio = g_streamAudio;
        t.rateT = 0, t.rate = 0;
        t.pub.start(pc);
        t.on = true;
        if (t.own()) {  // its own picture and encoder
            ChannelRelease(t.ch);
            t.ch.nextCapture = 0;
            t.ch.keep = g_streamSnap ? &t.snap : nullptr;
            t.ch.live.configure(lc);
            rtmp::Publisher* pub = &t.pub;
            t.ch.live.set(0, [pub](const live::UnitPtr& u) { pub->send(u); });
        }
        LiveApply();
        CameraReconcile();  // a camera it shows renders
    } else {
        t.on = false;
        LiveApply();  // first: nothing is handed to the publisher any more
        if (t.ch.live.wanted()) t.ch.live.set(0, nullptr);
        t.pub.stop();
        ChannelRelease(t.ch);
        CameraReconcile();
    }
}
// All the streams with a server on, or all off.
void SetStream(bool on) {
    if (!on) {
        for (int n = 0; n < kStreams; ++n) SetStreamTarget(n, false);
        return;
    }
    bool any = false;
    for (int n = 0; n < kStreams; ++n)
        if (StreamConfigured(n)) SetStreamTarget(n, true), any = true;
    if (!any && g_rec.ok) RecStatus(L"No stream server in the settings", 10000);
}

// Opens ILARTS.ini in Notepad, with the settings that have no control in the window written out.
void OpenSettingsFile() {
    auto ensure = [](const wchar_t* section, const wchar_t* key, const wchar_t* def) {
        wchar_t b[8];
        if (GetPrivateProfileStringW(section, key, L"\x01", b, 8, app.iniPath.c_str()) == 1 && b[0] == 1)
            WritePrivateProfileStringW(section, key, def, app.iniPath.c_str());
    };
    ensure(L"stream", L"url", L"");
    ensure(L"stream", L"key", L"");
    ensure(L"stream", L"bitrate_kbps", L"2500");
    ensure(L"blackbox", L"minutes", L"5");
    ensure(L"blackbox", L"mishap_before", L"60");
    ensure(L"blackbox", L"mishap_after", L"15");
    ensure(L"video", L"filename", L"{date} {time} {callsign} Case {case} {grade}");
    ensure(L"cctv", L"grain", L"1");
    ensure(L"cctv", L"scanlines", L"1");
    ensure(L"cctv", L"distortion", L"1");
    ensure(L"cctv", L"vignette", L"1");
    ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + app.iniPath + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

// ---- extra cameras (requests to DCS)
bool CameraIni(int i) { return GetPrivateProfileIntW(L"cameras", (L"camera" + std::to_wstring(i)).c_str(), 0, app.iniPath.c_str()) != 0; }
void SetCameraIni(int i, bool on) {
    WritePrivateProfileStringW(L"cameras", (L"camera" + std::to_wstring(i)).c_str(), on ? L"1" : L"0", app.iniPath.c_str());
}

// Deck positions wanted get a free render slot (asked to show them at once); the others give theirs back.
void AssignSlots() {
    for (int i = 0; i < CameraCount(); ++i)
        if (IsPosition(i) && !((g_cam.want >> i) & 1)) g_cam.slotPlus1[i] = 0;
    for (int i = 0; i < CameraCount(); ++i) {
        if (!IsPosition(i) || !((g_cam.want >> i) & 1) || g_cam.slotPlus1[i]) continue;
        for (int p : g_pool) {
            bool used = false;
            for (int j = 0; j < CameraCount(); ++j) used = used || (j != i && g_cam.slotPlus1[j] == p + 1);
            if (used) continue;
            g_cam.slotPlus1[i] = p + 1;
            CameraAskView(p, g_lcams[i].pos);
            break;
        }
    }
}
uint32_t PhysMask() {
    uint32_t m = 0;
    for (int i = 0; i < CameraCount(); ++i) {
        const int p = (g_cam.want >> i) & 1 ? PhysOf(i) : -1;
        if (p >= 0) m |= 1u << p;
    }
    return m;
}
void SendCameraMask() {
    if (!app.headerW) return;
    AssignSlots();
    auto& c = app.headerW->control;
    c.cameraMask = PhysMask();
    MemoryBarrier();
    g_cam.seqWritten = InterlockedIncrement(&c.cameraSeq);
}

void ConsoleDropSource(int s);  // console.inc: off any monitor that shows it
void SetRecArm(int s, bool on);  // console.inc
// drop: a camera switched off by the user leaves every monitor; one released because no monitor on screen shows
// it (a deck position) stays where it is, for when that monitor is on screen again.
void SetCamera(int i, bool on, bool drop) {
    if (i < 0 || i >= CameraCount() || !app.headerW) return;
    const bool was = ((g_cam.want >> i) & 1) != 0;
    if (on) g_cam.want |= 1u << i;
    else g_cam.want &= ~(1u << i);
    g_cam.confirmed[i] = false;
    g_cam.okSince[i] = 0;
    if (!IsPosition(i)) SetCameraIni(i, false);  // remembered again once it has delivered pictures for 10 s
    SendCameraMask();
    if (!on && drop) {  // a camera switched off leaves every monitor (so nothing asks for it again)
        if (app.selected == kFeeds + i) {
            if (g_con.progOff) app.selected = 0, UseGroup(0);  // monitor A stays off
            else SelectFeed(0);
        }
        ConsoleDropSource(kFeeds + i);
    }
    if (was != on && !IsPosition(i) && !BenchActive()) RecStatus(CameraName(i) + (on ? L" camera: asked DCS to render it" : L" camera off"));
}

bool ConsoleShows(int s);  // console.inc: monitor B or a preview on screen shows source s
bool SourceShown(int s) { return (s >= 0 && ProgramSource() == s) || ConsoleShows(s); }
// The deck position kept rendering for its REC switch while no monitor need show it: the last one shown, else the first.
int DeckRecSource() {
    const int n = CameraCount() - g_firstPos;
    if (n <= 0 || !RecArmed(PositionSource(0))) return -1;
    const int last = ConsoleSourceOf(g_con.lastDeck);
    return IsDeckSource(last) ? last : PositionSource(0);
}
// A camera renders while a monitor on screen shows it, a stream or output wants it, or it is recorded: the island
// camera while its REC switch is on, and with the DECK switch the last deck position shown. A deck position that
// is not needed gives its render slot back.
bool CycleWants(int s);  // console.inc: a monitor's deck cycle is bringing this position up next
void CycleTick(double now);
void TuneTick(double now);  // tuning.inc
bool CameraNeeded(int i) {
    if (BenchActive()) return BenchWantsCamera(i);  // a benchmark stage's views, nothing else
    const int s = kFeeds + i;
    return SourceShown(s) || (IsPosition(i) ? s == DeckRecSource() || CycleWants(s) : RecArmed(s)) || StreamWants(s) || OutputWants(s);
}
void CameraReconcile() {
    if (!g_cam.sent || !app.headerW) return;
    for (int i = 0; i < CameraCount(); ++i) {
        const bool need = CameraNeeded(i), on = ((g_cam.want >> i) & 1) != 0;
        if (need && !on) SetCamera(i, true);
        else if (!need && on) SetCamera(i, false, false);
    }
}

void CameraTick(double now) {
    if (!HeaderValid()) return;
    if (app.headerW) {  // the picture type per render slot (the DLL reads it every frame): FLIR (4) or as configured (0)
        int type[lsoshare::kMaxCameras] = {};
        if (g_expFlir)
            for (int i = 0; i < CameraCount() && i < 16; ++i) {
                const int p = PhysOf(i);
                if (g_flirOn[i] && p >= 0 && p < lsoshare::kMaxCameras) type[p] = 4;
            }
        for (int i = 0; i < lsoshare::kMaxCameras; ++i) app.headerW->ptz[i].pictureType = type[i];
    }
    if (CamLive()) {  // DCS's camera list (physical), remembered for when it is not running
        std::wstring t;
        for (int p = 0; p < PhysCount(); ++p) {
            if (p) t += L';';
            for (char c : PhysName(p)) t += static_cast<wchar_t>(static_cast<unsigned char>(c));
            for (int k = 0; k < PhysViewCount(p); ++k) {
                t += k ? L'|' : L'=';
                for (char c : PhysViewName(p, k)) t += static_cast<wchar_t>(static_cast<unsigned char>(c));
                if (PhysViewHangar(p, k)) t += L"\tH";
            }
        }
        if (t != g_knownText) {
            WritePrivateProfileStringW(L"cameras", L"known", t.c_str(), app.iniPath.c_str());
            ParseKnownCams(t);
        }
    }
    const DWORD pid = app.header->producerPid;
    if (pid != g_cam.pid) {  // another DCS session: nothing asked of it yet
        g_cam.pid = pid;
        g_cam.sent = false;
        g_cam.want = 0;
        for (auto& x : g_cam.slotPlus1) x = 0;
    }
    // A new mission in the same DCS: sharing starts again and clears the shared block - the cameras asked for
    // with it. Ask again (the DCS process is the same, so the session check above misses it).
    if (g_cam.sent && app.headerW && app.header->control.cameraSeq != g_cam.seqWritten) {
        if (g_cam.want) {
            SendCameraMask();
            RecStatus(L"DCS restarted the mission: cameras asked for again");
        } else {
            g_cam.seqWritten = app.header->control.cameraSeq;
        }
    }
    if (!g_cam.sent && CamLive() && app.headerW) {
        g_cam.sent = true;
        for (int i = 0; i < CameraCount(); ++i) {  // on a monitor, or (a fixed camera) recorded
            if (!CameraNeeded(i) && !((g_cam.testMask >> i) & 1)) continue;
            g_cam.want |= 1u << i;
            g_cam.confirmed[i] = false;
            g_cam.okSince[i] = 0;
            if (!IsPosition(i)) SetCameraIni(i, false);
        }
        if (g_cam.want) SendCameraMask();
    }
    CameraReconcile();
    if (app.headerW) {  // each render slot: what its camera follows (1 = nothing: AUTO off)
        int mode[lsoshare::kMaxCameras];
        for (auto& m : mode) m = 1;
        for (int i = 0; i < CameraCount(); ++i) {
            const int p = ((g_cam.want >> i) & 1) ? PhysOf(i) : -1;
            if (p >= 0 && p < lsoshare::kMaxCameras) mode[p] = TrackOf(i).armed && !IsHangar(i) ? TrackOf(i).target : 1;
        }
        for (int p = 0; p < lsoshare::kMaxCameras; ++p) app.headerW->ptz[p].trackMode = mode[p];
    }
    // each deck slot asked for its position until DCS shows it
    for (int i = 0; CamLive() && i < CameraCount(); ++i) {
        const int p = IsPosition(i) && ((g_cam.want >> i) & 1) ? PhysOf(i) : -1;
        if (p >= 0 && PhysCurrentView(p) != g_lcams[i].pos && GetTickCount64() - g_cam.viewAskedAt[p] > 500) CameraAskView(p, g_lcams[i].pos);
    }
    for (int i = 0; i < CameraCount(); ++i) {  // a fixed camera that delivers pictures is remembered on
        if (IsPosition(i) || !((g_cam.want >> i) & 1) || g_cam.confirmed[i]) continue;
        if (!ViewFor(kFeeds + i).main) {
            g_cam.okSince[i] = 0;
            continue;
        }
        if (g_cam.okSince[i] == 0) g_cam.okSince[i] = now;
        if (now - g_cam.okSince[i] > 10) {
            g_cam.confirmed[i] = true;
            SetCameraIni(i, true);
        }
    }
}

void FeedsReplayStart(double t0, double t1);  // console.inc: monitor B and the previews replay too
void FeedsReplayStop();
void FeedsReplaySave(const std::wstring& base);
void FeedsTick(double now);
void FeedsEach(const std::function<void(Channel&)>& fn);

// The pass an instant replay shows: the most recently ended one still in the buffer, else the latest under way.
const RecState::PassInfo* LastPass() {
    const double now = video::Now(), keep = g_rec.ok ? g_rec.main.eng.config().ringSeconds : 0;
    const RecState::PassInfo* best = nullptr;
    for (const auto& p : g_rec.passes)
        if (!p.active && p.result != 5 && now - p.end < keep && (!best || p.end > best->end)) best = &p;
    if (best) return best;
    for (const auto& p : g_rec.passes)
        if (p.active && (!best || p.start > best->start)) best = &p;
    return best;
}

// The last pass (from just before it came into view to a few seconds after it ended) if it is still in the
// buffer, else the last replay_default seconds.
void ReplayStart() {
    if (!g_rec.ok) return;
    double oldest = 0, newest = 0;
    if (!g_rec.main.eng.bounds(&oldest, &newest)) {
        RecStatus(L"Nothing to replay yet");
        return;
    }
    const double now = video::Now();
    double t0 = newest - g_rec.replayDefault, t1 = newest;
    std::wstring label = L"last " + std::to_wstring(static_cast<int>(g_rec.replayDefault)) + L" s";
    if (const RecState::PassInfo* p = LastPass()) {
        t0 = p->start - g_rec.preroll;
        t1 = p->active ? newest : p->end + g_rec.postroll;
        label = PassLabel(*p) + L" " + (p->active ? std::wstring(L"in progress") : std::wstring(ResultName(p->result)));
    }
    g_rec.frames = g_rec.main.eng.range(t0, t1);
    if (g_rec.frames.size() < 2) {
        g_rec.frames.clear();
        RecStatus(L"Nothing to replay yet");
        return;
    }
    g_rec.replay = true;
    g_rec.paused = false;
    g_rec.pos = 0;
    g_rec.speed = 1;
    g_rec.lastTick = now;
    g_rec.shown = -1;
    g_rec.replayLabel = label;
    FeedsReplayStart(g_rec.frames.front()->t, g_rec.frames.back()->t);
}

void ReplayStop() {
    g_rec.replay = false;
    g_rec.frames.clear();
    g_rec.frames.shrink_to_fit();
    FeedsReplayStop();
}

void ToggleReplay() {
    if (g_rec.replay) ReplayStop();
    else ReplayStart();
}

void ReplaySave() {
    if (!g_rec.replay || g_rec.frames.empty()) return;
    const std::wstring base = L"Replay " + Stamp() + L" " + g_rec.replayLabel;
    const double secs = g_rec.frames.back()->t - g_rec.frames.front()->t;
    StorageRequest(static_cast<unsigned long long>(RecBytesPerSecond() * secs * 2));  // monitor A's and as much again for the others
    g_rec.main.eng.saveClip(g_rec.frames, UniqueVideoPath(base));
    FeedsReplaySave(base);
    RecStatus(L"Saving the replay\x2026");
}

// Keys while a replay is shown; false = not a replay key (handled as usual).
bool ReplayKey(WPARAM w) {
    if (g_rec.frames.empty()) return false;
    const double dur = g_rec.frames.back()->t - g_rec.frames.front()->t;
    const double frame = 1.0 / g_rec.main.eng.config().fps;
    switch (w) {
        case VK_SPACE:
            if (g_rec.paused) {
                if (g_rec.pos >= dur - 1e-6) g_rec.pos = 0;
                g_rec.paused = false;
                g_rec.lastTick = video::Now();
            } else {
                g_rec.paused = true;
            }
            return true;
        case VK_ESCAPE:
        case VK_BACK: ReplayStop(); return true;
        case VK_LEFT: g_rec.pos = std::max(0.0, g_rec.pos - (g_rec.paused ? frame : 2.0)); return true;
        case VK_RIGHT: g_rec.pos = std::min(dur, g_rec.pos + (g_rec.paused ? frame : 2.0)); return true;
        case VK_OEM_COMMA:
            g_rec.paused = true;
            g_rec.pos = std::max(0.0, g_rec.pos - frame);
            return true;
        case VK_OEM_PERIOD:
            g_rec.paused = true;
            g_rec.pos = std::min(dur, g_rec.pos + frame);
            return true;
        case VK_UP: g_rec.speed = std::min(8.0, g_rec.speed * 2); return true;
        case VK_DOWN: g_rec.speed = std::max(0.125, g_rec.speed / 2); return true;
        case VK_HOME: g_rec.pos = 0; return true;
        case 'S': ReplaySave(); return true;
        default: return false;
    }
}

// Every loop: the engines' messages, the hook's passes (automatic recording, one file per pass and camera),
// automatic stops, late grades, mishaps and the black box, the cameras.
void RememberCompat();  // repair.inc
void RecTick() {
    if (!g_rec.ok) return;
    const double now = video::Now();
    auto drain = [&](Channel& ch) {
        video::Event e;
        while (ch.eng.poll(&e)) {
            if (e.key == kBenchRecKey) {  // a benchmark recording (null sink): nothing was saved
                BenchEncoded(e);
                continue;
            }
            const std::wstring name = e.path.substr(e.path.find_last_of(L"\\/") + 1);
            if (e.kind == video::Event::Saved) {
                ++g_savedCount;
                g_storage.requested = true;
                RecStatus(L"Saved " + name, 10000);
                for (auto& n : g_rec.named)
                    if (e.key > 0 && n.pass == static_cast<uint32_t>(e.key)) n.paths.push_back(e.path);
            } else if (e.kind == video::Event::Discarded) {
                ++g_discardCount;
                if (GetTickCount64() >= g_rec.statusUntil) RecStatus(L"Nothing recorded (no picture)");
            } else {
                RecStatus(L"Video failed: " + e.text, 10000);
            }
        }
        live::Event le;
        while (ch.live.poll(&le)) {
            const std::wstring name = le.path.substr(le.path.find_last_of(L"\\/") + 1);
            if (le.kind == live::Event::Saved) {
                ++g_bbSaved;
                g_storage.requested = true;
                RecStatus(L"Saved " + name, 10000);
            } else if (le.kind == live::Event::Failed) {
                RecStatus(L"Black box: " + le.text, 10000);
            } else {
                RecStatus(le.text, 10000);
            }
        }
    };
    drain(g_rec.main);
    for (auto& c : g_rec.extra) drain(*c);
    drain(g_rec.comp);
    FeedsEach(drain);  // saved replays of monitor B and the previews
    EnsureChannels();
    FeedsTick(now);
    VCamTick();
    CameraComponentPoll();
    RememberCompat();
    CameraTick(now);
    CycleTick(now);  // console.inc: the monitors' deck cycles
    TuneTick(now);   // tuning.inc: the camera tuning panel's file
    CarrierTick();
    SessionTick();  // before the passes: a new mission's first pass goes into its folder
    StorageTick();
    if (HeaderValid()) {
        const DWORD pid = app.header->producerPid;
        const uint32_t epoch = app.header->passEpoch;
        if (!g_rec.passInit || pid != g_rec.passPid || epoch != g_rec.passEpoch) {
            // a new DCS / mission: passes already under way at this point are not recorded
            g_rec.passInit = true;
            g_rec.passPid = pid;
            g_rec.passEpoch = epoch;
            g_rec.passes.clear();
            g_rec.passesPrimed = false;
        }
        const uint32_t n = std::min<uint32_t>(app.header->passCount, 4);
        auto str = [](const char* a, size_t len) { return std::string(a, strnlen(a, len)); };
        for (uint32_t i = 0; i < n; ++i) {
            lsoshare::Header::Pass p;
            memcpy(&p, const_cast<const lsoshare::Header::Pass*>(&app.header->passes[i]), sizeof(p));
            if (p.id == 0) continue;
            RecState::PassInfo* pi = FindPass(p.id);
            if (!pi) {
                RecState::PassInfo add;
                add.id = p.id;
                add.active = p.active != 0;
                add.start = now;
                add.end = p.active ? 0 : now;
                add.result = p.result;
                add.type = str(p.type, sizeof(p.type));
                add.board = str(p.board, sizeof(p.board));
                add.pilot = str(p.pilot, sizeof(p.pilot));
                add.carrier = str(p.carrier, sizeof(p.carrier));
                add.flags = app.header->passFlags[i];
                g_rec.passes.push_back(add);
                pi = &g_rec.passes.back();
                if (add.active && g_rec.passesPrimed && g_rec.autoMode) {
                    const bool want = AutoWantsPass(add.flags);
                    TestLog(L"auto pass " + std::to_wstring(add.id) + L" flags " + std::to_wstring(add.flags) + L" own " +
                            std::to_wstring(OwnAircraft()) + L" filter " + kAutoFilterKey[g_rec.autoFilter] + (want ? L": record" : L": skip"));
                    if (want) RecStartPass(*pi);
                }
            } else if (pi->active && !p.active && p.result != 0) {
                pi->active = false;
                pi->end = now;
                pi->result = p.result;
                for (auto& r : g_rec.recs)
                    if (!r.manual && r.pass == pi->id) r.stopAt = now + (p.result == 5 ? 0.0 : g_rec.postroll);
            }
            // callsign, case and the LSO's grade (the grade comes a few seconds after the pass ended)
            pi->caseNo = p.caseNo;
            pi->flags = app.header->passFlags[i];
            pi->callsign = str(p.callsign, sizeof(p.callsign));
            pi->grade = str(p.grade, sizeof(p.grade));
            pi->wire = str(p.wire, sizeof(p.wire));
        }
        g_rec.passesPrimed = true;
        // forget passes that left the replay buffer and have no recording (or file that may still be renamed)
        const double keep = g_rec.main.eng.config().ringSeconds + 10;
        g_rec.passes.erase(std::remove_if(g_rec.passes.begin(), g_rec.passes.end(),
                                          [&](const RecState::PassInfo& p) {
                                              if (p.active || now - p.end < keep) return false;
                                              for (const auto& r : g_rec.recs)
                                                  if (r.pass == p.id) return false;
                                              for (const auto& nm : g_rec.named)
                                                  if (nm.pass == p.id) return false;
                                              return true;
                                          }),
                           g_rec.passes.end());
        // a mishap near the carrier: the black box keeps what led up to it and a little after
        // Every mission starts the header from zero (same DCS process), so a new header resyncs to the count it
        // has; a count lower than the last one is a new header the viewer did not see go away.
        const LONG seq = app.header->mishap.seq;
        if (!g_bb.init || g_bb.pid != pid || seq < g_bb.seq) {
            g_bb.init = true;
            g_bb.pid = pid;
            g_bb.seq = seq;
        } else if (seq != g_bb.seq) {
            g_bb.seq = seq;
            ++g_mishaps;
            const std::wstring text = Utf8W(str(app.header->mishap.text, sizeof(app.header->mishap.text)));
            if (g_bb.on) {
                g_bb.pending.push_back({now + g_bb.after, now - g_bb.before, now + g_bb.after, Stamp() + L" MISHAP " + text});
                RecStatus(L"Mishap (" + text + L"): the black box will be saved", 10000);
            } else {
                RecStatus(L"Mishap (" + text + L"); the black box is off", 10000);
            }
        }
    } else {
        g_bb.init = false;  // between missions: the next header's mishap count starts again from 0
        if (Recording()) RecStopAll();  // DCS has gone
    }
    for (size_t i = 0; i < g_bb.pending.size();) {
        const auto pd = g_bb.pending[i];
        if (now >= pd.at || !HeaderValid()) {
            BlackBoxSave(pd.t0, std::min(pd.t1, now), pd.base);
            g_bb.pending.erase(g_bb.pending.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (size_t i = g_rec.recs.size(); i-- > 0;) {
        const auto& r = g_rec.recs[i];
        if (!r.manual && ((r.stopAt > 0 && now >= r.stopAt) || now - r.start > g_rec.autoMaxS)) RecStopAt(i);
    }
    // a grade that arrived after the file was named: rename the pass's files
    for (size_t i = 0; i < g_rec.named.size();) {
        auto& nm = g_rec.named[i];
        const RecState::PassInfo* p = FindPass(nm.pass);
        if (p && nm.paths.size() >= nm.expected) {
            const std::wstring want = PassFileName(nm.stamp, p, true);
            if (want != nm.base) {
                const std::wstring oldSafe = SafeName(nm.base), newSafe = SafeName(want);
                for (auto& path : nm.paths) {
                    const size_t slash = path.find_last_of(L"\\/");
                    const std::wstring file = path.substr(slash + 1);
                    if (file.rfind(oldSafe, 0) != 0) continue;
                    const std::wstring target = path.substr(0, slash + 1) + newSafe + file.substr(oldSafe.size());
                    StorageNote(target);
                    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES && MoveFileW(path.c_str(), target.c_str())) {
                        path = target;
                        ++g_renamed;
                    }
                }
                nm.base = want;
                RecStatus(L"Graded: " + want, 10000);
            }
        }
        if (now - nm.t > 120) g_rec.named.erase(g_rec.named.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

// Hidden self-test (--test-record S [--test-replay] | --test-auto, --test-log file): drives recording and
// replay against a live feed and writes what happened to the log file. Returns true to quit.
struct TestDrive {
    double recordS = 0;
    double runS = 0;                               // --test-run S: run that long, log a summary, quit
    std::vector<std::pair<double, wchar_t>> keys;  // --test-keys "3:4,5.5:c,8:!": keys at times (! = window capture)
    size_t keyAt = 0;
    bool replay = false, autoMode = false, stream = false;
    std::wstring log;
    int phase = 0;
    double t = 0, start = 0;
    bool shotRec = false;
    size_t maxRecs = 0;
    // console: --test-script lines (seconds, javascript or !shot), --test-offscreen, --test-commands
    std::vector<std::pair<double, std::wstring>> script;
    size_t scriptAt = 0;
    bool offscreen = false, logCommands = false;
    bool noWebView2 = false;  // --test-no-webview2: behave as if the WebView2 runtime were not installed
} g_test;
void ConsoleTestTick(double t);

void TestLog(const std::wstring& line) {
    FILE* f = nullptr;
    if (g_test.log.empty() || _wfopen_s(&f, g_test.log.c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"%.2f %s\n", video::Now() - g_test.start, line.c_str());
    fclose(f);
}

void HandleKey(WPARAM w);
void ConsoleProgramOff();  // console.inc: monitor A shows no source
int ConsoleDeckFor(int cur);  // console.inc: the deck position the DECK key picks
void ConsoleNoteDeck(int s);  // console.inc

void TestSummary() {
    TestLog(L"summary: saved=" + std::to_wstring(g_savedCount) + L" discarded=" + std::to_wstring(g_discardCount) + L" blackbox_saved=" +
            std::to_wstring(g_bbSaved) + L" mishaps=" + std::to_wstring(g_mishaps) + L" renamed=" + std::to_wstring(g_renamed) +
            L" stills_saved=" + std::to_wstring(g_stillsSaved) + L" stills_failed=" + std::to_wstring(g_stillsFailed));
    for (const auto& p : g_stillPaths) TestLog(L"still " + p + (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES ? L" (on disk)" : L" (MISSING)"));
    TestLog(L"session folder: " + SessionFolder(false) + L"; storage limit " + (g_storage.on ? std::to_wstring(g_storage.gb) + L" GB" : L"off") + L": runs " +
            std::to_wstring(g_storage.worker.runs) + L", deleted " + std::to_wstring(g_storage.worker.deleted) + L" (" +
            std::to_wstring(g_storage.worker.freed / 1000) + L" kB), folders removed " + std::to_wstring(g_storage.worker.dirsRemoved) + L", over " +
            std::to_wstring(g_storage.worker.over ? 1 : 0));
    double rs = 0;
    size_t rb = 0;
    g_rec.main.live.ringState(&rs, &rb);
    wchar_t b[256];
    swprintf_s(b, L"feed %d of %d, cctv %d, black box %d (%.1f s, %.2f MB, encoder %s ok=%d)", app.selected + 1, FeedCount(), g_cctv.on ? 1 : 0,
               g_bb.on ? 1 : 0, rs, rb / 1e6, g_rec.main.live.hardware() ? L"hardware" : L"software", g_rec.main.live.encoderOk() ? 1 : 0);
    TestLog(b);
    swprintf_s(b, L"cameras: offered %d, asked mask %u, rendering %d%d", CameraCount(), g_cam.want, CameraRendering(0) ? 1 : 0, CameraRendering(1) ? 1 : 0);
    TestLog(b);
    for (const auto& c : g_rec.extra) {
        c->live.ringState(&rs, &rb);
        swprintf_s(b, L"channel %s: source %d, last picture %.2f s ago, black box %.1f s", c->suffix.c_str(), c->source, video::Now() - c->lastFrame, rs);
        TestLog(b);
    }
    if (g_rec.comp.ok) {
        std::wstring tiles;
        for (int s : g_rec.compTiles) tiles += (tiles.empty() ? L"" : L",") + SourceTag(s);
        swprintf_s(b, L"composite %d: %dx%d, %.0f kbit/s, last picture %.2f s ago, tiles ", g_rec.composite ? 1 : 0, g_rec.comp.eng.config().w,
                   g_rec.comp.eng.config().h, g_rec.comp.eng.config().bitrate / 1000.0, video::Now() - g_rec.comp.lastFrame);
        TestLog(b + tiles);
    }
    for (int n = 0; n < kStreams; ++n) {
        const auto& t = g_streams[n];
        if (!t.on && n) continue;
        swprintf_s(b, L"stream %d: on %d source %s state %d sent %.0f kB dropped %ld size %dx%d text ", n + 1, t.on ? 1 : 0, t.source.c_str(),
                   static_cast<int>(t.pub.state()), t.pub.bytesSent() / 1000.0, t.pub.dropped(), t.own() ? t.ch.live.config().w : 0,
                   t.own() ? t.ch.live.config().h : 0);
        TestLog(b + Utf8W(t.pub.text()));
        if (t.snap.empty() || !t.own()) continue;
        const int w = t.ch.live.config().w, h = t.ch.live.config().h;  // its last picture, as a top-down 32-bit BMP
        if (t.snap.size() != static_cast<size_t>(w) * h * 4) continue;
        BITMAPINFOHEADER bi{sizeof(bi), w, -h, 1, 32, BI_RGB};
        BITMAPFILEHEADER fh{0x4D42, static_cast<DWORD>(sizeof(fh) + sizeof(bi) + t.snap.size()), 0, 0, sizeof(fh) + sizeof(bi)};
        const std::wstring path = g_test.log.substr(0, g_test.log.find_last_of(L"\\/") + 1) + L"stream" + std::to_wstring(n + 1) + L".bmp";
        FILE* bf = nullptr;
        if (_wfopen_s(&bf, path.c_str(), L"wb") == 0 && bf) {
            fwrite(&fh, sizeof(fh), 1, bf);
            fwrite(&bi, sizeof(bi), 1, bf);
            fwrite(t.snap.data(), 1, t.snap.size(), bf);
            fclose(bf);
        }
    }
    TestLog(L"status: " + g_rec.status);
}

bool TestTick() {
    if (g_test.recordS <= 0 && !g_test.autoMode && g_test.runS <= 0) return false;
    const double now = video::Now();
    if (g_test.phase == 0) {
        g_test.start = now;
        g_test.phase = 1;
        TestLog(L"start, video engine " + std::wstring(g_rec.ok ? L"ok" : L"FAILED"));
    }
    if (now - g_test.start > std::max(90.0, g_test.runS + 30)) {
        TestLog(L"timeout");
        return true;
    }
    if (g_test.runS > 0 && now - g_test.start >= g_test.runS) {
        TestSummary();
        return true;
    }
    ConsoleTestTick(now - g_test.start);
    if (app.presentedWithImage == 0) return false;
    while (g_test.keyAt < g_test.keys.size() && now - g_test.start >= g_test.keys[g_test.keyAt].first) {
        const wchar_t k = g_test.keys[g_test.keyAt++].second;
        TestLog(std::wstring(L"key ") + k + L" (status before: " + g_rec.status + L")");
        if (k == L'!') {  // the pictures and the window as they are now
            const std::wstring base = g_test.log + L"." + std::to_wstring(static_cast<int>(now - g_test.start));
            g_shotPath = base + L".bmp";
            SaveWindowCapture(base + L".window.bmp");
        } else {
            HandleKey(static_cast<WPARAM>(towupper(k)));
        }
    }
    static int lastSaved = 0, lastDiscard = 0;
    if (g_savedCount != lastSaved || g_discardCount != lastDiscard) {
        TestLog(L"saved=" + std::to_wstring(g_savedCount) + L" discarded=" + std::to_wstring(g_discardCount) + L" status: " + g_rec.status);
        lastSaved = g_savedCount;
        lastDiscard = g_discardCount;
    }
    if (g_test.recordS <= 0 && !g_test.autoMode) return false;  // a timed run only
    switch (g_test.phase) {
        case 1:
            if (g_test.autoMode) {
                g_test.phase = 10;
                TestLog(L"auto mode on, waiting for passes");
            } else {
                RecStartManual();
                g_test.t = now;
                g_test.phase = 2;
                TestLog(L"recording " + (g_rec.recs.empty() ? std::wstring(L"?") : g_rec.recs.back().path));
            }
            break;
        case 2:
            if (!g_test.log.empty() && now - g_test.t >= g_test.recordS / 2 && !g_test.shotRec) {
                g_test.shotRec = true;
                SaveWindowCapture(g_test.log + L".rec.bmp");
            }
            if (now - g_test.t >= g_test.recordS) {
                RecStopManual();
                g_test.phase = 3;
                TestLog(L"stopped");
            }
            break;
        case 3:
            if (g_savedCount >= 1) {
                if (!g_test.replay) return true;
                ReplayStart();
                TestLog(L"replay " + std::to_wstring(g_rec.frames.size()) + L" frames: " + g_rec.replayLabel);
                g_test.t = now;
                g_test.phase = 4;
            }
            break;
        case 4:
            if (now - g_test.t > 1.5) {
                if (!g_test.log.empty()) SaveWindowCapture(g_test.log + L".replay.bmp");
                TestLog(L"replay position " + std::to_wstring(g_rec.pos) + L" s, shown frame " + std::to_wstring(g_rec.shown));
                ReplaySave();
                g_test.phase = 5;
            }
            break;
        case 5:
            if (g_savedCount >= 2) return true;
            break;
        case 10:
            if (g_rec.recs.size() > g_test.maxRecs) {
                g_test.maxRecs = g_rec.recs.size();
                std::wstring which;
                for (const auto& r : g_rec.recs) which += L" pass " + std::to_wstring(r.pass);
                TestLog(std::to_wstring(g_rec.recs.size()) + L" recording(s) open:" + which);
                if (!g_test.log.empty() && g_rec.recs.size() >= 2) SaveWindowCapture(g_test.log + L".rec.bmp");
            }
            if (g_test.runS <= 0 && g_autoStarts > 0 && g_rec.recs.empty() && g_savedCount + g_discardCount >= g_autoStarts) {
                for (const auto& p : g_rec.passes)
                    TestLog(L"pass " + std::to_wstring(p.id) + L" " + PassLabel(p) + L" " + ResultName(p.result));
                return true;
            }
            break;
    }
    return false;
}

// One keyboard step of a picture adjustment (the program's levels).
enum Adjust { ADJ_EXPOSURE, ADJ_BRIGHT, ADJ_CONTRAST, ADJ_GAMMA };
void StepAdjust(Adjust a, int dir) {
    switch (a) {
        case ADJ_EXPOSURE: app.set.stops = std::clamp(app.set.stops + dir / 3.0f, -4.0f, 8.0f); break;
        case ADJ_BRIGHT: app.set.brightness = std::clamp(app.set.brightness + dir * 0.02f, -0.5f, 0.5f); break;
        case ADJ_CONTRAST: app.set.contrast = std::clamp(app.set.contrast * (dir > 0 ? 1.1f : 1 / 1.1f), 0.25f, 16.0f); break;
        case ADJ_GAMMA: app.set.gamma = std::clamp(app.set.gamma + dir * 0.1f, 0.3f, 4.0f); break;
    }
    Changed();
}

// Ctrl / Shift held: the thread's key state, or the keyboard's (keys that went to the console's WebView).
bool KeyHeld(int vk) { return ((GetKeyState(vk) | GetAsyncKeyState(vk)) & 0x8000) != 0; }

// ---- keyboard shortcuts: [keys] <action> = virtual key | 0x100 Shift | 0x200 Ctrl | 0x400 Alt (0 = none). The
// settings dialog rebinds them. The "in DCS" ones are system-wide hotkeys (they work while DCS has the keyboard).
// Fixed: the source keys 1-9 (monitor A), Ctrl + a preset's key (save to it), the replay's own keys.
constexpr int kKShift = 0x100, kKCtrl = 0x200, kKAlt = 0x400;
struct KeyAction {
    const wchar_t* id;
    const wchar_t* label;
    int def;
    bool global;
};
enum KeyId {
    K_RECORD, K_VTRMODE, K_REPLAY, K_BBSAVE, K_STILL, K_CCTV, K_AUTOLEV, K_RESETLEV, K_EXPUP, K_EXPDOWN, K_BRIUP, K_BRIDOWN, K_CONUP,
    K_CONDOWN, K_GAMUP, K_GAMDOWN, K_SHIPNEXT, K_SHIPPREV, K_PRESET1, K_PRESET2, K_PRESET3, K_PRESET4, K_CLOSE,
    K_GRECORD, K_GREPLAY, K_GBBSAVE, K_GSTILL, K_COUNT
};
const KeyAction kKeyActions[K_COUNT] = {
    {L"record", L"Record", 'V', false},
    {L"vtr_mode", L"VTR auto / manual", 'M', false},
    {L"replay", L"Instant replay", VK_SPACE, false},
    {L"bb_save", L"Save the black box", 'B', false},
    {L"still", L"Still", 'P', false},
    {L"cctv", L"CCTV look", 'C', false},
    {L"auto_levels", L"Auto levels", 'A', false},
    {L"reset_levels", L"Reset levels", 'R', false},
    {L"exposure_up", L"Exposure +", VK_OEM_PLUS, false},
    {L"exposure_down", L"Exposure \x2212", VK_OEM_MINUS, false},
    {L"brightness_up", L"Brightness +", VK_UP, false},
    {L"brightness_down", L"Brightness \x2212", VK_DOWN, false},
    {L"contrast_up", L"Contrast +", VK_RIGHT, false},
    {L"contrast_down", L"Contrast \x2212", VK_LEFT, false},
    {L"gamma_up", L"Gamma +", VK_PRIOR, false},
    {L"gamma_down", L"Gamma \x2212", VK_NEXT, false},
    {L"ship_next", L"Next ship", 'N', false},
    {L"ship_prev", L"Previous ship", 'N' | kKShift, false},
    {L"preset_1", L"Preset Night", VK_F5, false},
    {L"preset_2", L"Preset 1", VK_F6, false},
    {L"preset_3", L"Preset 2", VK_F7, false},
    {L"preset_4", L"Preset 3", VK_F8, false},
    {L"close", L"Close ILARTS", VK_ESCAPE, false},
    {L"dcs_record", L"Record (in DCS)", VK_F9 | kKCtrl | kKShift, true},
    {L"dcs_replay", L"Instant replay (in DCS)", VK_F10 | kKCtrl | kKShift, true},
    {L"dcs_bb_save", L"Save the black box (in DCS)", VK_F11 | kKCtrl | kKShift, true},
    {L"dcs_still", L"Still (in DCS)", VK_F8 | kKCtrl | kKShift, true},
};
int g_keyCode[K_COUNT];
void LoadKeys() {
    for (int i = 0; i < K_COUNT; ++i) g_keyCode[i] = GetPrivateProfileIntW(L"keys", kKeyActions[i].id, kKeyActions[i].def, app.iniPath.c_str());
}
// Keys whose auto-repeat is ignored: the carrier step, the still (one per press).
bool KeyNoRepeat(int vk) { return vk == 'N' || vk == (g_keyCode[K_STILL] & 0xFF); }
// The "in DCS" actions as system-wide hotkeys (ids 1 .. 4: WM_HOTKEY in the message loop), registered again after a change.
void RegisterGlobalKeys() {
    const int ids[4] = {K_GRECORD, K_GREPLAY, K_GBBSAVE, K_GSTILL};
    for (int h = 0; h < 4; ++h) {
        UnregisterHotKey(nullptr, h + 1);
        const int c = g_keyCode[ids[h]];
        if (!(c & 0xFF)) continue;
        const UINT mods = ((c & kKShift) ? MOD_SHIFT : 0) | ((c & kKCtrl) ? MOD_CONTROL : 0) | ((c & kKAlt) ? MOD_ALT : 0) | MOD_NOREPEAT;
        RegisterHotKey(nullptr, h + 1, mods, c & 0xFF);
    }
}

void HandleKey(WPARAM w) {
    if (g_rec.replay && ReplayKey(w)) return;
    const int mods = (KeyHeld(VK_SHIFT) ? kKShift : 0) | (KeyHeld(VK_CONTROL) ? kKCtrl : 0) | (KeyHeld(VK_MENU) ? kKAlt : 0);
    const int code = static_cast<int>(w & 0xFF) | mods;
    int act = -1;
    for (int i = 0; i < K_COUNT && act < 0; ++i)
        if (!kKeyActions[i].global && g_keyCode[i] == code) act = i;
    if (act < 0 && (mods & kKCtrl))  // Ctrl + a preset's key: save the look to it
        for (int i = K_PRESET1; i <= K_PRESET4; ++i)
            if (g_keyCode[i] == (code & ~kKCtrl)) {
                StorePreset(i - K_PRESET1);
                return;
            }
    switch (act) {
        case K_RECORD: ToggleRecording(); return;
        case K_VTRMODE: SetAutoMode(!g_rec.autoMode); return;
        case K_REPLAY: ToggleReplay(); return;
        case K_BBSAVE: BlackBoxSaveNow(); return;
        case K_STILL: StillTake(); return;
        case K_CCTV: g_cctv.on = !g_cctv.on; Changed(); return;
        case K_AUTOLEV: app.set.autoLevels = !app.set.autoLevels; ResetAuto(); Changed(); return;
        case K_RESETLEV: ResetAdjustments(); return;
        case K_EXPUP: StepAdjust(ADJ_EXPOSURE, +1); return;
        case K_EXPDOWN: StepAdjust(ADJ_EXPOSURE, -1); return;
        case K_BRIUP: StepAdjust(ADJ_BRIGHT, +1); return;
        case K_BRIDOWN: StepAdjust(ADJ_BRIGHT, -1); return;
        case K_CONUP: StepAdjust(ADJ_CONTRAST, +1); return;
        case K_CONDOWN: StepAdjust(ADJ_CONTRAST, -1); return;
        case K_GAMUP: StepAdjust(ADJ_GAMMA, +1); return;
        case K_GAMDOWN: StepAdjust(ADJ_GAMMA, -1); return;
        case K_SHIPNEXT: StepCarrier(+1); return;
        case K_SHIPPREV: StepCarrier(-1); return;
        case K_PRESET1: case K_PRESET2: case K_PRESET3: case K_PRESET4: LoadPreset(act - K_PRESET1); return;
        case K_CLOSE: DestroyWindow(app.hwnd); return;
        default: break;
    }
    // fixed: the numeric keypad's + / -, the source keys
    if (w == VK_ADD) return StepAdjust(ADJ_EXPOSURE, +1);
    if (w == VK_SUBTRACT) return StepAdjust(ADJ_EXPOSURE, -1);
    // The console's source keys in order: PLAT, LSO 1, LSO 2, ISLAND (a key each), then DECK, one key for all the
    // deck positions that picks one as the console's DECK key does (ConsoleDeckFor).
    const int fixedKeys = kFeeds + g_firstPos, sourceKeys = fixedKeys + (CameraCount() > g_firstPos ? 1 : 0);
    if (!mods && w >= '1' && w < static_cast<WPARAM>('1' + std::min(9, sourceKeys))) {
        const int k = static_cast<int>(w - '1');
        const int s = k < fixedKeys ? k : ConsoleDeckFor(ProgramSource());
        if (s < 0) return;
        if (s == ProgramSource()) return ConsoleProgramOff();  // the lit key again: monitor A off
        if (k < fixedKeys) return SelectFeed(s);
        if (g_rec.replay) ReplayStop();  // as the console's DECK key
        SelectFeed(s);
        ConsoleNoteDeck(s);
        CameraReconcile();
    }
}

#include "console.inc"
#include "bench.inc"
#include "repair.inc"

// The window and taskbar icon contrast with Windows' theme: the light artwork on a dark taskbar, the dark one on a
// light taskbar (ILARTS.rc: 1 light, 2 dark). Again on a theme change and on a move to a monitor with another DPI.
void ApplyAppIcon(HWND h) {
    static HICON big = nullptr, little = nullptr;
    DWORD light = 0, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    const UINT dpi = GetDpiForWindow(h);
    const auto load = [&](int metric) {
        const int px = GetSystemMetricsForDpi(metric, dpi);
        return static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(light ? 2 : 1), IMAGE_ICON, px, px, 0));
    };
    const HICON oldBig = big, oldLittle = little;
    big = load(SM_CXICON);
    little = load(SM_CXSMICON);
    SendMessageW(h, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    SendMessageW(h, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(little));
    if (oldBig) DestroyIcon(oldBig);
    if (oldLittle) DestroyIcon(oldLittle);
}

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK: case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: case WM_MOUSELEAVE:
            if (ConsoleMouse(h, m, w, l)) return (m == WM_XBUTTONDOWN || m == WM_XBUTTONUP || m == WM_XBUTTONDBLCLK) ? TRUE : 0;
            break;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                SetCursor(g_con.cursor ? g_con.cursor : LoadCursor(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_SETFOCUS:
            // The window got the keyboard (activated, Alt+Tab ...): hand it to the page. Typing in the settings
            // needs it there; the page passes the shortcuts on ({cmd:'key'}, AcceleratorKeyPressed).
            if (g_con.controller) g_con.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            return 0;
        case WM_NCHITTEST:
            if (!g_con.on) break;  // creating the window: before the console starts
            return ConsoleHitTest(POINT{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))});
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            const int dpi = static_cast<int>(GetDpiForWindow(h) ? GetDpiForWindow(h) : 96);
            mm->ptMinTrackSize.x = MulDiv(kConsoleMinW, dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(kConsoleMinH, dpi, 96);
            return 0;
        }
        case WM_MOVE:
            if (g_con.controller) g_con.controller->NotifyParentWindowPositionChanged();
            break;
        case WM_DPICHANGED: {  // per-monitor DPI: the size Windows suggests for the new monitor
            const auto* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyAppIcon(h);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (l && lstrcmpiW(reinterpret_cast<const wchar_t*>(l), L"ImmersiveColorSet") == 0) ApplyAppIcon(h);
            break;
        case WM_SIZE:
            if (g_con.on) ConsoleResize();
            return 0;
        case WM_EXITSIZEMOVE:
            ConsoleSavePlacement();
            return 0;
        case WM_NCCALCSIZE:
            // no title bar or frame: the client area covers the window. Maximised, Windows places the window
            // its frame width beyond the screen edges, so pull the client area back in by that much.
            if (w) {
                if (IsZoomed(h)) {
                    auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
                    const int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                    const int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                    p->rgrc[0].left += fx;
                    p->rgrc[0].right -= fx;
                    p->rgrc[0].top += fy;
                    p->rgrc[0].bottom -= fy;
                }
                return 0;
            }
            break;
        case WM_NCACTIVATE:
            return DefWindowProcW(h, m, w, -1);  // nothing of the frame to repaint
        case WM_DESTROY:
            if (g_con.on) ConsoleSavePlacement();
            ConsoleShutdown();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// No WebView2 runtime, or WebView2 failed: say so (an alert) and offer Microsoft's download page.
void WebView2Notice(const std::wstring& text, HWND owner) {
    if (MessageBoxW(owner, (text + L"\n\nOpen Microsoft's download page? Install the \"Evergreen Bootstrapper\", then start ILARTS again.").c_str(),
                    L"DCS ILARTS", MB_YESNO | MB_ICONWARNING) == IDYES)
        ShellExecuteW(nullptr, L"open", L"https://developer.microsoft.com/microsoft-edge/webview2/", nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring g_testStorage;  // --test-storage <folder>: the storage limit's self-test (recfiles.inc)
DWORD g_waitPid = 0;   // --wait-pid: a restart; the old process ends first

void ParseArgs() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--auto") app.set.autoLevels = true;
        else if (a == L"--no-auto") app.set.autoLevels = false;
        else if (a == L"--raw") app.set.proc = Proc::Levels;
        else if (a == L"--no-sym") app.set.symbology = false;
        else if (a == L"--debug") app.debug = true;
        else if (a == L"--test-record" && i + 1 < argc) g_test.recordS = _wtof(argv[++i]);
        else if (a == L"--test-replay") g_test.replay = true;
        else if (a == L"--test-auto") g_test.autoMode = true;
        else if (a == L"--test-run" && i + 1 < argc) g_test.runS = _wtof(argv[++i]);
        else if (a == L"--test-stream") g_test.stream = g_streamSnap = true;
        else if (a == L"--test-cam" && i + 1 < argc) g_cam.testMask = static_cast<uint32_t>(_wtoi(argv[++i]));
        else if (a == L"--test-keys" && i + 1 < argc) {
            for (const wchar_t* p = argv[++i]; *p;) {  // "seconds:key,seconds:key"
                wchar_t* end = nullptr;
                const double t = wcstod(p, &end);
                if (end == p || *end != L':' || !end[1]) break;
                g_test.keys.push_back({t, end[1]});
                p = end + 2;
                if (*p == L',') ++p;
            }
        }
        else if (a == L"--test-log" && i + 1 < argc) g_test.log = argv[++i];
        else if (a == L"--test-storage" && i + 1 < argc) g_testStorage = argv[++i];
        else if (a == L"--test-bench-report" && i + 1 < argc) g_testBenchReport = argv[++i];
        else if (a == L"--test-bench-seconds" && i + 1 < argc) {  // measure S s per stage, settle in proportion (20 s: as usual)
            g_bench.measureS = std::clamp(_wtof(argv[++i]), 1.0, 120.0);
            g_bench.scale = g_bench.measureS / 20.0;
        }
        else if (a == L"--test-saved-games" && i + 1 < argc) g_testSavedGames = argv[++i];  // stands in for Saved Games\DCS
        else if (a == L"--test-no-webview2") g_test.noWebView2 = true;
        else if (a == L"--wait-pid" && i + 1 < argc) g_waitPid = static_cast<DWORD>(_wtoi(argv[++i]));
        else if (a == L"--test-offscreen") g_test.offscreen = true;
        else if (a == L"--test-commands") g_test.logCommands = true;
        else if (a == L"--test-script" && i + 1 < argc) {  // lines "seconds<TAB>javascript" or "seconds<TAB>!shot"
            FILE* f = nullptr;
            if (_wfopen_s(&f, argv[++i], L"r, ccs=UTF-8") == 0 && f) {
                wchar_t line[4096];
                while (fgetws(line, 4096, f)) {
                    std::wstring s = line;
                    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
                    const size_t tab = s.find(L'\t');
                    if (tab == std::wstring::npos || s[0] == L'#') continue;
                    g_test.script.push_back({_wtof(s.substr(0, tab).c_str()), s.substr(tab + 1)});
                }
                fclose(f);
            }
        }
        else if (a == L"--mode" && i + 1 < argc) {
            std::wstring v = argv[++i];
            app.set.proc = v == L"off" ? Proc::Off : (v == L"levels" ? Proc::Levels : Proc::Pipeline);
        } else if (a == L"--stops" && i + 1 < argc) app.set.stops = static_cast<float>(_wtof(argv[++i]));
        else if (a == L"--gamma" && i + 1 < argc) app.set.gamma = static_cast<float>(_wtof(argv[++i]));
        else if (a.size() == 1 && a[0] >= L'0' && a[0] < L'0' + kFeeds) app.selected = a[0] - L'0';
    }
    LocalFree(argv);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    QueryPerformanceFrequency(&app.qpf);
    {  // elevated by the viewer itself: install or remove the virtual camera component, nothing else
        const std::wstring cl = GetCommandLineW();
        if (cl.find(L"--install-cameras") != std::wstring::npos) return CameraComponentInstall(true);
        if (cl.find(L"--uninstall-cameras") != std::wstring::npos) return CameraComponentInstall(false);
    }
    {  // a restart (settings): the old process finishes its files and lets go of the hot keys first
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i + 1 < argc; ++i)
            if (wcscmp(argv[i], L"--wait-pid") == 0)
                if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(_wtoi(argv[i + 1])))) {
                    WaitForSingleObject(old, 20000);
                    CloseHandle(old);
                }
        LocalFree(argv);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC (replay frames, stills)
    const bool mf = SUCCEEDED(MFStartup(MF_VERSION));
    LoadSettings();
    if (wcsstr(GetCommandLineW(), L"--repair")) {  // the installer: put back what the mod needs, no window
        const RepairResult r = Repair(wcsstr(GetCommandLineW(), L"--report") != nullptr);
        if (wcsstr(GetCommandLineW(), L"--report"))  // the Start menu's "Repair": says what it did
            MessageBoxW(nullptr, RepairText(r).c_str(), L"DCS ILARTS - Repair", MB_OK | (r.problems.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
        return static_cast<int>(r.problems.size());
    }
    if (wcsstr(GetCommandLineW(), L"--remove-permissions"))  // the uninstaller: autoexec.cfg back as it was
        return static_cast<int>(RemovePermissions().problems.size());
    StartupCheck();  // a zip installation that was never set up: say so and offer to do it
    SyncCams();  // the cameras the last DCS session offered (sources while DCS is not running)
    LoadPresets();
    ParseArgs();
    if (!g_testStorage.empty()) return StorageSelfTest(g_testStorage, g_test.log);
    if (!g_testBenchReport.empty()) return BenchSelfTest(g_testBenchReport, g_test.log);
    OutputsApply();
    // The console is the interface (the classic window was removed 2026-11-18). Without the WebView2 runtime ILARTS
    // cannot open: it says so and offers the download page.
    if (!ConsoleAvailable()) {
        if (!ConsoleUiFolder(nullptr)) {
            MessageBoxW(nullptr, L"The ui folder next to ILARTS.exe is missing. Reinstall DCS ILARTS.", L"DCS ILARTS", MB_OK | MB_ICONERROR);
            return 1;
        }
        WebView2Notice(L"DCS ILARTS can't run on this PC yet: it needs the Microsoft Edge WebView2 Runtime, which is not installed.", nullptr);
        return 1;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"ILARTSViewer";
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&wc);

    app.hwnd = CreateWindowW(L"ILARTSViewer", kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                             CW_USEDEFAULT, 1150, 820, nullptr, nullptr, inst, nullptr);
    ApplyAppIcon(app.hwnd);
    {
        // frameless: WM_NCCALCSIZE takes effect with a frame change; a one-pixel extended frame keeps DWM's shadow
        const MARGINS shadow{0, 0, 1, 0};
        DwmExtendFrameIntoClientArea(app.hwnd, &shadow);
        SetWindowPos(app.hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!ConsoleActivate()) {  // DirectComposition or WebView2 would not start
        WebView2Notice(L"The console could not start (Microsoft Edge WebView2). ILARTS closes now.\n\n"
                       L"If this keeps happening, repair or reinstall the Microsoft Edge WebView2 Runtime.", nullptr);
        DestroyWindow(app.hwnd);
        ConsoleShutdown();
        if (mf) MFShutdown();
        return 1;
    }
    if (g_test.offscreen) show = SW_SHOWNOACTIVATE;
    else if (GetPrivateProfileIntW(L"console", L"max", 0, app.iniPath.c_str()) && show != SW_HIDE) show = SW_SHOWMAXIMIZED;
    ShowWindow(app.hwnd, show);
    if (mf) RecInit();
    if (g_test.autoMode) g_rec.autoMode = true;
    if (g_test.stream) SetStream(true);
    // Keys that work while DCS has the keyboard (ignored if another program owns them).
    LoadKeys();
    RegisterGlobalKeys();  // record / replay / black box while DCS has the keyboard

    MSG msg{};
    ULONGLONG lastMapTry = 0;
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            // Keys are handled here for every window of the app, so no child or dialog can swallow them.
            // The console: keys for the WebView's own windows are typing (a settings field) or shortcuts the page
            // passes on ({cmd:'key'}, AcceleratorKeyPressed) - they must reach it. (Until 2026-11-14 every key-down
            // was taken here: nothing could be typed into the settings dialog, and digits switched sources.)
            if (g_test.logCommands && (msg.message == WM_KEYDOWN || msg.message == WM_CHAR)) {  // tests: where keys go
                wchar_t cls[64] = {};
                GetClassNameW(msg.hwnd, cls, 64);
                DWORD pid = 0;
                GetWindowThreadProcessId(msg.hwnd, &pid);
                ConsoleLog(std::wstring(msg.message == WM_KEYDOWN ? L"keydown " : L"char ") + std::to_wstring(msg.wParam) + L" to " + cls +
                           (msg.hwnd == app.hwnd ? L" (main window)" : L"") + (pid == GetCurrentProcessId() ? L"" : L" (other process)"));
            }
            if (msg.message == WM_KEYDOWN && msg.hwnd != app.hwnd) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                continue;
            }
            if (msg.message == WM_KEYDOWN) {
                if (KeyNoRepeat(static_cast<int>(msg.wParam)) && (msg.lParam & (1 << 30))) continue;   // no auto-repeat
                HandleKey(msg.wParam);
                continue;
            }
            if (msg.message == WM_HOTKEY) {
                if (msg.wParam == 1) ToggleRecording();
                else if (msg.wParam == 2) ToggleReplay();
                else if (msg.wParam == 3) BlackBoxSaveNow();
                else if (msg.wParam == 4) StillTake();
                continue;
            }
            if (msg.message == WM_MOUSEWHEEL) {
                ConsoleMouse(app.hwnd, msg.message, msg.wParam, msg.lParam);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        ULONGLONG now = GetTickCount64();
        if (!app.header && now - lastMapTry > 1000) {
            lastMapTry = now;
            app.mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, lsoshare::kMappingName);
            if (app.mapping) {
                app.headerW = static_cast<lsoshare::Header*>(
                    MapViewOfFile(app.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(lsoshare::Header)));
                app.header = app.headerW;
            } else if ((app.mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, lsoshare::kMappingName)) != nullptr) {
                app.header = static_cast<const lsoshare::Header*>(
                    MapViewOfFile(app.mapping, FILE_MAP_READ, 0, 0, sizeof(lsoshare::Header)));
            }
            if (app.mapping && !app.header) {  // smaller than this version's header (an older ILARTS.dll): try again later
                CloseHandle(app.mapping);
                app.mapping = nullptr;
            }
        }
        SyncCams();
        Update();
        RecTick();
        BenchTick();
        ConsoleRender();
        if (TestTick()) break;
        if (g_con.failed && !g_con.failNoticed) {  // WebView2 gave up: say so, close cleanly (files finished)
            g_con.failNoticed = true;
            TestLog(L"console failed: notice shown");
            WebView2Notice(L"The console stopped working (Microsoft Edge WebView2). ILARTS closes now.\n\n"
                           L"If this keeps happening, repair or reinstall the Microsoft Edge WebView2 Runtime.", app.hwnd);
            PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
        }
        if (now - g_con.lastPush >= 100) {
            g_con.lastPush = now;
            ConsolePush();
        }
        if (!app.swap) MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT);  // the page stays responsive
    }
    ConsoleShutdown();
    if (BenchActive()) BenchFinish(L"ILARTS was closed", false);  // what was measured, and the hook told
    else if (BenchReportDue()) BenchReport();                   // a finished run still waiting for its encoders
    RecStopAll();
    SetStream(false);
    OutputsShutdown();
    g_rec.main.live.stop();
    g_rec.main.eng.stop();  // finishes the files being written
    for (auto& c : g_rec.extra) {
        c->live.stop();
        c->eng.stop();
    }
    g_rec.comp.eng.stop();
    FeedsShutdown();
    CloseDevice();
    if (mf) MFShutdown();
    if (app.header) UnmapViewOfFile(app.header);
    if (app.mapping) CloseHandle(app.mapping);
    return 0;
}
