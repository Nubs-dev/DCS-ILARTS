// Video for the DCS ILARTS viewer: MP4 recording and the instant-replay buffer.
//
// The viewer draws what it shows into a fixed-size canvas (default 1024 x 768) at a fixed rate (default
// 30 fps), reads it back and hands each frame to Engine::push. A worker thread then
//   * JPEG-encodes it into the replay ring (the last ringSeconds; a few MB per second of video), and
//   * converts it to NV12 once and writes it to every recording that is open - several can run at the same
//     time (closely spaced approaches each get their own file) - as H.264 MP4 (Media Foundation sink writer,
//     the GPU's hardware encoder when there is one).
// A recording can start in the past (pre-roll): it begins with the ring's frames from that time on. Replays
// and saved clips are built from the ring. Nothing here touches Direct3D; all calls except push/poll come
// from the viewer's UI thread.
#pragma once

#include <windows.h>

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace video {

template <typename T>
void Release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

inline double Now() {
    static const double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * inv;
}

// BGRA (top-down, any pitch) -> NV12, BT.709 limited range. w and h even.
inline void BgraToNv12(const uint8_t* src, size_t pitch, int w, int h, uint8_t* dst) {
    uint8_t* yp = dst;
    uint8_t* uv = dst + static_cast<size_t>(w) * h;
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * pitch;
        uint8_t* d = yp + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x, s += 4) d[x] = static_cast<uint8_t>(((47 * s[2] + 157 * s[1] + 16 * s[0] + 128) >> 8) + 16);
    }
    for (int y = 0; y < h; y += 2) {
        const uint8_t* s0 = src + static_cast<size_t>(y) * pitch;
        const uint8_t* s1 = s0 + pitch;
        uint8_t* d = uv + static_cast<size_t>(y / 2) * w;
        for (int x = 0; x < w; x += 2, s0 += 8, s1 += 8) {
            const int b = s0[0] + s0[4] + s1[0] + s1[4], g = s0[1] + s0[5] + s1[1] + s1[5], r = s0[2] + s0[6] + s1[2] + s1[6];
            d[x] = static_cast<uint8_t>((-26 * r - 87 * g + 112 * b + 128 * 1024 + 512) >> 10);
            d[x + 1] = static_cast<uint8_t>((112 * r - 102 * g - 10 * b + 128 * 1024 + 512) >> 10);
        }
    }
}

// JPEG through WIC. One instance per thread (COM must be initialised on it).
class Jpeg {
  public:
    ~Jpeg() { Release(f_); }
    bool init() {
        return f_ || SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f_)));
    }
    bool encode(const uint8_t* bgra, int w, int h, size_t pitch, float quality, std::vector<uint8_t>* out) {
        if (!init()) return false;
        IStream* st = nullptr;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* fr = nullptr;
        IPropertyBag2* props = nullptr;
        IWICBitmap* bmp = nullptr;
        HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &st);
        if (SUCCEEDED(hr)) hr = f_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
        if (SUCCEEDED(hr)) hr = enc->Initialize(st, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, &props);
        if (SUCCEEDED(hr)) {
            PROPBAG2 opt{};
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_R4;
            v.fltVal = quality;
            props->Write(1, &opt, &v);
            hr = fr->Initialize(props);
        }
        if (SUCCEEDED(hr)) hr = f_->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGR, static_cast<UINT>(pitch),
                                                            static_cast<UINT>(pitch * h), const_cast<BYTE*>(bgra), &bmp);
        if (SUCCEEDED(hr)) hr = fr->SetSize(w, h);
        if (SUCCEEDED(hr)) hr = fr->WriteSource(bmp, nullptr);
        if (SUCCEEDED(hr)) hr = fr->Commit();
        if (SUCCEEDED(hr)) hr = enc->Commit();
        if (SUCCEEDED(hr)) {
            HGLOBAL g = nullptr;
            STATSTG ss{};
            hr = st->Stat(&ss, STATFLAG_NONAME);
            if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(st, &g);
            if (SUCCEEDED(hr)) {
                const void* p = GlobalLock(g);
                out->assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + ss.cbSize.QuadPart);
                GlobalUnlock(g);
            }
        }
        Release(bmp);
        Release(props);
        Release(fr);
        Release(enc);
        Release(st);
        return SUCCEEDED(hr);
    }
    // To BGRA (alpha 255), tightly packed.
    bool decode(const std::vector<uint8_t>& in, std::vector<uint8_t>* bgra, int* w, int* h) {
        if (!init() || in.empty()) return false;
        IWICStream* st = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* fr = nullptr;
        IWICBitmapSource* conv = nullptr;
        UINT uw = 0, uh = 0;
        HRESULT hr = f_->CreateStream(&st);
        if (SUCCEEDED(hr)) hr = st->InitializeFromMemory(const_cast<BYTE*>(in.data()), static_cast<DWORD>(in.size()));
        if (SUCCEEDED(hr)) hr = f_->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
        if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &fr);
        if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, fr, &conv);
        if (SUCCEEDED(hr)) hr = conv->GetSize(&uw, &uh);
        if (SUCCEEDED(hr)) {
            bgra->resize(static_cast<size_t>(uw) * uh * 4);
            hr = conv->CopyPixels(nullptr, uw * 4, static_cast<UINT>(bgra->size()), bgra->data());
        }
        Release(conv);
        Release(fr);
        Release(dec);
        Release(st);
        *w = static_cast<int>(uw);
        *h = static_cast<int>(uh);
        return SUCCEEDED(hr);
    }

  private:
    IWICImagingFactory* f_ = nullptr;
};

