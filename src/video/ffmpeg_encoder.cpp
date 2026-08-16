#include "video/mf_encoder.h"

// The Linux implementation of the encoder interface: raw BGRA frames piped to
// the system ffmpeg, which owns the codec and the container. Same shape as the
// Windows side's use of Media Foundation -- the OS-native encoding path, no
// codec libraries bundled (and no GPL-bundling questions: ffmpeg is a separate
// process, not a linked library).
//
// Software x264/x265 only, deliberately. The recorder renders every frame to
// full convergence before encoding, so the path tracer -- not the encoder --
// is the clock; hardware NVENC would buy nothing here and adds a
// works-only-on-some-machines axis.

#include <sys/wait.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace pcbview::video {
namespace {

// Single-quote a string for /bin/sh (popen runs one). ' becomes '\''.
std::string shQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

// Run a command, capture its stdout. Empty on any failure.
std::string capture(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

}  // namespace

struct MfEncoder::Impl {
    FILE* pipe = nullptr;
    std::string logPath;  // ffmpeg's stderr, for error reporting

    ~Impl() {
        if (pipe) pclose(pipe);
        if (!logPath.empty()) std::remove(logPath.c_str());
    }

    // Last few lines of the stderr log -- the part of an ffmpeg failure that
    // actually says what went wrong.
    std::string logTail() const {
        FILE* f = std::fopen(logPath.c_str(), "r");
        if (!f) return {};
        std::string all;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
        std::fclose(f);
        while (!all.empty() && all.back() == '\n') all.pop_back();
        if (all.empty()) return {};
        size_t pos = all.size();
        for (int lines = 0; lines < 4 && pos > 0; ++lines) {
            const size_t nl = all.find_last_of('\n', pos - 1);
            if (nl == std::string::npos) return all;
            pos = nl;
        }
        return all.substr(pos + 1);
    }
};

MfEncoder::MfEncoder() = default;

MfEncoder::~MfEncoder() {
    delete impl_;
}

std::string MfEncoder::open(const std::wstring& path, int width, int height,
                            int fps, int bitsPerSecond, bool preferHevc) {
    if (impl_) return "encoder already open";
    if (width <= 0 || height <= 0 || fps <= 0) return "bad video dimensions";

    // A dead ffmpeg shows up as EPIPE on a later fwrite, not as a signal.
    // Process-global, but nothing else in the app relies on SIGPIPE's default
    // (terminal) disposition.
    std::signal(SIGPIPE, SIG_IGN);

    const std::string versions = capture("ffmpeg -hide_banner -encoders 2>/dev/null");
    if (versions.empty())
        return "ffmpeg not found -- install it (pacman -S ffmpeg) and retry";

    // H.265 only if its encoder is actually compiled in; otherwise fall back
    // to H.264 silently, exactly as the Windows side does when no HEVC MFT is
    // present. codecUsed() reports which one ran.
    hevc_ = preferHevc && versions.find(" libx265") != std::string::npos;
    const char* codec = hevc_ ? "libx265" : "libx264";

    width_ = width;
    height_ = height;
    fps_ = fps;
    frame_ = 0;

    const std::string out = std::filesystem::path(path).string();

    auto impl = new Impl;
    impl->logPath = out + ".ffmpeg.log";

    // -f rawvideo -pix_fmt bgra: exactly the writeFrame contract (top-down
    // BGRA, width*height*4 bytes). yuv420p output: what players expect; both
    // dimensions are even (the renderer aligns them), so no scale needed.
    char head[512];
    std::snprintf(head, sizeof head,
                  "ffmpeg -y -hide_banner -f rawvideo -pix_fmt bgra -s %dx%d "
                  "-r %d -i - -c:v %s -b:v %d -pix_fmt yuv420p -movflags "
                  "+faststart ",
                  width, height, fps, codec, bitsPerSecond);
    const std::string cmd =
        std::string(head) + shQuote(out) + " 2> " + shQuote(impl->logPath);

    impl->pipe = popen(cmd.c_str(), "w");
    if (!impl->pipe) {
        const int e = errno;
        delete impl;
        return std::string("could not start ffmpeg: ") + std::strerror(e);
    }
    impl_ = impl;
    return {};
}

std::string MfEncoder::writeFrame(const uint8_t* bgraTopDown) {
    if (!impl_ || !impl_->pipe) return "encoder not open";
    const size_t bytes =
        static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4;
    if (fwrite(bgraTopDown, 1, bytes, impl_->pipe) != bytes) {
        // ffmpeg died mid-stream; its stderr says why.
        const std::string why = impl_->logTail();
        return "ffmpeg stopped accepting frames" +
               (why.empty() ? std::string() : (": " + why));
    }
    ++frame_;
    return {};
}

std::string MfEncoder::finish() {
    if (!impl_ || !impl_->pipe) return "encoder not open";
    const int status = pclose(impl_->pipe);
    impl_->pipe = nullptr;
    std::string err;
    if (status == -1) {
        err = std::string("waiting for ffmpeg failed: ") + std::strerror(errno);
    } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        const std::string why = impl_->logTail();
        err = "ffmpeg exited with code " + std::to_string(WEXITSTATUS(status)) +
              (why.empty() ? std::string() : (": " + why));
    } else if (WIFSIGNALED(status)) {
        err = "ffmpeg was killed by signal " + std::to_string(WTERMSIG(status));
    }
    delete impl_;
    impl_ = nullptr;
    return err;
}

}  // namespace pcbview::video
