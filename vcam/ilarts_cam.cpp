// ILARTSCam.dll: the DCS ILARTS virtual cameras (Windows 11). A Media Foundation media source per camera (one COM
// class each, vcam_shared.h), loaded by Windows' camera service (Frame Server) when an application opens "ILARTS
// PLAT" and so on. The pictures come from the viewer through a shared block (vcam_shared.h); without one (the viewer
// closed, or not drawing that source yet) the camera delivers black.
//
// Shape (as Microsoft's VirtualCamera sample): the class factory makes an IMFActivate; Frame Server activates the
// media source (IMFMediaSourceEx, IMFGetService, IKsControl) with one video stream (IMFMediaStream2, IKsControl).
// Every RequestSample delivers one frame, paced to the frame rate.
//
// Registration (admin): DllRegisterServer writes HKLM\SOFTWARE\Classes\CLSID\{...}\InprocServer32 for the six
// classes; the viewer's --install-cameras copies this DLL to Program Files first (Frame Server cannot read a user's
// folders).
#include <initguid.h>

#include <windows.h>

#include <ks.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "vcam_shared.h"

// IKsControl (ksproxy.h, which only declares it for kernel-streaming proxies): Frame Server asks every camera source
// and stream for it
MIDL_INTERFACE("28F54685-06FD-11D2-B27A-00A0C9223196")
IKsControl : public IUnknown {
  public:
    virtual HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) = 0;
};

// KSCATEGORY / pin names (ksmedia.h), stated here to keep that header out
DEFINE_GUID(kPinCapture, 0xfb6c4281, 0x0353, 0x11d1, 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba);         // PINNAME_VIDEO_CAPTURE
DEFINE_GUID(kProfileLegacy, 0xb4894d81, 0x62b7, 0x4eec, 0x87, 0x40, 0x80, 0x65, 0x8c, 0x4a, 0x9d, 0x3e);      // KSCAMERAPROFILE_Legacy

namespace {

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0}, g_locks{0};

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// A short log for when it does not work: %ProgramData%\DCS ILARTS\ILARTSCam.log (the installer makes the folder
// writable; without it nothing is logged). Kept under 256 kB.
void Log(const char* fmt, ...) {
    wchar_t path[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%ProgramData%\\DCS ILARTS\\ILARTSCam.log", path, MAX_PATH)) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"ab") != 0 || !f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) < 256 * 1024) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                GetCurrentProcessId());
        va_list a;
        va_start(a, fmt);
        vfprintf(f, fmt, a);
        va_end(a);
        fputs("\r\n", f);
    }
    fclose(f);
}

// ---- the shared block (vcam_shared.h): created here, opened by the viewer
struct Shared {
    HANDLE map = nullptr;
    ilcam::Block* b = nullptr;
    bool open(int cam) {
        if (b) return true;
        // anyone may open it (the viewer runs as the user, this as a service), also at low integrity
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;AC)S:(ML;;NW;;;LW)",
                                                                 SDDL_REVISION_1, &sd, nullptr))
            sa.lpSecurityDescriptor = sd;
        wchar_t name[64];
        for (bool global : {true, false}) {  // a service may create Global\ names; anything else gets its session's
            ilcam::BlockName(cam, name, 64, global);
            map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, static_cast<DWORD>(ilcam::kBlockBytes), name);
            if (map) break;
        }
        if (sd) LocalFree(sd);
        if (!map) {
            Log("camera %d: no shared block (%lu)", cam, GetLastError());
            return false;
        }
        const bool fresh = GetLastError() != ERROR_ALREADY_EXISTS;
        b = static_cast<ilcam::Block*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!b) {
            CloseHandle(map);
            map = nullptr;
            return false;
        }
        if (fresh || b->magic != ilcam::kMagic) {
            b->version = ilcam::kVersion;
            b->seq = 0;
            b->frameW = b->frameH = 0;
            b->writeTick = 0;
            MemoryBarrier();
            b->magic = ilcam::kMagic;
        }
        Log("camera %d: shared block %ls (%s)", cam, name, fresh ? "new" : "existing");
        return true;
    }
    void close() {
        if (b) UnmapViewOfFile(b);
        if (map) CloseHandle(map);
        b = nullptr;
        map = nullptr;
    }
    ~Shared() { close(); }
};