// H.264 MP4 file (Media Foundation sink writer), NV12 frames in. MFStartup must have been called.
class Mp4 {
  public:
    ~Mp4() { abandon(); }
    bool isOpen() const { return w_ != nullptr; }
    bool hardware() const { return hw_; }
    // encoder: 0 = hardware if possible, 1 = hardware only, 2 = software only
    bool open(const std::wstring& path, int width, int height, int fps, int bitrate, int encoder, std::wstring* err) {
        abandon();
        width_ = width;
        height_ = height;
        if (encoder != 2 && tryOpen(path, true, fps, bitrate)) return hw_ = true, true;
        if (encoder != 1 && tryOpen(path, false, fps, bitrate)) return hw_ = false, true;
        if (err) *err = L"cannot create the video file (encoder or folder)";
        return false;
    }
    bool write(const uint8_t* nv12, LONGLONG time, LONGLONG duration) {
        if (!w_) return false;
        const DWORD size = static_cast<DWORD>(static_cast<size_t>(width_) * height_ * 3 / 2);
        IMFMediaBuffer* buf = nullptr;
        IMFSample* s = nullptr;
        BYTE* p = nullptr;
        HRESULT hr = MFCreateMemoryBuffer(size, &buf);
        if (SUCCEEDED(hr)) hr = buf->Lock(&p, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            memcpy(p, nv12, size);
            buf->Unlock();
            hr = buf->SetCurrentLength(size);
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
        if (SUCCEEDED(hr)) hr = s->AddBuffer(buf);
        if (SUCCEEDED(hr)) hr = s->SetSampleTime(time);
        if (SUCCEEDED(hr)) hr = s->SetSampleDuration(duration);
        if (SUCCEEDED(hr)) hr = w_->WriteSample(stream_, s);
        Release(s);
        Release(buf);
        if (SUCCEEDED(hr)) ++frames_;
        return SUCCEEDED(hr);
    }
    long frames() const { return frames_; }
    bool close() {
        if (!w_) return false;
        const HRESULT hr = frames_ ? w_->Finalize() : E_FAIL;
        Release(w_);
        return SUCCEEDED(hr);
    }

  private:
    void abandon() {
        Release(w_);
        frames_ = 0;
    }
    bool tryOpen(const std::wstring& path, bool hw, int fps, int bitrate) {
        IMFAttributes* attr = nullptr;
        IMFMediaType* out = nullptr;
        IMFMediaType* in = nullptr;
        HRESULT hr = MFCreateAttributes(&attr, 3);
        if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hw ? TRUE : FALSE);
        if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        if (SUCCEEDED(hr)) hr = attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attr, &w_);
        auto common = [&](IMFMediaType* t) {
            HRESULT r = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(r)) r = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (SUCCEEDED(r)) r = MFSetAttributeSize(t, MF_MT_FRAME_SIZE, width_, height_);
            if (SUCCEEDED(r)) r = MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fps, 1);
            if (SUCCEEDED(r)) r = MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (SUCCEEDED(r)) r = t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            if (SUCCEEDED(r)) r = t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            return r;
        };
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&out);
        if (SUCCEEDED(hr)) hr = common(out);
        if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));
        if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        if (SUCCEEDED(hr)) hr = w_->AddStream(out, &stream_);
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&in);
        if (SUCCEEDED(hr)) hr = common(in);
        if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        // Peak-constrained VBR: still pictures take a fraction of the bit rate (a night Case III recording, 74 s:
        // 6.1 MB against 32 MB at constant rate, worst frame 45 dB PSNR), and a file never exceeds about mean x
        // length. Encoders that refuse the settings get the media type alone.
        IMFAttributes* enc = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(MFCreateAttributes(&enc, 3))) {
            enc->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR);
            enc->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, static_cast<UINT32>(bitrate));
            enc->SetUINT32(CODECAPI_AVEncCommonMaxBitRate, static_cast<UINT32>(std::min<long long>(bitrate * 2LL, 0x7FFFFFFF)));
        }
        if (SUCCEEDED(hr)) {
            const HRESULT h2 = enc ? w_->SetInputMediaType(stream_, in, enc) : E_FAIL;
            hr = SUCCEEDED(h2) ? h2 : w_->SetInputMediaType(stream_, in, nullptr);
        }
        Release(enc);
        if (SUCCEEDED(hr)) hr = w_->BeginWriting();
        Release(in);
        Release(out);
        Release(attr);
        if (FAILED(hr)) {
            Release(w_);
            DeleteFileW(path.c_str());
        }
        frames_ = 0;
        return SUCCEEDED(hr);
    }
    IMFSinkWriter* w_ = nullptr;
    DWORD stream_ = 0;
    int width_ = 0, height_ = 0;
    long frames_ = 0;
    bool hw_ = false;
};

