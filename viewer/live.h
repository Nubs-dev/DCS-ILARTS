// Live H.264 for the DCS ILARTS viewer: the black box and the RTMP stream.
//
// One encoder (a Media Foundation H.264 encoder transform: the GPU's when there is one, else Microsoft's
// software encoder) turns the recording canvas into a continuous H.264 stream of access units. They go to
//   * the black box: a ring in memory holding the last few minutes, written to an MP4 without re-encoding
//     when something worth keeping happened (a mishap reported by DCS, or on request), and
//   * the RTMP publisher (rtmp.h).
// The encoder only runs while one of the two is switched on. Recordings of passes and manual recordings do
// not go through here (video.h: one sink writer each, tuned for file size).
#pragma once

#include <windows.h>

#include <codecapi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "video.h"

namespace live {

using video::Release;

// One H.264 access unit, Annex B (start codes).
struct Unit {
    double t = 0;      // capture time (video::Now clock)
    bool key = false;  // IDR: decoding can start here
    std::vector<uint8_t> data;
};
using UnitPtr = std::shared_ptr<const Unit>;

// NAL units of an Annex B buffer: calls fn(type, payload begin, payload end) for each.
inline void ForEachNal(const uint8_t* p, size_t n, const std::function<void(int, const uint8_t*, const uint8_t*)>& fn) {
    size_t i = 0, start = SIZE_MAX;
    auto emit = [&](size_t end) {
        if (start != SIZE_MAX && end > start) fn(p[start] & 0x1F, p + start, p + end);
    };
    while (i + 3 <= n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            size_t end = i;
            while (end > 0 && start != SIZE_MAX && end > start && p[end - 1] == 0) --end;  // trailing zeros belong to the start code
            emit(end);
            start = i + 3;
            i += 3;
        } else {
            ++i;
        }
    }
    emit(n);
}

// SPS and PPS (without start codes) of a key unit; false if it has none.
inline bool ParameterSets(const std::vector<uint8_t>& annexB, std::vector<uint8_t>* sps, std::vector<uint8_t>* pps) {
    sps->clear();
    pps->clear();
    ForEachNal(annexB.data(), annexB.size(), [&](int type, const uint8_t* b, const uint8_t* e) {
        if (type == 7 && sps->empty()) sps->assign(b, e);
        if (type == 8 && pps->empty()) pps->assign(b, e);
    });
    return !sps->empty() && !pps->empty();
}

// H.264 encoder transform, NV12 in, Annex B access units out. One thread uses it (COM initialised, MFStartup done).
class H264Encoder {
  public:
    ~H264Encoder() { close(); }
    bool isOpen() const { return mft_ != nullptr; }
    bool hardware() const { return hw_; }
    const std::wstring& name() const { return name_; }
    // Why the last open failed: the step and its HRESULT.
    const char* failStep() const { return step_; }
    HRESULT failCode() const { return fail_; }

    // encoder: 0 = hardware if possible, 1 = hardware only, 2 = software only. cbr: constant bit rate (streams).
    bool open(int w, int h, int fps, int bitrate, int encoder, double gopSeconds, bool cbr) {
        close();
        w_ = w, h_ = h, fps_ = fps, bitrate_ = bitrate, gop_ = static_cast<int>(fps * gopSeconds + 0.5), cbr_ = cbr;
        if (encoder != 2 && tryOpen(true)) return true;
        if (encoder != 1 && tryOpen(false)) return true;
        return false;
    }

    void close() {
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        Release(events_);
        Release(codec_);
        if (mft_ && hw_) MFShutdownObject(mft_);
        Release(mft_);
        times_.clear();
        needInput_ = 0;
    }

    void forceKeyFrame() {
        if (!codec_) return;
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 1;
        codec_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
    }