// ---- picture formats
struct Format {
    GUID subtype;
    UINT32 w, h;
};
const Format kFormats[] = {
    {MFVideoFormat_NV12, 1280, 720}, {MFVideoFormat_NV12, 1920, 1080}, {MFVideoFormat_NV12, 960, 720},
    {MFVideoFormat_NV12, 640, 360},  {MFVideoFormat_RGB32, 1280, 720},
};
constexpr UINT32 kFps = 30;

HRESULT MakeType(const Format& f, IMFMediaType** out) {
    IMFMediaType* t = nullptr;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    const bool rgb = f.subtype == MFVideoFormat_RGB32;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, f.subtype);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, f.w, f.h);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE_RANGE_MAX, kFps, 1);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE_RANGE_MIN, kFps, 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, rgb ? f.w * 4 : f.w);  // top-down
    t->SetUINT32(MF_MT_SAMPLE_SIZE, rgb ? f.w * f.h * 4 : f.w * f.h * 3 / 2);
    t->SetUINT32(MF_MT_AVG_BITRATE, (rgb ? f.w * f.h * 32 : f.w * f.h * 12) * kFps);
    *out = t;
    return S_OK;
}

// BGRA (sw x sh) to the frame (dst: w x h, NV12 or RGB32), scaled to fit, aspect kept, black around.
void Convert(const uint8_t* src, int sw, int sh, bool rgb, uint8_t* dst, int w, int h) {
    if (rgb) memset(dst, 0, static_cast<size_t>(w) * h * 4);
    else {
        memset(dst, 16, static_cast<size_t>(w) * h);
        memset(dst + static_cast<size_t>(w) * h, 128, static_cast<size_t>(w) * h / 2);
    }
    if (!src || sw <= 0 || sh <= 0) return;
    // the picture's rectangle in the frame
    const double s = std::min(static_cast<double>(w) / sw, static_cast<double>(h) / sh);
    int rw = std::max(2, static_cast<int>(sw * s + 0.5)) & ~1, rh = std::max(2, static_cast<int>(sh * s + 0.5)) & ~1;
    rw = std::min(rw, w), rh = std::min(rh, h);
    const int x0 = ((w - rw) / 2) & ~1, y0 = ((h - rh) / 2) & ~1;
    std::vector<int> mapX(rw);
    for (int x = 0; x < rw; ++x) mapX[x] = std::min(sw - 1, static_cast<int>((x + 0.5) * sw / rw));
    auto srcRow = [&](int y) { return src + static_cast<size_t>(std::min(sh - 1, static_cast<int>((y + 0.5) * sh / rh))) * sw * 4; };
    if (rgb) {
        for (int y = 0; y < rh; ++y) {
            const uint8_t* r = srcRow(y);
            uint32_t* o = reinterpret_cast<uint32_t*>(dst + (static_cast<size_t>(y0 + y) * w + x0) * 4);
            for (int x = 0; x < rw; ++x) o[x] = *reinterpret_cast<const uint32_t*>(r + mapX[x] * 4) | 0xFF000000u;
        }
        return;
    }
    uint8_t* Y = dst;
    uint8_t* UV = dst + static_cast<size_t>(w) * h;
    for (int y = 0; y < rh; ++y) {  // BT.601, video range
        const uint8_t* r = srcRow(y);
        uint8_t* o = Y + static_cast<size_t>(y0 + y) * w + x0;
        for (int x = 0; x < rw; ++x) {
            const uint8_t* p = r + mapX[x] * 4;
            o[x] = static_cast<uint8_t>(((66 * p[2] + 129 * p[1] + 25 * p[0] + 128) >> 8) + 16);
        }
    }
    for (int y = 0; y < rh; y += 2) {
        const uint8_t* r = srcRow(y);
        uint8_t* o = UV + static_cast<size_t>((y0 + y) / 2) * w + x0;
        for (int x = 0; x < rw; x += 2) {
            const uint8_t* p = r + mapX[x] * 4;
            const int R = p[2], G = p[1], B = p[0];
            o[x] = static_cast<uint8_t>(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
            o[x + 1] = static_cast<uint8_t>(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
        }
    }
}

HRESULT KsNotFound() { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

class Source;

// ---------------------------------------------------------------- the stream
class Stream : public IMFMediaStream2, public IKsControl {
  public:
    Stream(Source* parent, int cam) : parent_(parent), cam_(cam) {
        InitializeCriticalSection(&cs_);
        ++g_objects;
    }
    HRESULT init() {
        HRESULT hr = MFCreateEventQueue(&queue_);
        IMFMediaType* types[ARRAYSIZE(kFormats)] = {};
        for (int i = 0; SUCCEEDED(hr) && i < ARRAYSIZE(kFormats); ++i) hr = MakeType(kFormats[i], &types[i]);
        if (SUCCEEDED(hr)) hr = MFCreateStreamDescriptor(0, ARRAYSIZE(kFormats), types, &sd_);
        IMFMediaTypeHandler* h = nullptr;
        if (SUCCEEDED(hr)) hr = sd_->GetMediaTypeHandler(&h);
        if (SUCCEEDED(hr)) hr = h->SetCurrentMediaType(types[0]);
        SafeRelease(h);
        for (auto*& t : types) SafeRelease(t);
        if (SUCCEEDED(hr)) {
            sd_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, kPinCapture);
            sd_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
            sd_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
            sd_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
        }
        return hr;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaStream) || riid == __uuidof(IMFMediaStream2))
            *ppv = static_cast<IMFMediaStream2*>(this);
        else if (riid == __uuidof(IKsControl))
            *ppv = static_cast<IKsControl*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override {
        IMFMediaEventQueue* q = nullptr;
        {
            Lock l(cs_);
            if (shut_) return MF_E_SHUTDOWN;
            q = queue_;
            q->AddRef();
        }
        const HRESULT hr = q->GetEvent(flags, e);  // may block: outside the lock
        q->Release();
        return hr;
    }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) override {
        Lock l(cs_);
        return shut_ ? MF_E_SHUTDOWN : queue_->BeginGetEvent(cb, st);
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override {
        Lock l(cs_);
        return shut_ ? MF_E_SHUTDOWN : queue_->EndGetEvent(r, e);
    }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT s, const PROPVARIANT* v) override {
        Lock l(cs_);
        return shut_ ? MF_E_SHUTDOWN : queue_->QueueEventParamVar(t, g, s, v);
    }

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** out) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** out) override {
        Lock l(cs_);
        if (shut_) return MF_E_SHUTDOWN;
        if (!out) return E_POINTER;
        *out = sd_;
        sd_->AddRef();
        return S_OK;
    }
    STDMETHODIMP RequestSample(IUnknown* token) override {
        Lock l(cs_);
        if (shut_) return MF_E_SHUTDOWN;
        if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
        IMFSample* s = nullptr;
        HRESULT hr = makeSample(&s);
        if (SUCCEEDED(hr) && token) s->SetUnknown(MFSampleExtension_Token, token);
        if (SUCCEEDED(hr)) hr = queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, s);
        SafeRelease(s);
        if (FAILED(hr) && !errLogged_) {
            errLogged_ = true;
            Log("camera %d: RequestSample failed 0x%08lx", cam_, hr);
        }
        return hr;
    }

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE v) override {
        Lock l(cs_);
        if (shut_) return MF_E_SHUTDOWN;
        if (v == state_) return S_OK;
        if (v == MF_STREAM_STATE_RUNNING) return startLocked(nullptr);
        if (v == MF_STREAM_STATE_STOPPED) return stopLocked();
        return MF_E_INVALID_STATE_TRANSITION;
    }
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* v) override {
        Lock l(cs_);
        if (shut_) return MF_E_SHUTDOWN;
        if (!v) return E_POINTER;
        *v = state_;
        return S_OK;
    }

    // IKsControl: nothing to control
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return KsNotFound(); }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return KsNotFound(); }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return KsNotFound(); }

    // from the source
    HRESULT start(const PROPVARIANT* at) {
        Lock l(cs_);
        if (shut_) return MF_E_SHUTDOWN;
        return startLocked(at);
    }
    HRESULT stop() {
        Lock l(cs_);
        if (shut_) return MF_E_SHUTDOWN;
        return stopLocked();
    }
    bool running() {
        Lock l(cs_);
        return state_ == MF_STREAM_STATE_RUNNING;
    }
    void shutdown() {
        Lock l(cs_);
        if (shut_) return;
        shut_ = true;
        if (queue_) queue_->Shutdown();
        SafeRelease(queue_);
        SafeRelease(sd_);
        shared_.close();
        parent_ = nullptr;
    }

  private:
    struct Lock {
        CRITICAL_SECTION& c;
        explicit Lock(CRITICAL_SECTION& x) : c(x) { EnterCriticalSection(&c); }
        ~Lock() { LeaveCriticalSection(&c); }
    };
    ~Stream() {
        shutdown();
        DeleteCriticalSection(&cs_);
        --g_objects;
    }
    HRESULT startLocked(const PROPVARIANT* at) {
        state_ = MF_STREAM_STATE_RUNNING;
        next_ = 0;
        shared_.open(cam_);
        Log("camera %d: started", cam_);
        PROPVARIANT none;
        PropVariantInit(&none);
        return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, at ? at : &none);
    }
    HRESULT stopLocked() {
        state_ = MF_STREAM_STATE_STOPPED;
        Log("camera %d: stopped", cam_);
        return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }
    // One frame, from the viewer's picture if it has a fresh one; paced to the frame rate.
    HRESULT makeSample(IMFSample** out) {
        IMFMediaTypeHandler* h = nullptr;
        IMFMediaType* t = nullptr;
        HRESULT hr = sd_->GetMediaTypeHandler(&h);
        if (SUCCEEDED(hr)) hr = h->GetCurrentMediaType(&t);
        SafeRelease(h);
        if (FAILED(hr)) return hr;
        GUID sub{};
        UINT32 w = 0, hgt = 0;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &hgt);
        SafeRelease(t);
        if (!w || !hgt || w > ilcam::kMaxW || hgt > ilcam::kMaxH) return MF_E_INVALIDMEDIATYPE;
        const bool rgb = sub == MFVideoFormat_RGB32;
        // pacing
        const LONGLONG step = 10000000 / kFps;
        LONGLONG now = MFGetSystemTime();
        if (next_ && now < next_) {
            Sleep(static_cast<DWORD>(std::min<LONGLONG>((next_ - now) / 10000, 40)));
            now = MFGetSystemTime();
        }
        next_ = (next_ && now - next_ < step) ? next_ + step : now + step;
        // the viewer's picture
        bool have = false;
        int sw = 0, sh = 0;
        if (shared_.b || shared_.open(cam_)) {
            ilcam::Block* b = shared_.b;
            InterlockedExchange(&b->askW, static_cast<LONG>(w));
            InterlockedExchange(&b->askH, static_cast<LONG>(hgt));
            InterlockedExchange64(&b->readTick, static_cast<LONG64>(GetTickCount64()));
            const LONG s0 = b->seq;
            MemoryBarrier();
            sw = b->frameW, sh = b->frameH;
            const bool fresh = GetTickCount64() - static_cast<ULONGLONG>(b->writeTick) < 2000;
            if (!(s0 & 1) && fresh && sw > 0 && sh > 0 && sw <= ilcam::kMaxW && sh <= ilcam::kMaxH) {
                pic_.resize(static_cast<size_t>(sw) * sh * 4);
                memcpy(pic_.data(), reinterpret_cast<const uint8_t*>(b + 1), pic_.size());
                MemoryBarrier();
                have = b->seq == s0;  // not rewritten meanwhile
            }
        }
        const DWORD bytes = rgb ? w * hgt * 4 : w * hgt * 3 / 2;
        IMFMediaBuffer* buf = nullptr;
        IMFSample* s = nullptr;
        hr = MFCreateMemoryBuffer(bytes, &buf);
        BYTE* p = nullptr;
        if (SUCCEEDED(hr)) hr = buf->Lock(&p, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            Convert(have ? pic_.data() : nullptr, sw, sh, rgb, p, static_cast<int>(w), static_cast<int>(hgt));
            buf->Unlock();
            buf->SetCurrentLength(bytes);
            hr = MFCreateSample(&s);
        }
        if (SUCCEEDED(hr)) hr = s->AddBuffer(buf);
        if (SUCCEEDED(hr)) hr = s->SetSampleTime(now);
        if (SUCCEEDED(hr)) hr = s->SetSampleDuration(step);
        SafeRelease(buf);
        if (FAILED(hr)) {
            SafeRelease(s);
            return hr;
        }
        *out = s;
        return S_OK;
    }

    LONG ref_ = 1;
    CRITICAL_SECTION cs_;
    Source* parent_;
    int cam_;
    bool shut_ = false, errLogged_ = false;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    IMFMediaEventQueue* queue_ = nullptr;
    IMFStreamDescriptor* sd_ = nullptr;
    Shared shared_;
    std::vector<uint8_t> pic_;
    LONGLONG next_ = 0;
};

