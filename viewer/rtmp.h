// RTMP publisher for the DCS ILARTS viewer: sends the live H.264 stream (live.h) to an RTMP server
// (MediaMTX, nginx-rtmp, Twitch, YouTube ...), with a silent AAC track because several services refuse a
// stream without audio.
//
//   url   rtmp://host[:port]/app[/instance]      key   the stream key (if empty, the last part of the url)
//
// Plain RTMP only (no rtmps / TLS). Own thread: connects, publishes, sends what send() queues, answers the
// server's control messages, and reconnects after an error. If the network is slower than the stream, queued
// video is dropped up to the next key frame rather than falling behind.
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "live.h"

namespace rtmp {

using Bytes = std::vector<uint8_t>;

// ---- AMF0
inline void PutU16(Bytes& b, uint32_t v) { b.push_back(static_cast<uint8_t>(v >> 8)), b.push_back(static_cast<uint8_t>(v)); }
inline void PutU24(Bytes& b, uint32_t v) { b.push_back(static_cast<uint8_t>(v >> 16)), PutU16(b, v & 0xFFFF); }
inline void PutU32(Bytes& b, uint32_t v) { PutU16(b, v >> 16), PutU16(b, v & 0xFFFF); }
inline void AmfNumber(Bytes& b, double v) {
    b.push_back(0x00);
    uint64_t u;
    memcpy(&u, &v, 8);
    for (int i = 7; i >= 0; --i) b.push_back(static_cast<uint8_t>(u >> (i * 8)));
}
inline void AmfBool(Bytes& b, bool v) { b.push_back(0x01), b.push_back(v ? 1 : 0); }
inline void AmfKey(Bytes& b, const std::string& s) {
    PutU16(b, static_cast<uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
inline void AmfString(Bytes& b, const std::string& s) { b.push_back(0x02), AmfKey(b, s); }
inline void AmfNull(Bytes& b) { b.push_back(0x05); }
inline void AmfEnd(Bytes& b) { b.push_back(0), b.push_back(0), b.push_back(9); }

// One raw AAC-LC frame of silence (44.1 kHz stereo) from Windows' AAC encoder; a known-good frame if that fails.
inline Bytes SilentAacFrame() {
    Bytes fallback = {0x21, 0x10, 0x04, 0x60, 0x8C, 0x1C};
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Audio, MFAudioFormat_AAC};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &outInfo, &acts, &count)) || !count)
        return fallback;
    IMFTransform* mft = nullptr;
    acts[0]->ActivateObject(IID_PPV_ARGS(&mft));
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (!mft) return fallback;
    Bytes result;
    IMFMediaType* in = nullptr;
    IMFMediaType* out = nullptr;
    HRESULT hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr)) {
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 12000);
        out->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);  // raw frames
        out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        hr = mft->SetInputType(0, in, 0);
        if (SUCCEEDED(hr)) hr = mft->SetOutputType(0, out, 0);
    }
    if (SUCCEEDED(hr)) {
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        MFT_OUTPUT_STREAM_INFO si{};
        mft->GetOutputStreamInfo(0, &si);
        const DWORD pcm = 1024 * 4;
        int frames = 0;
        for (int i = 0; i < 24 && SUCCEEDED(hr) && frames < 8; ++i) {
            IMFMediaBuffer* b = nullptr;
            IMFSample* s = nullptr;
            BYTE* p = nullptr;
            MFCreateMemoryBuffer(pcm, &b);
            b->Lock(&p, nullptr, nullptr);
            memset(p, 0, pcm);
            b->Unlock();
            b->SetCurrentLength(pcm);
            MFCreateSample(&s);
            s->AddBuffer(b);
            s->SetSampleTime(static_cast<LONGLONG>(i) * 10000000LL * 1024 / 44100);
            s->SetSampleDuration(10000000LL * 1024 / 44100);
            hr = mft->ProcessInput(0, s, 0);
            s->Release();
            b->Release();
            for (;;) {
                MFT_OUTPUT_DATA_BUFFER ob{};
                IMFSample* os = nullptr;
                IMFMediaBuffer* obuf = nullptr;
                MFCreateSample(&os);
                MFCreateMemoryBuffer(si.cbSize ? si.cbSize : 4096, &obuf);
                os->AddBuffer(obuf);
                ob.pSample = os;
                DWORD status = 0;
                const HRESULT h2 = mft->ProcessOutput(0, 1, &ob, &status);
                if (SUCCEEDED(h2)) {
                    BYTE* q = nullptr;
                    DWORD len = 0;
                    if (SUCCEEDED(obuf->Lock(&q, nullptr, &len))) {
                        if (++frames >= 5 && len > 0 && len < 64) result.assign(q, q + len);  // settled, and really silence
                        obuf->Unlock();
                    }
                }
                obuf->Release();
                os->Release();
                if (ob.pEvents) ob.pEvents->Release();
                if (FAILED(h2)) break;
            }
        }
    }
    if (in) in->Release();
    if (out) out->Release();
    mft->Release();
    return result.empty() ? fallback : result;
}

