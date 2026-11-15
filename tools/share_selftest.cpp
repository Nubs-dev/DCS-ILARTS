// Offline self-test of the Phase 5 sharing mechanism (no DCS involved).
// For each format DCS uses: create a texture with exactly the producer's flags on device A,
// publish it as a named NT handle, open it by name on an independent device B, synchronise with
// the keyed mutex (key 0), copy, read back on B and compare with what A wrote.

#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// --serve <seconds> <capture.dds> [capture.dds ...]: publishes captured DCS frames through the real
// sharing protocol (same mapping, flags, keyed mutex and naming as src/share.cpp) so the viewer can
// be tested without DCS. Target name = file name up to the first '-' (e.g. LSOFRAME-2026...dds).
// Uses the adapter with the most dedicated VRAM (the discrete GPU DCS uses).
struct Served {
    std::string name;
    ID3D11Texture2D* src = nullptr;
    ID3D11Texture2D* shared = nullptr;
    IDXGIKeyedMutex* km = nullptr;
    HANDLE handle = nullptr;
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

int Serve(int seconds, int count, wchar_t** paths) {
    if (count < 1 || count > lsoshare::kMaxTargets) { printf("1..%d dds files\n", lsoshare::kMaxTargets); return 1; }
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
    hdr->targetCount = static_cast<uint32_t>(count);
    hdr->version = lsoshare::kVersion;

    std::vector<Served> served(count);
    for (int i = 0; i < count; ++i) {
        std::vector<unsigned char> file;
        UINT w = 0, h = 0, bpp = 0;
        DXGI_FORMAT fmt{};
        if (!LoadDds(paths[i], file, w, h, fmt, bpp)) { printf("cannot load %ls\n", paths[i]); return 1; }
        std::wstring base = paths[i];
        base = base.substr(base.find_last_of(L"\\/") + 1);
        base = base.substr(0, base.find(L'-'));
        Served& s = served[i];
        s.name.assign(base.begin(), base.end());
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
    }
    hdr->magic = lsoshare::kMagic;
    printf("on %ls for %d s\n", bd.Description, seconds);

    ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    unsigned long long copies = 0, skipped = 0;
    while (GetTickCount64() < end) {
        for (int i = 0; i < count; ++i) {
            Served& s = served[i];
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
    if (argc >= 4 && wcscmp(argv[1], L"--serve") == 0) return Serve(_wtoi(argv[2]), argc - 3, argv + 3);
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