// ---------------------------------------------------------------- the source
class Source : public IMFMediaSourceEx, public IMFGetService, public IKsControl {
  public:
    explicit Source(int cam) : cam_(cam) {
        InitializeCriticalSection(&cs_);
        ++g_objects;
    }
    HRESULT init(IMFAttributes* from) {
        HRESULT hr = MFCreateEventQueue(&queue_);
        if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attrs_, 4);
        if (SUCCEEDED(hr) && from) from->CopyAllItems(attrs_);
        if (SUCCEEDED(hr)) {  // one profile: the legacy one, everything allowed
            IMFSensorProfileCollection* col = nullptr;
            IMFSensorProfile* prof = nullptr;
            if (SUCCEEDED(MFCreateSensorProfileCollection(&col)) && SUCCEEDED(MFCreateSensorProfile(kProfileLegacy, 0, nullptr, &prof)) &&
                SUCCEEDED(prof->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))")) && SUCCEEDED(col->AddProfile(prof)))
                attrs_->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, col);
            SafeRelease(prof);
            SafeRelease(col);
        }
        if (SUCCEEDED(hr)) {
            stream_ = new (std::nothrow) Stream(this, cam_);
            hr = stream_ ? stream_->init() : E_OUTOFMEMORY;
        }
        IMFStreamDescriptor* sd = nullptr;
        if (SUCCEEDED(hr)) hr = stream_->GetStreamDescriptor(&sd);
        if (SUCCEEDED(hr)) hr = MFCreatePresentationDescriptor(1, &sd, &pd_);
        if (SUCCEEDED(hr)) hr = pd_->SelectStream(0);
        SafeRelease(sd);
        Log("camera %d: source created (0x%08lx)", cam_, hr);
        return hr;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaSourceEx))
            *ppv = static_cast<IMFMediaSourceEx*>(this);
        else if (riid == __uuidof(IMFGetService))
            *ppv = static_cast<IMFGetService*>(this);
        else if (riid == __uuidof(IKsControl))
            *ppv = static_cast<IKsControl*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override {
        IMFMediaEventQueue* q = nullptr;
        {
            Lock l(cs_);
            if (shut_) return MF_E_SHUTDOWN;
            q = queue_;
            q->AddRef();
        }
        const HRESULT hr = q->GetEvent(flags, e);
        q->Release();
        return hr;
    }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) override {
        Lock l(cs_);
        return shut_ ? MF_E_SHUTDOWN : queue_->BeginGetEvent(cb, st);
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override {
        Lock l(cs_);
        return shut_ ? MF_E_SHUTDOWN : queue_->EndGetEvent(r, e);
    }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT s, const PROPVARIANT* v) override {
        Lock l(cs_);
        return shut_ ? MF_E_SHUTDOWN : queue_->QueueEventParamVar(t, g, s, v);
    }

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* c) override {
        Lock l(cs_);
        if (shut_) return MF_E_SHUTDOWN;
        if (!c) return E_POINTER;
        *c = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** out) override {
        Lock l(cs_);
        if (shut_) return MF_E_SHUTDOWN;
        if (!out) return E_POINTER;
        return pd_->Clone(out);
    }
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* fmt, const PROPVARIANT* at) override {
        Lock l(cs_);
        if (shut_) return MF_E_SHUTDOWN;
        if (!pd) return E_INVALIDARG;
        if (fmt && *fmt != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        BOOL selected = FALSE;
        IMFStreamDescriptor* sd = nullptr;
        HRESULT hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
        SafeRelease(sd);
        if (FAILED(hr)) return hr;
        if (selected) {
            const bool was = stream_->running();
            hr = queue_->QueueEventParamUnk(was ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, static_cast<IMFMediaStream2*>(stream_));
            if (SUCCEEDED(hr)) hr = stream_->start(at);
        }
        if (SUCCEEDED(hr)) hr = queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, at);
        return hr;
    }
    STDMETHODIMP Stop() override {
        Lock l(cs_);
        if (shut_) return MF_E_SHUTDOWN;
        if (stream_->running()) stream_->stop();
        return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    }
    STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    STDMETHODIMP Shutdown() override {
        Lock l(cs_);
        if (shut_) return S_OK;
        shut_ = true;
        if (stream_) stream_->shutdown();
        SafeRelease(stream_);
        if (queue_) queue_->Shutdown();
        SafeRelease(queue_);
        SafeRelease(pd_);
        SafeRelease(attrs_);
        Log("camera %d: source shut down", cam_);
        return S_OK;
    }

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** out) override {
        Lock l(cs_);
        if (shut_) return MF_E_SHUTDOWN;
        if (!out) return E_POINTER;
        *out = attrs_;
        attrs_->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** out) override {
        Lock l(cs_);
        if (shut_) return MF_E_SHUTDOWN;
        if (!out) return E_POINTER;
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        IMFStreamDescriptor* sd = nullptr;
        HRESULT hr = stream_->GetStreamDescriptor(&sd);
        if (SUCCEEDED(hr)) hr = sd->QueryInterface(IID_PPV_ARGS(out));
        SafeRelease(sd);
        return hr;
    }
    STDMETHODIMP SetD3DManager(IUnknown*) override { return S_OK; }  // frames are in system memory

    // IMFGetService
    STDMETHODIMP GetService(REFGUID, REFIID, LPVOID*) override { return MF_E_UNSUPPORTED_SERVICE; }

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return KsNotFound(); }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return KsNotFound(); }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return KsNotFound(); }

    bool alive() {
        Lock l(cs_);
        return !shut_;
    }

  private:
    struct Lock {
        CRITICAL_SECTION& c;
        explicit Lock(CRITICAL_SECTION& x) : c(x) { EnterCriticalSection(&c); }
        ~Lock() { LeaveCriticalSection(&c); }
    };
    ~Source() {
        Shutdown();
        DeleteCriticalSection(&cs_);
        --g_objects;
    }
    LONG ref_ = 1;
    CRITICAL_SECTION cs_;
    int cam_;
    bool shut_ = false;
    IMFMediaEventQueue* queue_ = nullptr;
    IMFAttributes* attrs_ = nullptr;
    IMFPresentationDescriptor* pd_ = nullptr;
    Stream* stream_ = nullptr;
};