    // One NV12 frame captured at time t. Finished access units are appended to out. False = the encoder failed.
    bool encode(const uint8_t* nv12, double t, std::vector<UnitPtr>* out) {
        if (!mft_) return false;
        if (t0_ < 0) t0_ = t;
        const LONGLONG ts = static_cast<LONGLONG>((t - t0_) * 1e7 + 0.5);
        if (ts <= lastTs_) return true;  // time must increase
        lastTs_ = ts;
        const DWORD size = static_cast<DWORD>(static_cast<size_t>(w_) * h_ * 3 / 2);
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
        if (SUCCEEDED(hr)) hr = s->SetSampleTime(ts);
        if (SUCCEEDED(hr)) hr = s->SetSampleDuration(10000000LL / fps_);
        if (SUCCEEDED(hr)) {
            times_.push_back({ts, t});
            while (times_.size() > 64) times_.pop_front();
            hr = async_ ? feedAsync(s, out) : feedSync(s, out);
        }
        Release(s);
        Release(buf);
        return SUCCEEDED(hr);
    }

  private:
    bool tryOpen(bool hw) {
        MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        const UINT32 flags = (hw ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, &acts, &count))) return false;
        bool ok = false;
        for (UINT32 i = 0; i < count; ++i) {
            if (!ok && SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft_)))) {
                hw_ = hw;
                WCHAR* nm = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &len)) && nm) name_ = nm;
                CoTaskMemFree(nm);
                ok = configure();
                if (!ok) {
                    Release(events_);
                    Release(codec_);
                    if (hw) acts[i]->ShutdownObject();
                    Release(mft_);
                }
            }
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
        return ok;
    }

    void codecValue(const GUID& api, UINT32 value) {
        if (!codec_) return;
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = value;
        codec_->SetValue(&api, &v);
    }

    bool configure() {
        async_ = false;
        IMFAttributes* attr = nullptr;
        if (SUCCEEDED(mft_->GetAttributes(&attr)) && attr) {
            UINT32 a = 0;
            if (SUCCEEDED(attr->GetUINT32(MF_TRANSFORM_ASYNC, &a)) && a) {
                async_ = true;
                attr->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
            attr->SetUINT32(MF_LOW_LATENCY, TRUE);
            attr->Release();
        }
        if (async_ && FAILED(mft_->QueryInterface(IID_PPV_ARGS(&events_)))) return false;
        mft_->QueryInterface(IID_PPV_ARGS(&codec_));
        codecValue(CODECAPI_AVEncCommonRateControlMode, cbr_ ? eAVEncCommonRateControlMode_CBR : eAVEncCommonRateControlMode_PeakConstrainedVBR);
        codecValue(CODECAPI_AVEncCommonMeanBitRate, static_cast<UINT32>(bitrate_));
        codecValue(CODECAPI_AVEncCommonMaxBitRate, static_cast<UINT32>(cbr_ ? bitrate_ : bitrate_ * 2));
        codecValue(CODECAPI_AVEncMPVGOPSize, static_cast<UINT32>(gop_));
        codecValue(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        codecValue(CODECAPI_AVLowLatencyMode, 1);
        auto common = [&](IMFMediaType* t) {
            HRESULT r = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (SUCCEEDED(r)) r = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (SUCCEEDED(r)) r = MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w_, h_);
            if (SUCCEEDED(r)) r = MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fps_, 1);
            if (SUCCEEDED(r)) r = MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            return r;
        };
        IMFMediaType* out = nullptr;
        IMFMediaType* in = nullptr;
        HRESULT hr = S_OK;
        auto step = [&](const char* what, HRESULT r) {
            if (SUCCEEDED(hr) && FAILED(r)) step_ = what, fail_ = r;
            if (SUCCEEDED(hr)) hr = r;
        };
        step("output type", MFCreateMediaType(&out));
        if (SUCCEEDED(hr)) step("output type", common(out));
        if (SUCCEEDED(hr)) step("output type", out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
        if (SUCCEEDED(hr)) step("output type", out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate_)));
        if (SUCCEEDED(hr)) step("output type", out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main));
        if (SUCCEEDED(hr)) step("SetOutputType", mft_->SetOutputType(0, out, 0));
        if (SUCCEEDED(hr)) step("input type", MFCreateMediaType(&in));
        if (SUCCEEDED(hr)) step("input type", common(in));
        if (SUCCEEDED(hr)) step("input type", in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
        if (SUCCEEDED(hr)) step("SetInputType", mft_->SetInputType(0, in, 0));
        Release(in);
        Release(out);
        if (SUCCEEDED(hr)) {
            MFT_OUTPUT_STREAM_INFO si{};
            step("GetOutputStreamInfo", mft_->GetOutputStreamInfo(0, &si));
            provides_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            outSize_ = si.cbSize ? si.cbSize : static_cast<DWORD>(w_ * h_ * 2);
        }
        // (no flush here: Microsoft's software encoder refuses one before it has streamed)
        if (SUCCEEDED(hr)) step("begin streaming", mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        if (SUCCEEDED(hr)) step("start of stream", mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
        t0_ = -1;
        lastTs_ = -1;
        needInput_ = 0;
        return SUCCEEDED(hr);
    }

    // One ProcessOutput. S_OK = a unit was appended (or the stream format changed and was accepted).
    HRESULT pull(std::vector<UnitPtr>* out) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        IMFSample* mine = nullptr;
        if (!provides_) {
            IMFMediaBuffer* b = nullptr;
            HRESULT hr = MFCreateSample(&mine);
            if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(outSize_, &b);
            if (SUCCEEDED(hr)) hr = mine->AddBuffer(b);
            Release(b);
            if (FAILED(hr)) {
                Release(mine);
                return hr;
            }
            ob.pSample = mine;
        }
        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            IMFMediaType* t = nullptr;
            if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &t))) {
                mft_->SetOutputType(0, t, 0);
                t->Release();
            }
            hr = S_OK;
        } else if (SUCCEEDED(hr) && ob.pSample) {
            auto u = std::make_shared<Unit>();
            LONGLONG ts = 0;
            ob.pSample->GetSampleTime(&ts);
            u->t = t0_ + ts / 1e7;
            for (const auto& tt : times_)
                if (tt.first == ts) u->t = tt.second;
            UINT32 clean = 0;
            ob.pSample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
            IMFMediaBuffer* b = nullptr;
            if (SUCCEEDED(ob.pSample->ConvertToContiguousBuffer(&b))) {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                    u->data.assign(p, p + len);
                    b->Unlock();
                }
                b->Release();
            }
            bool idr = false;
            ForEachNal(u->data.data(), u->data.size(), [&](int type, const uint8_t*, const uint8_t*) { idr = idr || type == 5; });
            u->key = idr || clean != 0;
            if (!u->data.empty()) out->push_back(std::move(u));
        }
        if (ob.pSample && ob.pSample != mine) ob.pSample->Release();
        Release(mine);
        if (ob.pEvents) ob.pEvents->Release();
        return hr;
    }

    HRESULT feedSync(IMFSample* s, std::vector<UnitPtr>* out) {
        HRESULT hr = mft_->ProcessInput(0, s, 0);
        if (hr == MF_E_NOTACCEPTING) {
            while (pull(out) == S_OK) {}
            hr = mft_->ProcessInput(0, s, 0);
        }
        if (FAILED(hr)) return hr;
        for (;;) {
            hr = pull(out);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
            if (FAILED(hr)) return hr;
        }
    }

    // Asynchronous transforms (hardware encoders) say when they want input and when output is ready.
    HRESULT feedAsync(IMFSample* s, std::vector<UnitPtr>* out) {
        bool fed = false;
        const ULONGLONG deadline = GetTickCount64() + 2000;
        for (;;) {
            if (!fed && needInput_ > 0) {
                const HRESULT hr = mft_->ProcessInput(0, s, 0);
                if (FAILED(hr)) return hr;
                --needInput_;
                fed = true;
            }
            IMFMediaEvent* ev = nullptr;
            HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                if (fed) return S_OK;
                if (GetTickCount64() > deadline) return E_FAIL;  // the encoder never asked for input
                Sleep(1);
                continue;
            }
            if (FAILED(hr)) return hr;
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            ev->Release();
            if (type == METransformNeedInput) ++needInput_;
            else if (type == METransformHaveOutput) {
                hr = pull(out);
                if (FAILED(hr)) return hr;
            }
        }
    }

    IMFTransform* mft_ = nullptr;
    IMFMediaEventGenerator* events_ = nullptr;
    ICodecAPI* codec_ = nullptr;
    bool hw_ = false, async_ = false, provides_ = false, cbr_ = false;
    DWORD outSize_ = 0;
    int w_ = 0, h_ = 0, fps_ = 30, bitrate_ = 0, gop_ = 30, needInput_ = 0;
    double t0_ = -1;
    LONGLONG lastTs_ = -1;
    std::deque<std::pair<LONGLONG, double>> times_;
    std::wstring name_;
    const char* step_ = "no encoder found";
    HRESULT fail_ = S_OK;
};

