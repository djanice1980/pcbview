#include "video/mf_encoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace pcbview::video {
namespace {

// Small intrusive-free COM pointer; enough for this file.
template <typename T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T** put() {
        reset();
        return &p_;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};

std::string hrText(const char* what, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s failed (hr=0x%08lx)", what,
                  static_cast<unsigned long>(hr));
    return buf;
}

constexpr int64_t kHns = 10'000'000;  // 100ns units per second

}  // namespace

struct MfEncoder::Impl {
    bool mfStarted = false;
    Com<IMFSinkWriter> writer;
    DWORD stream = 0;

    ~Impl() {
        writer.reset();
        if (mfStarted) MFShutdown();
    }

    HRESULT addStreams(int w, int h, int fps, int bitrate, bool hevc) {
        Com<IMFMediaType> out;
        HRESULT hr = MFCreateMediaType(out.put());
        if (FAILED(hr)) return hr;
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE,
                     hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out.get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(out.get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(out.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->AddStream(out.get(), &stream);
        if (FAILED(hr)) return hr;

        Com<IMFMediaType> in;
        hr = MFCreateMediaType(in.put());
        if (FAILED(hr)) return hr;
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in.get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(in.get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(in.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(4 * w));
        return writer->SetInputMediaType(stream, in.get(), nullptr);
    }
};

MfEncoder::MfEncoder() = default;

MfEncoder::~MfEncoder() { delete impl_; }

std::string MfEncoder::open(const std::wstring& path, int width, int height,
                            int fps, int bitsPerSecond, bool preferHevc) {
    delete impl_;
    impl_ = new Impl;
    width_ = width;
    height_ = height;
    fps_ = fps;
    frame_ = 0;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return hrText("MFStartup", hr);
    impl_->mfStarted = true;

    // Hardware transforms give the driver's HEVC/H.264 encoder MFTs.
    Com<IMFAttributes> attrs;
    MFCreateAttributes(attrs.put(), 2);
    if (attrs) {
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    }

    // A failed HEVC AddStream/SetInputMediaType can leave the writer sour;
    // recreate it clean for the H.264 fallback.
    for (const bool tryHevc : preferHevc ? std::vector<bool>{true, false}
                                         : std::vector<bool>{false}) {
        impl_->writer.reset();
        hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.get(),
                                       impl_->writer.put());
        if (FAILED(hr)) return hrText("MFCreateSinkWriterFromURL", hr);
        hr = impl_->addStreams(width, height, fps, bitsPerSecond, tryHevc);
        if (SUCCEEDED(hr)) {
            hevc_ = tryHevc;
            hr = impl_->writer->BeginWriting();
            if (FAILED(hr)) return hrText("BeginWriting", hr);
            return {};
        }
    }
    return hrText("no usable encoder (tried HEVC and H.264)", hr);
}

std::string MfEncoder::writeFrame(const uint8_t* bgraTopDown) {
    if (!impl_ || !impl_->writer) return "encoder not open";
    const LONG rowBytes = width_ * 4;
    const DWORD total = static_cast<DWORD>(rowBytes) * height_;

    Com<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(total, buffer.put());
    if (FAILED(hr)) return hrText("MFCreateMemoryBuffer", hr);
    BYTE* dst = nullptr;
    hr = buffer->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr)) return hrText("Lock", hr);
    // Verified empirically: this SinkWriter pipeline treats positive-stride
    // RGB32 as TOP-DOWN (a bottom-up flip here came out inverted).
    std::memcpy(dst, bgraTopDown, total);
    buffer->Unlock();
    buffer->SetCurrentLength(total);

    Com<IMFSample> sample;
    hr = MFCreateSample(sample.put());
    if (FAILED(hr)) return hrText("MFCreateSample", hr);
    sample->AddBuffer(buffer.get());
    sample->SetSampleTime(frame_ * kHns / fps_);
    sample->SetSampleDuration(kHns / fps_);
    hr = impl_->writer->WriteSample(impl_->stream, sample.get());
    if (FAILED(hr)) return hrText("WriteSample", hr);
    ++frame_;
    return {};
}

std::string MfEncoder::finish() {
    if (!impl_ || !impl_->writer) return "encoder not open";
    const HRESULT hr = impl_->writer->Finalize();
    impl_->writer.reset();
    if (FAILED(hr)) return hrText("Finalize", hr);
    return {};
}

}  // namespace pcbview::video
