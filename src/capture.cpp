// Frame capture of a named DCS render target, for visual verification (Phase 4).
//
// Steps (each logged):
//   1. DX11Renderer::getRenderTarget(name) -> DCS texture object (one owned reference)
//   2. read the ID3D11Resource* at texture+0x88 (offset from copyTexture disassembly, same in 2.9.25 and 2.9.31)
//   3. validate before use: pointer and its vtable readable, vtable inside a loaded module,
//      QueryInterface(ID3D11Texture2D) succeeds, and GetDevice() == DX11Renderer::getDevice()
//   4. GetDesc; create our own STAGING texture (CPU read, 1 mip, same size/format)
//   5. CopySubresourceRegion(staging, 0, ..., src, 0) on DCS's immediate context, Map(READ), Unmap
//      (no pipeline state is touched; ID3D11Multithread lock held if protection is enabled)
//   6. write <name>-<time>.dds (raw, lossless) and .bmp (8-bit preview where the format is known)
//   7. release every COM reference and the DCS texture reference

#include "capture.h"

#include "common.h"
#include "engine.h"

#include <d3d11.h>
#include <d3d11_4.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace lsodiag {
namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

const char* FormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        default: return "other";
    }
}

float HalfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31) v = 65504.0f;
    else v = std::ldexp(static_cast<float>(mant | 0x400), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

// Unsigned small floats used by R11G11B10_FLOAT: 5-bit exponent (bias 15), 6- or 5-bit mantissa.
float SmallFloat(uint32_t bits, int mantBits) {
    uint32_t exp = bits >> mantBits, mant = bits & ((1u << mantBits) - 1);
    if (exp == 0) return std::ldexp(static_cast<float>(mant), -14 - mantBits);
    if (exp == 31) return 65504.0f;
    return std::ldexp(static_cast<float>(mant | (1u << mantBits)), static_cast<int>(exp) - 15 - mantBits);
}

uint8_t ToByte(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

struct Stats {
    float minV = 1e30f, maxV = -1e30f;
    double sum = 0;
    size_t n = 0;
    void add(float v) {
        minV = std::min(minV, v);
        maxV = std::max(maxV, v);
        sum += v;
        ++n;
    }
};

// Converts one mapped row-pitched image to bottom-up 24-bit BGR. Returns false for unknown formats.
bool ToBgr24(DXGI_FORMAT f, const uint8_t* src, UINT pitch, UINT w, UINT h, std::vector<uint8_t>& out,
             UINT& outStride, Stats& st) {
    outStride = (w * 3 + 3) & ~3u;
    out.assign(static_cast<size_t>(outStride) * h, 0);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * pitch;
        uint8_t* dst = out.data() + static_cast<size_t>(h - 1 - y) * outStride;  // BMP is bottom-up
        for (UINT x = 0; x < w; ++x) {
            float r, g, b;
            switch (f) {
                case DXGI_FORMAT_R11G11B10_FLOAT: {
                    uint32_t v;
                    memcpy(&v, row + x * 4, 4);
                    r = SmallFloat(v & 0x7FF, 6);
                    g = SmallFloat((v >> 11) & 0x7FF, 6);
                    b = SmallFloat((v >> 22) & 0x3FF, 5);
                    break;
                }
                case DXGI_FORMAT_R16G16B16A16_FLOAT: {
                    uint16_t c[4];
                    memcpy(c, row + x * 8, 8);
                    r = HalfToFloat(c[0]);
                    g = HalfToFloat(c[1]);
                    b = HalfToFloat(c[2]);
                    break;
                }
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                case DXGI_FORMAT_B8G8R8X8_UNORM:
                    b = row[x * 4] / 255.0f;
                    g = row[x * 4 + 1] / 255.0f;
                    r = row[x * 4 + 2] / 255.0f;
                    break;
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                    r = row[x * 4] / 255.0f;
                    g = row[x * 4 + 1] / 255.0f;
                    b = row[x * 4 + 2] / 255.0f;
                    break;
                case DXGI_FORMAT_R10G10B10A2_UNORM: {
                    uint32_t v;
                    memcpy(&v, row + x * 4, 4);
                    r = (v & 0x3FF) / 1023.0f;
                    g = ((v >> 10) & 0x3FF) / 1023.0f;
                    b = ((v >> 20) & 0x3FF) / 1023.0f;
                    break;
                }
                default:
                    return false;
            }
            st.add(0.2126f * r + 0.7152f * g + 0.0722f * b);
            dst[x * 3] = ToByte(b);
            dst[x * 3 + 1] = ToByte(g);
            dst[x * 3 + 2] = ToByte(r);
        }
    }
    return true;
}

bool WriteFileBytes(const std::wstring& path, const void* a, size_t na, const void* b, size_t nb) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w1 = 0, w2 = 0;
    BOOL ok = WriteFile(f, a, static_cast<DWORD>(na), &w1, nullptr) &&
              (nb == 0 || WriteFile(f, b, static_cast<DWORD>(nb), &w2, nullptr));
    CloseHandle(f);
    return ok && w1 == na && w2 == nb;
}