// Writes access units (the first one a key unit) to an H.264 MP4 without re-encoding.
inline bool WriteMp4(const std::wstring& path, const std::vector<UnitPtr>& units, int w, int h, int fps, int bitrate) {
    if (units.empty()) return false;
    std::vector<uint8_t> sps, pps;
    for (const auto& u : units)
        if (u->key && ParameterSets(u->data, &sps, &pps)) break;
    if (sps.empty() || pps.empty()) return false;
    std::vector<uint8_t> header;
    for (const auto* ps : {&sps, &pps}) {
        header.insert(header.end(), {0, 0, 0, 1});
        header.insert(header.end(), ps->begin(), ps->end());
    }
    IMFAttributes* attr = nullptr;
    IMFSinkWriter* wr = nullptr;
    IMFMediaType* t = nullptr;
    DWORD stream = 0;
    HRESULT hr = MFCreateAttributes(&attr, 2);
    if (SUCCEEDED(hr)) hr = attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attr, &wr);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));
    if (SUCCEEDED(hr)) hr = t->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), static_cast<UINT32>(header.size()));
    if (SUCCEEDED(hr)) hr = wr->AddStream(t, &stream);
    if (SUCCEEDED(hr)) hr = wr->SetInputMediaType(stream, t, nullptr);
    if (SUCCEEDED(hr)) hr = wr->BeginWriting();
    const double t0 = units.front()->t;
    LONGLONG last = -1;
    for (size_t i = 0; SUCCEEDED(hr) && i < units.size(); ++i) {
        const Unit& u = *units[i];
        LONGLONG ts = static_cast<LONGLONG>((u.t - t0) * 1e7 + 0.5);
        if (ts <= last) ts = last + 1;
        last = ts;
        const LONGLONG dur = i + 1 < units.size() ? std::max<LONGLONG>(1, static_cast<LONGLONG>((units[i + 1]->t - u.t) * 1e7)) : 10000000LL / fps;
        IMFMediaBuffer* b = nullptr;
        IMFSample* s = nullptr;
        BYTE* p = nullptr;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(u.data.size()), &b);
        if (SUCCEEDED(hr)) hr = b->Lock(&p, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            memcpy(p, u.data.data(), u.data.size());
            b->Unlock();
            hr = b->SetCurrentLength(static_cast<DWORD>(u.data.size()));
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
        if (SUCCEEDED(hr)) hr = s->AddBuffer(b);
        if (SUCCEEDED(hr)) hr = s->SetSampleTime(ts);
        if (SUCCEEDED(hr)) hr = s->SetSampleDuration(dur);
        if (SUCCEEDED(hr) && u.key) hr = s->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        if (SUCCEEDED(hr)) hr = wr->WriteSample(stream, s);
        Release(s);
        Release(b);
    }
    if (SUCCEEDED(hr)) hr = wr->Finalize();
    Release(t);
    Release(wr);
    Release(attr);
    if (FAILED(hr)) DeleteFileW(path.c_str());
    return SUCCEEDED(hr);
}