class Publisher {
  public:
    struct Config {
        std::string url, key;
        int w = 1024, h = 768, fps = 30, bitrate = 2000000;
        bool audio = true;
    };
    enum State { Off, Connecting, Live, Retrying };

    ~Publisher() { stop(); }

    void start(const Config& c) {
        stop();
        cfg_ = c;
        quit_ = false;
        state_ = Connecting;
        setText("connecting");
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        if (thread_.joinable()) {
            {
                std::lock_guard<std::mutex> l(m_);
                quit_ = true;
            }
            cv_.notify_all();
            closeSocket();
            thread_.join();
        }
        state_ = Off;
        std::lock_guard<std::mutex> l(m_);
        q_.clear();
    }

    // A unit of the live stream (any thread).
    void send(const live::UnitPtr& u) {
        if (state_ == Off) return;
        {
            std::lock_guard<std::mutex> l(m_);
            if (!q_.empty() && u->t - q_.front()->t > 3.0) {  // the network is behind: drop to the next key frame
                dropped_ += static_cast<long>(q_.size());
                q_.clear();
                needKey_ = true;
            }
            q_.push_back(u);
        }
        cv_.notify_one();
    }

    State state() const { return state_; }
    std::string text() const {
        std::lock_guard<std::mutex> l(tm_);
        return text_;
    }
    double liveSeconds() const { return state_ == Live ? video::Now() - liveSince_ : 0; }
    long long bytesSent() const { return sent_; }
    long dropped() const { return dropped_; }

  private:
    // ---- url
    bool parseUrl() {
        std::string u = cfg_.url;
        if (u.rfind("rtmps://", 0) == 0) return fail("rtmps is not supported (use rtmp://)");
        if (u.rfind("rtmp://", 0) != 0) return fail("the address must start with rtmp://");
        u = u.substr(7);
        const size_t slash = u.find('/');
        std::string hostport = slash == std::string::npos ? u : u.substr(0, slash);
        std::string path = slash == std::string::npos ? "" : u.substr(slash + 1);
        while (!path.empty() && path.back() == '/') path.pop_back();
        const size_t colon = hostport.rfind(':');
        host_ = colon == std::string::npos ? hostport : hostport.substr(0, colon);
        port_ = colon == std::string::npos ? "1935" : hostport.substr(colon + 1);
        playpath_ = cfg_.key;
        if (playpath_.empty()) {
            const size_t last = path.rfind('/');
            if (last == std::string::npos) return fail("no stream key (give one, or end the address with it)");
            playpath_ = path.substr(last + 1);
            path = path.substr(0, last);
        }
        app_ = path;
        tcUrl_ = "rtmp://" + hostport + "/" + app_;
        if (host_.empty() || app_.empty() || playpath_.empty()) return fail("the address needs a host and an application (rtmp://host/app)");
        return true;
    }