struct RingFrame {
    double t;
    std::vector<uint8_t> jpeg;
};
using FramePtr = std::shared_ptr<const RingFrame>;

struct Event {
    enum Kind { Saved, Discarded, Failed } kind;
    std::wstring path, text;
    int key = 0;  // the recording's key (-1: a saved clip)
};

class Engine {
  public:
    struct Config {
        int w = 1024, h = 768, fps = 30;
        double ringSeconds = 45;
        float quality = 0.85f;
        int bitrate = 0;  // bits/s average (peak 2x), 0 = 2.2 Mbit/s (a 60 s pass stays under 20 MB)
        int encoder = 0;  // Mp4::open
    };

    ~Engine() { stop(); }

    bool start(const Config& c) {
        if (running_) return true;
        cfg_ = c;
        cfg_.w &= ~1;
        cfg_.h &= ~1;
        if (cfg_.bitrate <= 0) cfg_.bitrate = 2200000;
        quit_ = false;
        running_ = true;
        worker_ = std::thread([this] { workerLoop(); });
        clips_ = std::thread([this] { clipLoop(); });
        return true;
    }

    void stop() {
        if (!running_) return;
        {
            std::lock_guard<std::mutex> l(qm_);
            quit_ = true;
        }
        qcv_.notify_all();
        ccv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (clips_.joinable()) clips_.join();
        running_ = false;
    }

    const Config& config() const { return cfg_; }

    // A canvas frame: cfg.w x cfg.h BGRA, tightly packed. Dropped when the worker is more than 2 s behind.
    void push(std::vector<uint8_t>&& bgra, double t) {
        if (!running_) return;
        {
            std::lock_guard<std::mutex> l(qm_);
            int frames = 0;
            for (const auto& j : q_) frames += j.kind == Job::Frame;
            if (frames > cfg_.fps * 2) {
                ++dropped_;
                return;
            }
            q_.push_back(Job{Job::Frame, std::move(bgra), t, {}, 0, 0});
        }
        qcv_.notify_one();
    }