STDMETHODIMP Stream::GetMediaSource(IMFMediaSource** out) {
    Lock l(cs_);
    if (shut_ || !parent_) return MF_E_SHUTDOWN;
    if (!out) return E_POINTER;
    *out = parent_;
    parent_->AddRef();
    return S_OK;
}

// ---------------------------------------------------------------- activation
// The object Frame Server creates: attributes it may set, and the source on ActivateObject.
class Activate : public IMFActivate {
  public:
    explicit Activate(int cam) : cam_(cam) {
        ++g_objects;
        MFCreateAttributes(&a_, 4);
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate)) {
            *ppv = static_cast<IMFActivate*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!source_) {
            Source* s = new (std::nothrow) Source(cam_);
            if (!s) return E_OUTOFMEMORY;
            const HRESULT hr = s->init(a_);
            if (FAILED(hr)) {
                s->Release();
                return hr;
            }
            source_ = s;
        }
        return source_->QueryInterface(riid, ppv);
    }
    STDMETHODIMP ShutdownObject() override {
        if (source_) source_->Shutdown();
        SafeRelease(source_);
        return S_OK;
    }
    STDMETHODIMP DetachObject() override {
        SafeRelease(source_);
        return S_OK;
    }
    // IMFAttributes: the store's
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return a_->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return a_->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return a_->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return a_->Compare(o, m, r); }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return a_->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return a_->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return a_->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return a_->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* n) override { return a_->GetStringLength(k, n); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR v, UINT32 n, UINT32* l) override { return a_->GetString(k, v, n, l); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* v, UINT32* l) override { return a_->GetAllocatedString(k, v, l); }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return a_->GetBlobSize(k, n); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* l) override { return a_->GetBlob(k, b, n, l); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return a_->GetAllocatedBlob(k, b, n); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID r, LPVOID* p) override { return a_->GetUnknown(k, r, p); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return a_->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) override { return a_->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() override { return a_->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return a_->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return a_->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return a_->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return a_->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return a_->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return a_->SetBlob(k, b, n); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* p) override { return a_->SetUnknown(k, p); }
    STDMETHODIMP LockStore() override { return a_->LockStore(); }
    STDMETHODIMP UnlockStore() override { return a_->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* n) override { return a_->GetCount(n); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return a_->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return a_->CopyAllItems(d); }

  private:
    ~Activate() {
        if (source_) source_->Shutdown();
        SafeRelease(source_);
        SafeRelease(a_);
        --g_objects;
    }
    LONG ref_ = 1;
    int cam_;
    IMFAttributes* a_ = nullptr;
    Source* source_ = nullptr;
};