bool WriteBmp(const std::wstring& path, const std::vector<uint8_t>& pixels, UINT w, UINT h) {
#pragma pack(push, 1)
    struct {
        BITMAPFILEHEADER fh;
        BITMAPINFOHEADER ih;
    } hdr{};
#pragma pack(pop)
    hdr.fh.bfType = 0x4D42;
    hdr.fh.bfOffBits = sizeof(hdr);
    hdr.fh.bfSize = static_cast<DWORD>(sizeof(hdr) + pixels.size());
    hdr.ih.biSize = sizeof(BITMAPINFOHEADER);
    hdr.ih.biWidth = static_cast<LONG>(w);
    hdr.ih.biHeight = static_cast<LONG>(h);
    hdr.ih.biPlanes = 1;
    hdr.ih.biBitCount = 24;
    hdr.ih.biCompression = BI_RGB;
    hdr.ih.biSizeImage = static_cast<DWORD>(pixels.size());
    return WriteFileBytes(path, &hdr, sizeof(hdr), pixels.data(), pixels.size());
}

// DDS with DX10 header: raw texels, tightly packed rows. Opens in texconv, RenderDoc, Paint.NET, GIMP.
bool WriteDds(const std::wstring& path, DXGI_FORMAT fmt, UINT w, UINT h, UINT bpp, const uint8_t* src, UINT pitch) {
    struct {
        DWORD magic = 0x20534444;  // "DDS "
        DWORD size = 124, flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8, height = 0, width = 0, pitch = 0, depth = 0,
              mips = 1, reserved[11] = {};
        struct {
            DWORD size = 32, flags = 0x4, fourCC = 0x30315844 /* DX10 */, bits = 0, r = 0, g = 0, b = 0, a = 0;
        } pf;
        DWORD caps = 0x1000, caps2 = 0, caps3 = 0, caps4 = 0, reserved2 = 0;
        DWORD dxgiFormat = 0, dimension = 3 /* TEXTURE2D */, miscFlag = 0, arraySize = 1, miscFlags2 = 0;
    } hdr;
    hdr.height = h;
    hdr.width = w;
    hdr.pitch = w * bpp;
    hdr.dxgiFormat = static_cast<DWORD>(fmt);
    std::vector<uint8_t> packed(static_cast<size_t>(w) * bpp * h);
    for (UINT y = 0; y < h; ++y) memcpy(packed.data() + static_cast<size_t>(y) * w * bpp, src + static_cast<size_t>(y) * pitch, static_cast<size_t>(w) * bpp);
    return WriteFileBytes(path, &hdr, sizeof(hdr), packed.data(), packed.size());
}

UINT BytesPerPixel(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
        default: return 0;
    }
}