    // Recording `key` (any number; several can be open): writes path with the ring's frames from `from`
    // (seconds, Now() clock) on, then the live ones.
    void beginRecording(int key, const std::wstring& path, double from) { enqueue(Job{Job::Begin, {}, 0, path, from, key}); }
    // Closes recording `key` and renames it to finalPath ("" = keep the name, "-" = delete it).
    void endRecording(int key, const std::wstring& finalPath) { enqueue(Job{Job::End, {}, 0, finalPath, 0, key}); }

    std::vector<FramePtr> range(double t0, double t1) const {
        std::lock_guard<std::mutex> l(rm_);
        std::vector<FramePtr> out;
        for (const auto& f : ring_)
            if (f->t >= t0 && f->t <= t1) out.push_back(f);
        return out;
    }
    bool bounds(double* oldest, double* newest) const {
        std::lock_guard<std::mutex> l(rm_);
        if (ring_.empty()) return false;
        *oldest = ring_.front()->t;
        *newest = ring_.back()->t;
        return true;
    }
    size_t ringBytes() const {
        std::lock_guard<std::mutex> l(rm_);
        return ringBytes_;
    }
    long dropped() const { return dropped_; }
    bool hardwareEncoder() const { return hw_; }

    // Writes frames (from range()) to path on the clip thread.
    void saveClip(std::vector<FramePtr> frames, const std::wstring& path) {
        {
            std::lock_guard<std::mutex> l(qm_);
            clipQ_.push_back(Clip{std::move(frames), path});
        }
        ccv_.notify_one();
    }

    bool poll(Event* e) {
        std::lock_guard<std::mutex> l(em_);
        if (events_.empty()) return false;
        *e = events_.front();
        events_.pop_front();
        return true;
    }

  private:
    struct Job {
        enum Kind { Frame, Begin, End } kind;
        std::vector<uint8_t> bgra;
        double t;
        std::wstring path;
        double from;
        int key;
    };
    struct Clip {
        std::vector<FramePtr> frames;
        std::wstring path;
    };

    void enqueue(Job&& j) {
        {
            std::lock_guard<std::mutex> l(qm_);
            q_.push_back(std::move(j));
        }
        qcv_.notify_one();
    }
    void event(Event::Kind k, const std::wstring& path, const std::wstring& text, int key = 0) {
        std::lock_guard<std::mutex> l(em_);
        events_.push_back(Event{k, path, text, key});
        while (events_.size() > 32) events_.pop_front();
    }
    LONGLONG Duration() const { return 10000000LL / cfg_.fps; }

    struct Slot {  // one open recording
        Mp4 mp4;
        std::wstring path;
        double t0 = -1, last = -1;
    };
    // Writes one NV12 frame at time t (times relative to the recording's first frame, increasing).
    void writeNv12(Slot& s, const uint8_t* nv12, double t) {
        if (s.t0 < 0) s.t0 = t;
        if (t <= s.last) return;
        s.last = t;
        s.mp4.write(nv12, static_cast<LONGLONG>((t - s.t0) * 1e7 + 0.5), Duration());
    }
    void toNv12(const uint8_t* bgra, std::vector<uint8_t>& nv12) const {
        BgraToNv12(bgra, static_cast<size_t>(cfg_.w) * 4, cfg_.w, cfg_.h, nv12.data());
    }