class Factory : public IClassFactory {
  public:
    explicit Factory(int cam) : cam_(cam) { ++g_objects; }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        Activate* a = new (std::nothrow) Activate(cam_);
        if (!a) return E_OUTOFMEMORY;
        const HRESULT hr = a->QueryInterface(riid, ppv);
        a->Release();
        Log("camera %d: activation object created (0x%08lx)", cam_, hr);
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_locks;
        else --g_locks;
        return S_OK;
    }

  private:
    ~Factory() { --g_objects; }
    LONG ref_ = 1;
    int cam_;
};

std::wstring ClsidText(const GUID& g) {
    wchar_t b[64];
    StringFromGUID2(g, b, 64);
    return b;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    for (int i = 0; i < ilcam::kCams; ++i) {
        if (clsid != ilcam::kCamClsid[i]) continue;
        Factory* f = new (std::nothrow) Factory(i);
        if (!f) return E_OUTOFMEMORY;
        const HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

// HKLM\SOFTWARE\Classes\CLSID\{...}: the camera's name; InprocServer32: this file, both threading models.
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    for (int i = 0; i < ilcam::kCams; ++i) {
        const std::wstring key = L"SOFTWARE\\Classes\\CLSID\\" + ClsidText(ilcam::kCamClsid[i]);
        HKEY k = nullptr, s = nullptr;
        LSTATUS r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
        if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
        RegSetValueExW(k, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(ilcam::kCamName[i]),
                       static_cast<DWORD>((wcslen(ilcam::kCamName[i]) + 1) * sizeof(wchar_t)));
        r = RegCreateKeyExW(k, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &s, nullptr);
        if (r == ERROR_SUCCESS) {
            RegSetValueExW(s, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path), static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
            const wchar_t tm[] = L"Both";
            RegSetValueExW(s, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(tm), sizeof(tm));
            RegCloseKey(s);
        }
        RegCloseKey(k);
        if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    }
    return S_OK;
}

STDAPI DllUnregisterServer() {
    for (int i = 0; i < ilcam::kCams; ++i) {
        const std::wstring key = L"SOFTWARE\\Classes\\CLSID\\" + ClsidText(ilcam::kCamClsid[i]);
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
    }
    return S_OK;
}
