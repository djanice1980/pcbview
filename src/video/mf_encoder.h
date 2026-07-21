#pragma once

// MP4 video encoding through Windows Media Foundation.
//
// Chosen over bundling an encoder: MF ships with Windows, the H.265 (HEVC)
// encoder MFT is provided by the GPU driver (NVENC/AMF/QuickSync -- any
// machine that can run pcbview's ray tracing has one), and the built-in
// software H.264 encoder is the fallback for machines that don't. No new
// DLLs, no GPL-bundling questions.

#include <cstdint>
#include <string>

namespace pcbview::video {

class MfEncoder {
public:
    MfEncoder();
    ~MfEncoder();
    MfEncoder(const MfEncoder&) = delete;
    MfEncoder& operator=(const MfEncoder&) = delete;

    // Opens `path` (.mp4) for width x height at `fps`. `preferHevc` tries
    // H.265 first and silently falls back to H.264 when no HEVC encoder is
    // available; codecUsed() reports the result. Returns an empty string on
    // success, else a description of what failed.
    std::string open(const std::wstring& path, int width, int height, int fps,
                     int bitsPerSecond, bool preferHevc);

    // One frame of top-down BGRA, exactly width*height*4 bytes.
    std::string writeFrame(const uint8_t* bgraTopDown);

    // Finalises the container. The encoder is unusable afterwards.
    std::string finish();

    const char* codecUsed() const { return hevc_ ? "H.265" : "H.264"; }
    int64_t framesWritten() const { return frame_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool hevc_ = false;
    int width_ = 0, height_ = 0, fps_ = 30;
    int64_t frame_ = 0;
};

}  // namespace pcbview::video