    void workerLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            Jpeg jpeg;
            std::map<int, std::unique_ptr<Slot>> open;
            std::vector<uint8_t> nv12(static_cast<size_t>(cfg_.w) * cfg_.h * 3 / 2), decoded;
            for (;;) {
                Job j;
                {
                    std::unique_lock<std::mutex> l(qm_);
                    qcv_.wait(l, [&] { return quit_ || !q_.empty(); });
                    if (q_.empty()) break;  // quit, queue drained
                    j = std::move(q_.front());
                    q_.pop_front();
                }
                if (j.kind == Job::Frame) {
                    if (j.bgra.size() != static_cast<size_t>(cfg_.w) * cfg_.h * 4) continue;
                    auto f = std::make_shared<RingFrame>();
                    f->t = j.t;
                    if (jpeg.encode(j.bgra.data(), cfg_.w, cfg_.h, static_cast<size_t>(cfg_.w) * 4, cfg_.quality, &f->jpeg)) {
                        std::lock_guard<std::mutex> l(rm_);
                        ringBytes_ += f->jpeg.size();
                        ring_.push_back(std::move(f));
                        while (!ring_.empty() && ring_.back()->t - ring_.front()->t > cfg_.ringSeconds) {
                            ringBytes_ -= ring_.front()->jpeg.size();
                            ring_.pop_front();
                        }
                    }
                    if (!open.empty()) {
                        toNv12(j.bgra.data(), nv12);
                        for (auto& kv : open) writeNv12(*kv.second, nv12.data(), j.t);
                    }
                } else if (j.kind == Job::Begin) {
                    auto it = open.find(j.key);
                    if (it != open.end()) {
                        finish(it->second->mp4, it->second->path, L"", j.key);
                        open.erase(it);
                    }
                    auto slot = std::make_unique<Slot>();
                    std::wstring err;
                    if (!slot->mp4.open(j.path, cfg_.w, cfg_.h, cfg_.fps, cfg_.bitrate, cfg_.encoder, &err)) {
                        event(Event::Failed, j.path, err, j.key);
                        continue;
                    }
                    hw_ = slot->mp4.hardware();
                    slot->path = j.path;
                    for (const auto& f : range(j.from, 1e300)) {  // pre-roll from the ring
                        int w = 0, h = 0;
                        if (jpeg.decode(f->jpeg, &decoded, &w, &h) && w == cfg_.w && h == cfg_.h) {
                            toNv12(decoded.data(), nv12);
                            writeNv12(*slot, nv12.data(), f->t);
                        }
                    }
                    open[j.key] = std::move(slot);
                } else if (j.kind == Job::End) {
                    auto it = open.find(j.key);
                    if (it != open.end()) {
                        finish(it->second->mp4, it->second->path, j.path, j.key);
                        open.erase(it);
                    }
                }
            }
            for (auto& kv : open) finish(kv.second->mp4, kv.second->path, L"", kv.first);
        }
        CoUninitialize();
    }

    void finish(Mp4& rec, const std::wstring& path, const std::wstring& finalPath, int key) {
        const long frames = rec.frames();
        const bool ok = rec.close();
        if (finalPath == L"-" || !ok || frames == 0) {
            DeleteFileW(path.c_str());
            event(ok || frames == 0 ? Event::Discarded : Event::Failed, path, ok || frames == 0 ? L"" : L"could not finish the file", key);
            return;
        }
        std::wstring dst = finalPath.empty() ? path : finalPath;
        if (dst != path && !MoveFileExW(path.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) dst = path;
        event(Event::Saved, dst, std::to_wstring(frames) + L" frames", key);
    }

    void clipLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            Jpeg jpeg;
            std::vector<uint8_t> nv12(static_cast<size_t>(cfg_.w) * cfg_.h * 3 / 2), decoded;
            for (;;) {
                Clip c;
                {
                    std::unique_lock<std::mutex> l(qm_);
                    ccv_.wait(l, [&] { return quit_ || !clipQ_.empty(); });
                    if (clipQ_.empty()) break;
                    c = std::move(clipQ_.front());
                    clipQ_.pop_front();
                }
                Slot slot;
                std::wstring err;
                if (!slot.mp4.open(c.path, cfg_.w, cfg_.h, cfg_.fps, cfg_.bitrate, cfg_.encoder, &err)) {
                    event(Event::Failed, c.path, err);
                    continue;
                }
                for (const auto& f : c.frames) {
                    int w = 0, h = 0;
                    if (jpeg.decode(f->jpeg, &decoded, &w, &h) && w == cfg_.w && h == cfg_.h) {
                        toNv12(decoded.data(), nv12);
                        writeNv12(slot, nv12.data(), f->t);
                    }
                }
                finish(slot.mp4, c.path, L"", -1);
            }
        }
        CoUninitialize();
    }

    Config cfg_;
    std::thread worker_, clips_;
    bool running_ = false;
    std::atomic<bool> hw_{false};
    std::atomic<long> dropped_{0};
    mutable std::mutex qm_, rm_, em_;
    std::condition_variable qcv_, ccv_;
    bool quit_ = false;
    std::deque<Job> q_;
    std::deque<Clip> clipQ_;
    std::deque<FramePtr> ring_;
    size_t ringBytes_ = 0;
    std::deque<Event> events_;
};

}  // namespace video