    // ---- socket
    void closeSocket() {
        const SOCKET s = sock_.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) closesocket(s);
    }
    bool sendAll(const uint8_t* p, size_t n) {
        const SOCKET s = sock_;
        while (n > 0) {
            const int r = ::send(s, reinterpret_cast<const char*>(p), static_cast<int>(std::min<size_t>(n, 1 << 16)), 0);
            if (r <= 0) return fail("connection lost while sending");
            p += r;
            n -= static_cast<size_t>(r);
            sent_ += r;
        }
        return true;
    }
    // Reads what is there (waits up to waitMs for the first byte) into in_. False = closed / error.
    bool readSome(int waitMs) {
        const SOCKET s = sock_;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{waitMs / 1000, (waitMs % 1000) * 1000};
        const int sel = select(0, &rd, nullptr, nullptr, &tv);
        if (sel < 0) return fail("connection lost");
        if (sel == 0) return true;
        uint8_t buf[4096];
        const int r = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (r <= 0) return fail("the server closed the connection");
        in_.insert(in_.end(), buf, buf + r);
        received_ += static_cast<uint32_t>(r);
        return true;
    }
    bool readExact(uint8_t* dst, size_t n, int timeoutMs) {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (in_.size() < n) {
            if (GetTickCount64() > deadline) return fail("the server did not answer");
            if (!readSome(200)) return false;
            if (stopping()) return false;
        }
        memcpy(dst, in_.data(), n);
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(n));
        return true;
    }

    bool connectSocket() {
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host_.c_str(), port_.c_str(), &hints, &res) != 0 || !res) return fail("host not found: " + host_);
        SOCKET s = INVALID_SOCKET;
        for (addrinfo* a = res; a && s == INVALID_SOCKET; a = a->ai_next) {
            s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == INVALID_SOCKET) continue;
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen));
            fd_set wr;
            FD_ZERO(&wr);
            FD_SET(s, &wr);
            timeval tv{5, 0};
            if (select(0, nullptr, &wr, nullptr, &tv) <= 0) {
                closesocket(s);
                s = INVALID_SOCKET;
                continue;
            }
            nb = 0;
            ioctlsocket(s, FIONBIO, &nb);
        }
        freeaddrinfo(res);
        if (s == INVALID_SOCKET) return fail("cannot connect to " + host_ + ":" + port_);
        const DWORD to = 5000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
        const int nodelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        sock_ = s;
        return true;
    }

    // ---- chunk stream
    bool message(int csid, uint8_t type, uint32_t streamId, uint32_t ts, const uint8_t* data, size_t n) {
        Bytes out;
        out.reserve(n + n / outChunk_ * 5 + 20);
        const bool ext = ts >= 0xFFFFFF;
        out.push_back(static_cast<uint8_t>(csid));
        PutU24(out, ext ? 0xFFFFFF : ts);
        PutU24(out, static_cast<uint32_t>(n));
        out.push_back(type);
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(streamId >> (i * 8)));  // little endian
        if (ext) PutU32(out, ts);
        size_t off = 0;
        while (off < n) {
            const size_t k = std::min<size_t>(outChunk_, n - off);
            if (off) {
                out.push_back(static_cast<uint8_t>(0xC0 | csid));
                if (ext) PutU32(out, ts);
            }
            out.insert(out.end(), data + off, data + off + k);
            off += k;
        }
        return sendAll(out.data(), out.size());
    }
    bool message(int csid, uint8_t type, uint32_t streamId, uint32_t ts, const Bytes& b) { return message(csid, type, streamId, ts, b.data(), b.size()); }

    struct InStream {
        uint32_t ts = 0, len = 0, sid = 0;
        uint8_t type = 0;
        bool ext = false;
        Bytes buf;
    };

    // Parses the complete chunks in in_; handles finished messages. False = protocol error.
    bool parse() {
        for (;;) {
            size_t pos = 0;
            if (in_.size() < 1) return true;
            const int fmt = in_[0] >> 6;
            int csid = in_[0] & 63;
            pos = 1;
            if (csid == 0) {
                if (in_.size() < 2) return true;
                csid = 64 + in_[1];
                pos = 2;
            } else if (csid == 1) {
                if (in_.size() < 3) return true;
                csid = 64 + in_[1] + in_[2] * 256;
                pos = 3;
            }
            static const int kHead[4] = {11, 7, 3, 0};
            if (in_.size() < pos + kHead[fmt]) return true;
            InStream st = streams_[csid];
            const uint8_t* h = in_.data() + pos;
            uint32_t tsField = 0;
            if (fmt <= 2) tsField = (h[0] << 16) | (h[1] << 8) | h[2];
            if (fmt <= 1) {
                st.len = (h[3] << 16) | (h[4] << 8) | h[5];
                st.type = h[6];
            }
            if (fmt == 0) st.sid = h[7] | (h[8] << 8) | (h[9] << 16) | (static_cast<uint32_t>(h[10]) << 24);
            pos += kHead[fmt];
            if (fmt <= 2) st.ext = tsField == 0xFFFFFF;
            if (st.ext) {
                if (in_.size() < pos + 4) return true;
                pos += 4;
            }
            const size_t want = std::min<size_t>(inChunk_, st.len - std::min<size_t>(st.len, st.buf.size()));
            if (in_.size() < pos + want) return true;
            st.buf.insert(st.buf.end(), in_.begin() + static_cast<std::ptrdiff_t>(pos), in_.begin() + static_cast<std::ptrdiff_t>(pos + want));
            in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(pos + want));
            if (st.buf.size() >= st.len) {
                Bytes msg;
                msg.swap(st.buf);
                streams_[csid] = st;
                if (!handle(st.type, msg)) return false;
            } else {
                streams_[csid] = st;
            }
        }
    }

    static bool Contains(const Bytes& b, const char* s) {
        const size_t n = strlen(s);
        if (b.size() < n) return false;
        for (size_t i = 0; i + n <= b.size(); ++i)
            if (memcmp(b.data() + i, s, n) == 0) return true;
        return false;
    }

    bool handle(uint8_t type, const Bytes& m) {
        auto be32 = [&](size_t o) { return (static_cast<uint32_t>(m[o]) << 24) | (m[o + 1] << 16) | (m[o + 2] << 8) | m[o + 3]; };
        if (type == 1 && m.size() >= 4) {
            inChunk_ = std::max<uint32_t>(1, be32(0) & 0x7FFFFFFF);
        } else if (type == 4 && m.size() >= 6) {
            if (((m[0] << 8) | m[1]) == 6) {  // ping request -> ping response with the same time
                Bytes r = {0, 7, m[2], m[3], m[4], m[5]};
                return message(2, 4, 0, 0, r);
            }
        } else if (type == 5 && m.size() >= 4) {
            window_ = be32(0);
        } else if (type == 6) {
            Bytes r;
            PutU32(r, 2500000);
            return message(2, 5, 0, 0, r);
        } else if (type == 20 || type == 17) {
            size_t o = type == 17 ? 1 : 0;
            if (m.size() < o + 3 || m[o] != 0x02) return true;
            const size_t len = (m[o + 1] << 8) | m[o + 2];
            if (m.size() < o + 3 + len) return true;
            const std::string name(reinterpret_cast<const char*>(m.data() + o + 3), len);
            o += 3 + len;
            double txn = 0;
            if (m.size() >= o + 9 && m[o] == 0x00) {
                uint64_t u = 0;
                for (int i = 0; i < 8; ++i) u = (u << 8) | m[o + 1 + i];
                memcpy(&txn, &u, 8);
                o += 9;
            }
            if (name == "_result" && txn == 1) connected_ = true;
            if (name == "_result" && txn == 4 && m.size() >= o + 10 && m[o] == 0x05 && m[o + 1] == 0x00) {
                uint64_t u = 0;
                for (int i = 0; i < 8; ++i) u = (u << 8) | m[o + 2 + i];
                double sid = 0;
                memcpy(&sid, &u, 8);
                streamId_ = static_cast<uint32_t>(sid);
                haveStream_ = true;
            }
            if (name == "_error") return fail(Contains(m, "Rejected") ? "the server rejected the connection (address or key)" : "the server reported an error");
            if (name == "onStatus") {
                if (Contains(m, "NetStream.Publish.Start")) publishing_ = true;
                else if (Contains(m, "BadName")) return fail("the server refused the stream key (already in use?)");
                else if (Contains(m, "Denied") || Contains(m, "Rejected") || Contains(m, "Unauthorized") || Contains(m, "Failed"))
                    return fail("the server refused the stream (key?)");
            }
        }
        return true;
    }

    bool pump(int waitMs) {
        if (!readSome(waitMs)) return false;
        if (!parse()) return false;
        if (window_ && received_ - lastAck_ >= window_ / 2) {
            lastAck_ = received_;
            Bytes a;
            PutU32(a, received_);
            if (!message(2, 3, 0, 0, a)) return false;
        }
        return true;
    }
    template <typename Pred>
    bool waitFor(Pred done, int timeoutMs, const char* what) {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (!done()) {
            if (stopping()) return false;
            if (GetTickCount64() > deadline) return fail(std::string("no answer from the server (") + what + ")");
            if (!pump(100)) return false;
        }
        return true;
    }

    // ---- session
    bool handshake() {
        Bytes c(1 + 1536, 0);
        c[0] = 3;
        for (size_t i = 9; i < c.size(); ++i) c[i] = static_cast<uint8_t>(rand());
        if (!sendAll(c.data(), c.size())) return false;
        Bytes s(1 + 1536 + 1536);
        if (!readExact(s.data(), s.size(), 8000)) return false;
        if (s[0] != 3) return fail("not an RTMP server");
        return sendAll(s.data() + 1, 1536);  // C2 = S1
    }

    bool open() {
        in_.clear();
        streams_.clear();
        inChunk_ = 128;
        outChunk_ = 128;
        window_ = received_ = lastAck_ = 0;
        connected_ = haveStream_ = publishing_ = false;
        if (!connectSocket() || !handshake()) return false;
        Bytes b;
        PutU32(b, 4096);
        if (!message(2, 1, 0, 0, b)) return false;
        outChunk_ = 4096;
        b.clear();
        AmfString(b, "connect");
        AmfNumber(b, 1);
        b.push_back(0x03);
        AmfKey(b, "app"), AmfString(b, app_);
        AmfKey(b, "type"), AmfString(b, "nonprivate");
        AmfKey(b, "flashVer"), AmfString(b, "FMLE/3.0 (compatible; FMSc/1.0)");
        AmfKey(b, "tcUrl"), AmfString(b, tcUrl_);
        AmfEnd(b);
        if (!message(3, 20, 0, 0, b)) return false;
        if (!waitFor([&] { return connected_; }, 8000, "connect")) return false;
        for (const auto& [cmd, txn] : {std::pair<const char*, double>{"releaseStream", 2}, {"FCPublish", 3}}) {
            b.clear();
            AmfString(b, cmd), AmfNumber(b, txn), AmfNull(b), AmfString(b, playpath_);
            if (!message(3, 20, 0, 0, b)) return false;
        }
        b.clear();
        AmfString(b, "createStream"), AmfNumber(b, 4), AmfNull(b);
        if (!message(3, 20, 0, 0, b)) return false;
        if (!waitFor([&] { return haveStream_; }, 8000, "createStream")) return false;
        b.clear();
        AmfString(b, "publish"), AmfNumber(b, 5), AmfNull(b), AmfString(b, playpath_), AmfString(b, "live");
        if (!message(8, 20, streamId_, 0, b)) return false;
        if (!waitFor([&] { return publishing_; }, 8000, "publish")) return false;
        b.clear();
        AmfString(b, "@setDataFrame");
        AmfString(b, "onMetaData");
        b.push_back(0x08);
        PutU32(b, cfg_.audio ? 12 : 6);
        AmfKey(b, "width"), AmfNumber(b, cfg_.w);
        AmfKey(b, "height"), AmfNumber(b, cfg_.h);
        AmfKey(b, "framerate"), AmfNumber(b, cfg_.fps);
        AmfKey(b, "videocodecid"), AmfNumber(b, 7);
        AmfKey(b, "videodatarate"), AmfNumber(b, cfg_.bitrate / 1000.0);
        AmfKey(b, "encoder"), AmfString(b, "DCS ILARTS");
        if (cfg_.audio) {
            AmfKey(b, "audiocodecid"), AmfNumber(b, 10);
            AmfKey(b, "audiodatarate"), AmfNumber(b, 2);
            AmfKey(b, "audiosamplerate"), AmfNumber(b, 44100);
            AmfKey(b, "audiosamplesize"), AmfNumber(b, 16);
            AmfKey(b, "audiochannels"), AmfNumber(b, 2);
            AmfKey(b, "stereo"), AmfBool(b, true);
        }
        AmfEnd(b);
        return message(4, 18, streamId_, 0, b);
    }

    bool sendUnit(const live::Unit& u) {
        if (needKey_) {
            if (!u.key) return true;
            std::vector<uint8_t> sps, pps;
            if (!live::ParameterSets(u.data, &sps, &pps) && sps_.empty()) return true;
            if (!sps.empty()) sps_ = sps, pps_ = pps;
            if (!sentConfig_) {
                Bytes v = {0x17, 0x00, 0, 0, 0, 0x01, sps_[1], sps_[2], sps_[3], 0xFF, 0xE1};
                PutU16(v, static_cast<uint32_t>(sps_.size()));
                v.insert(v.end(), sps_.begin(), sps_.end());
                v.push_back(1);
                PutU16(v, static_cast<uint32_t>(pps_.size()));
                v.insert(v.end(), pps_.begin(), pps_.end());
                if (!message(6, 9, streamId_, 0, v)) return false;
                if (cfg_.audio) {
                    const Bytes a = {0xAF, 0x00, 0x12, 0x10};  // AAC-LC, 44.1 kHz, stereo
                    if (!message(4, 8, streamId_, 0, a)) return false;
                }
                sentConfig_ = true;
                t0_ = u.t;
                audioFrames_ = 0;
            }
            needKey_ = false;
        }
        const uint32_t ts = static_cast<uint32_t>(std::max(0.0, (u.t - t0_) * 1000.0));
        if (cfg_.audio) {  // silence up to this frame's time: 1024 samples per frame at 44.1 kHz
            while (audioFrames_ * 1024.0 / 44.1 <= ts) {
                Bytes a = {0xAF, 0x01};
                a.insert(a.end(), silence_.begin(), silence_.end());
                if (!message(4, 8, streamId_, static_cast<uint32_t>(audioFrames_ * 1024.0 / 44.1), a)) return false;
                ++audioFrames_;
            }
        }
        Bytes v = {static_cast<uint8_t>(u.key ? 0x17 : 0x27), 0x01, 0, 0, 0};
        v.reserve(u.data.size() + 16);
        live::ForEachNal(u.data.data(), u.data.size(), [&](int type, const uint8_t* b, const uint8_t* e) {
            if (type == 7 || type == 8 || type == 9) return;  // parameter sets go in the config, delimiters are not needed
            PutU32(v, static_cast<uint32_t>(e - b));
            v.insert(v.end(), b, e);
        });
        if (v.size() <= 5) return true;
        return message(6, 9, streamId_, ts, v);
    }

    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        if (cfg_.audio) silence_ = SilentAacFrame();
        while (!stopping()) {
            state_ = Connecting;
            error_.clear();
            bool ok = parseUrl() && open();
            if (ok) {
                state_ = Live;
                liveSince_ = video::Now();
                setText("live");
                needKey_ = true;
                sentConfig_ = false;
                {
                    std::lock_guard<std::mutex> l(m_);
                    q_.clear();
                }
                while (ok && !stopping()) {
                    live::UnitPtr u;
                    {
                        std::unique_lock<std::mutex> l(m_);
                        cv_.wait_for(l, std::chrono::milliseconds(100), [&] { return quit_ || !q_.empty(); });
                        if (!q_.empty()) {
                            u = q_.front();
                            q_.pop_front();
                        }
                    }
                    if (u) ok = sendUnit(*u);
                    if (ok) ok = pump(0);
                }
            }
            closeSocket();
            if (stopping()) break;
            state_ = Retrying;
            setText(error_.empty() ? "connection lost" : error_);
            std::unique_lock<std::mutex> l(m_);
            cv_.wait_for(l, std::chrono::seconds(5), [&] { return quit_; });
        }
        WSACleanup();
        CoUninitialize();
    }

    bool stopping() {
        std::lock_guard<std::mutex> l(m_);
        return quit_;
    }
    bool fail(const std::string& why) {
        if (error_.empty()) error_ = why;
        return false;
    }
    void setText(const std::string& t) {
        std::lock_guard<std::mutex> l(tm_);
        text_ = t;
    }

    Config cfg_;
    std::thread thread_;
    std::atomic<State> state_{Off};
    std::atomic<SOCKET> sock_{INVALID_SOCKET};
    std::atomic<long long> sent_{0};
    std::atomic<long> dropped_{0};
    std::atomic<double> liveSince_{0};
    mutable std::mutex m_, tm_;
    std::condition_variable cv_;
    bool quit_ = false;
    std::deque<live::UnitPtr> q_;
    std::atomic<bool> needKey_{true};
    std::string text_, error_;
    // session (publisher thread only)
    std::string host_, port_, app_, playpath_, tcUrl_;
    Bytes in_;
    std::map<int, InStream> streams_;
    uint32_t inChunk_ = 128, outChunk_ = 128, window_ = 0, received_ = 0, lastAck_ = 0, streamId_ = 0;
    bool connected_ = false, haveStream_ = false, publishing_ = false, sentConfig_ = false;
    std::vector<uint8_t> sps_, pps_;
    Bytes silence_;
    double t0_ = 0;
    long long audioFrames_ = 0;
};

}  // namespace rtmp