struct Event {
    enum Kind { Saved, Failed, EncoderLost } kind;
    std::wstring path, text;
};

// The encoder thread, the black box ring and the hand-off to a stream.
class Live {
  public:
    struct Config {
        int w = 1024, h = 768, fps = 30;
        int bitrate = 2000000;
        int encoder = 0;      // H264Encoder::open
        bool cbr = false;
    };
    ~Live() { stop(); }

    void configure(const Config& c) {
        cfg_ = c;
        cfg_.w &= ~1;
        cfg_.h &= ~1;
    }
    const Config& config() const { return cfg_; }

    // The black box keeps the last `seconds` (0 = off). `sink` receives every unit (the stream; null = none).
    void set(double ringSeconds, std::function<void(const UnitPtr&)> sink) {
        {
            std::lock_guard<std::mutex> l(m_);
            ringSeconds_ = ringSeconds;
            sink_ = std::move(sink);
            want_ = ringSeconds > 0 || sink_;
            if (ringSeconds <= 0) {
                ring_.clear();
                ringBytes_ = 0;
            }
        }
        if (want_ && !running_) start();
        if (!want_ && running_) stop();
    }
    bool running() const { return running_; }
    bool wanted() const { return want_; }
    bool hardware() const { return hw_; }
    bool encoderOk() const { return encOk_; }