// Validates that p looks like a live COM object (readable pointer, readable vtable inside a module).
bool PlausibleComObject(const void* p, std::string& where) {
    const void* vtbl = nullptr;
    const void* first = nullptr;
    if (!p || !SafeReadPtr(p, &vtbl) || !vtbl || !SafeReadPtr(vtbl, &first) || !first) return false;
    where = DescribeAddress(vtbl);
    return where.find('+') != std::string::npos;  // "module.dll+0x..." means inside an image
}

// SEH-wrapped QueryInterface on an object we have only plausibility-checked.
HRESULT SehQueryTexture2D(IUnknown* u, ID3D11Texture2D** out, DWORD* code) {
    __try {
        return u->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(out));
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

std::wstring CaptureDir() {
    const std::wstring& p = LogPath();
    size_t slash = p.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash)) + L"\\captures";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring Widen(const char* s) {
    std::wstring w;
    for (; *s; ++s) w += (*s >= 32 && *s < 127 && !strchr("\\/:*?\"<>|", *s)) ? static_cast<wchar_t>(*s) : L'_';
    return w;
}

// Inner capture once the D3D texture is validated. Returns a result line ("" on failure; error in why).
std::string CopyAndSave(const char* name, ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                        std::string& why) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    Log("CAPTURE %s desc: %ux%u fmt=%d(%s) mips=%u array=%u samples=%u usage=%d bind=0x%X cpu=0x%X misc=0x%X", name,
        d.Width, d.Height, static_cast<int>(d.Format), FormatName(d.Format), d.MipLevels, d.ArraySize, d.SampleDesc.Count,
        static_cast<int>(d.Usage), d.BindFlags, d.CPUAccessFlags, d.MiscFlags);
    if (d.SampleDesc.Count != 1) {
        why = "multisampled texture: needs ResolveSubresource, not implemented";
        return {};
    }
    UINT bpp = BytesPerPixel(d.Format);
    if (!bpp) {
        why = "unsupported DXGI format for capture";
        return {};
    }

    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    Com<ID3D11Texture2D> staging;
    HRESULT hr = dev->CreateTexture2D(&sd, nullptr, staging.put());
    if (FAILED(hr)) {
        why = "CreateTexture2D(staging) failed hr=" + std::to_string(hr);
        return {};
    }

    // Serialise with other context users if D3D11 multithread protection is on.
    Com<ID3D11Multithread> mt;
    bool locked = false;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(mt.put()))) && mt &&
        mt->GetMultithreadProtected()) {
        mt->Enter();
        locked = true;
    }
    ctx->CopySubresourceRegion(staging.p, 0, 0, 0, 0, tex, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    hr = ctx->Map(staging.p, 0, D3D11_MAP_READ, 0, &m);  // blocks until the GPU copy completes (one-off stall)
    std::vector<uint8_t> copy;
    UINT pitch = 0;
    if (SUCCEEDED(hr)) {
        pitch = m.RowPitch;
        copy.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(pitch) * d.Height);
        ctx->Unmap(staging.p, 0);
    }
    if (locked) mt->Leave();
    Log("CAPTURE %s multithread-protected=%s map hr=0x%08lX", name, locked ? "yes" : "no", static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        why = "Map(staging) failed";
        return {};
    }

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf_s(stamp, L"-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring base = CaptureDir() + L"\\" + Widen(name) + stamp;

    bool ddsOk = WriteDds(base + L".dds", d.Format, d.Width, d.Height, bpp, copy.data(), pitch);
    std::vector<uint8_t> bgr;
    UINT stride = 0;
    Stats st;
    bool bmpOk = ToBgr24(d.Format, copy.data(), pitch, d.Width, d.Height, bgr, stride, st) &&
                 WriteBmp(base + L".bmp", bgr, d.Width, d.Height);
    char line[512];
    _snprintf_s(line, _countof(line), _TRUNCATE,
                "%ux%u %s luminance min=%.4f max=%.4f mean=%.4f dds=%s bmp=%s -> %s.(dds|bmp)", d.Width, d.Height,
                FormatName(d.Format), st.n ? st.minV : 0.0f, st.n ? st.maxV : 0.0f, st.n ? st.sum / st.n : 0.0,
                ddsOk ? "ok" : "FAILED", bmpOk ? "ok" : "FAILED", Utf8(base).c_str());
    Log("CAPTURE %s %s", name, line);
    return line;
}

}  // namespace