    // A canvas frame (cfg.w x cfg.h BGRA, tightly packed); copied. Dropped when the encoder is a second behind.
    void push(const uint8_t* bgra, double t) {
        if (!running_) return;
        {
            std::lock_guard<std::mutex> l(m_);
            if (q_.size() > static_cast<size_t>(cfg_.fps)) {
                ++dropped_;
                return;
            }
            Frame f;
            f.t = t;
            f.bgra.assign(bgra, bgra + static_cast<size_t>(cfg_.w) * cfg_.h * 4);
            q_.push_back(std::move(f));
        }
        cv_.notify_one();
    }

    // What the ring holds: seconds and bytes.
    void ringState(double* seconds, size_t* bytes) const {
        std::lock_guard<std::mutex> l(m_);
        *seconds = ring_.empty() ? 0 : ring_.back()->t - ring_.front()->t;
        *bytes = ringBytes_;
    }

    // Writes the ring's units between t0 and t1 (from the last key unit at or before t0) to path, on its own thread.
    void save(double t0, double t1, const std::wstring& path) {
        std::vector<UnitPtr> units;
        {
            std::lock_guard<std::mutex> l(m_);
            size_t first = SIZE_MAX;
            for (size_t i = 0; i < ring_.size(); ++i) {
                if (ring_[i]->t > t1) break;
                if (ring_[i]->key && (ring_[i]->t <= t0 || first == SIZE_MAX)) first = i;
            }
            if (first != SIZE_MAX)
                for (size_t i = first; i < ring_.size() && ring_[i]->t <= t1; ++i) units.push_back(ring_[i]);
        }
        if (units.size() < 2) {
            event(Event::Failed, path, L"nothing in the black box yet");
            return;
        }
        const Config c = cfg_;
        std::lock_guard<std::mutex> l(sm_);
        savers_.emplace_back([this, units = std::move(units), path, c] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool ok = WriteMp4(path, units, c.w, c.h, c.fps, c.bitrate);
            event(ok ? Event::Saved : Event::Failed, path, ok ? L"" : L"could not write the file");
            CoUninitialize();
        });
    }

    bool poll(Event* e) {
        std::lock_guard<std::mutex> l(em_);
        if (events_.empty()) return false;
        *e = events_.front();
        events_.pop_front();
        return true;
    }

    void stop() {
        if (running_) {
            {
                std::lock_guard<std::mutex> l(m_);
                quit_ = true;
            }
            cv_.notify_all();
            if (worker_.joinable()) worker_.join();
            running_ = false;
        }
        std::lock_guard<std::mutex> l(sm_);
        for (auto& t : savers_)
            if (t.joinable()) t.join();
        savers_.clear();
    }

  private:
    struct Frame {
        double t;
        std::vector<uint8_t> bgra;
    };

    void start() {
        quit_ = false;
        running_ = true;
        encOk_ = true;
        worker_ = std::thread([this] { loop(); });
    }
    void event(Event::Kind k, const std::wstring& path, const std::wstring& text) {
        std::lock_guard<std::mutex> l(em_);
        events_.push_back(Event{k, path, text});
        while (events_.size() > 16) events_.pop_front();
    }

    void loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            H264Encoder enc;
            std::vector<uint8_t> nv12(static_cast<size_t>(cfg_.w) * cfg_.h * 3 / 2);
            std::vector<UnitPtr> units;
            ULONGLONG retryAt = 0;
            int failures = 0;
            for (;;) {
                Frame f;
                {
                    std::unique_lock<std::mutex> l(m_);
                    cv_.wait(l, [&] { return quit_ || !q_.empty(); });
                    if (quit_) break;
                    f = std::move(q_.front());
                    q_.pop_front();
                }
                if (!enc.isOpen()) {
                    if (GetTickCount64() < retryAt) continue;
                    // key units every second: a saved clip or a stream starts within a second of where it should
                    if (!enc.open(cfg_.w, cfg_.h, cfg_.fps, cfg_.bitrate, cfg_.encoder, 1.0, cfg_.cbr)) {
                        retryAt = GetTickCount64() + 5000;
                        wchar_t why[96];
                        swprintf_s(why, L"no H.264 encoder (%S, 0x%08lX)", enc.failStep(), static_cast<unsigned long>(enc.failCode()));
                        if (encOk_) event(Event::EncoderLost, L"", why);
                        encOk_ = false;
                        continue;
                    }
                    hw_ = enc.hardware();
                    encOk_ = true;
                }
                video::BgraToNv12(f.bgra.data(), static_cast<size_t>(cfg_.w) * 4, cfg_.w, cfg_.h, nv12.data());
                units.clear();
                if (!enc.encode(nv12.data(), f.t, &units)) {
                    enc.close();
                    retryAt = GetTickCount64() + (++failures > 3 ? 5000 : 200);
                    continue;
                }
                failures = 0;
                std::function<void(const UnitPtr&)> sink;
                {
                    std::lock_guard<std::mutex> l(m_);
                    sink = sink_;
                    if (ringSeconds_ > 0) {
                        for (const auto& u : units) {
                            ringBytes_ += u->data.size();
                            ring_.push_back(u);
                        }
                        while (ring_.size() > 2 && ring_.back()->t - ring_.front()->t > ringSeconds_) {
                            ringBytes_ -= ring_.front()->data.size();
                            ring_.pop_front();
                        }
                    }
                }
                if (sink)
                    for (const auto& u : units) sink(u);
            }
        }
        CoUninitialize();
    }

    Config cfg_;
    std::thread worker_;
    std::atomic<bool> running_{false}, want_{false}, hw_{false}, encOk_{true};
    std::atomic<long> dropped_{0};
    mutable std::mutex m_, em_, sm_;
    std::condition_variable cv_;
    bool quit_ = false;
    std::deque<Frame> q_;
    std::deque<UnitPtr> ring_;
    size_t ringBytes_ = 0;
    double ringSeconds_ = 0;
    std::function<void(const UnitPtr&)> sink_;
    std::deque<Event> events_;
    std::vector<std::thread> savers_;
};

}  // namespace live