bool RendererDevice(const Renderer& r, ID3D11Device** dev, ID3D11DeviceContext** ctx) {
    DWORD code = 0;
    *dev = static_cast<ID3D11Device*>(SehGetPtr(r.getDevice, r.self, &code));
    if (!code) *ctx = static_cast<ID3D11DeviceContext*>(SehGetPtr(r.getDeviceContext, r.self, &code));
    if (code) {
        DisableEngine("getDevice/getDeviceContext", code);
        return false;
    }
    return *dev && *ctx;
}

ID3D11Texture2D* OpenDcsTexture(const Renderer& r, const char* name, bool verbose, std::string& why) {
    DWORD code = 0;
    void* texture = SehGetRenderTarget(r, name, &code);
    if (code) {
        DisableEngine("getRenderTarget", code);
        why = "exception in getRenderTarget";
        return nullptr;
    }
    if (!texture) {
        why = "render target not found";
        return nullptr;
    }

    ID3D11Texture2D* out = nullptr;
    do {
        LONG refs = 0;
        SafeRead(static_cast<char*>(texture) + kTextureRefCountOffset, &refs, sizeof(refs));
        const void* resource = nullptr;
        SafeReadPtr(static_cast<char*>(texture) + kTextureResourceOffset, &resource);
        std::string where;
        bool plausible = PlausibleComObject(resource, where);
        if (verbose) {
            Log("TEXTURE %s object=%p refs=%ld resource(+0x%zX)=%p vtable@%s", name, texture, refs,
                kTextureResourceOffset, resource, plausible ? where.c_str() : "<implausible>");
        }
        if (!plausible) {
            why = "texture+0x88 is not a plausible COM object";
            break;
        }
        Com<ID3D11Texture2D> tex;
        HRESULT hr = SehQueryTexture2D(static_cast<IUnknown*>(const_cast<void*>(resource)), tex.put(), &code);
        if (code) {
            DisableEngine("QueryInterface(texture+0x88)", code);
            why = "exception in QueryInterface";
            break;
        }
        if (FAILED(hr) || !tex) {
            why = "texture+0x88 is not an ID3D11Texture2D";
            break;
        }
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        if (!RendererDevice(r, &dev, &ctx)) {
            why = "renderer device/context unavailable";
            break;
        }
        Com<ID3D11Device> texDev;
        tex->GetDevice(texDev.put());
        if (verbose) Log("TEXTURE %s device=%p renderer device=%p context=%p", name, texDev.p, dev, ctx);
        if (texDev.p != dev) {
            why = "texture belongs to a different D3D11 device";
            break;
        }
        out = tex.p;  // transfer our QueryInterface reference to the caller
        tex.p = nullptr;
    } while (false);

    // Drop the DCS ed::Ptr reference; the caller keeps the D3D reference independently.
    if (!SehReleaseTexture(texture, &code)) DisableEngine("texture release", code);
    return out;
}

std::string CaptureRenderTarget(const char* name, std::string& why) {
    Renderer r;
    why = ResolveRenderer(r);
    if (!why.empty()) return {};
    std::string result;
    Com<ID3D11Texture2D> tex;
    tex.p = OpenDcsTexture(r, name, true, why);
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (tex && RendererDevice(r, &dev, &ctx)) result = CopyAndSave(name, dev, ctx, tex.p, why);
    if (!why.empty()) Log("CAPTURE %s failed: %s", name, why.c_str());
    return result;
}

}  // namespace lsodiag
