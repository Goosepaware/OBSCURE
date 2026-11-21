#include "io/ffmpeg_writer.h"

#include <io.h>
#include <fcntl.h>

#include <sstream>
#include <stdexcept>

#include "io/hw_encoder.h"

namespace obs {

namespace {

std::string quote_if_needed(const std::string& s) {
    if (s.find(' ') == std::string::npos && s.find('\t') == std::string::npos) {
        return s;
    }
    return "\"" + s + "\"";
}

}  // namespace

FfmpegWriter::~FfmpegWriter() {
    close();
}

bool FfmpegWriter::open(const std::string& ffmpeg, const std::string& output, int width,
                        int height, double fps, double bitrate_mbps, bool use_crf,
                        const std::string& encoder) {
    close();
    width_ = width;
    height_ = height;
    if (fps <= 0) fps = 60.0;

    std::ostringstream cmd;
    cmd << quote_if_needed(ffmpeg) << " -hide_banner -loglevel error -y"
        << " -f rawvideo -pix_fmt bgr24 -s " << width << "x" << height
        << " -r " << fps << " -i - -an";
    if (use_crf) {
        // CRF is an x264/x265 concept; hardware encoders use the CBR path.
        cmd << " -c:v libx264 -preset veryfast -crf 16";
    } else {
        const std::string name = encoder.empty() ? "libx264" : encoder;
        cmd << video_encoder_args(name, bitrate_mbps, 60, false);
    }
    cmd << " -pix_fmt yuv420p -g 60 -keyint_min 60 -sc_threshold 0"
        << " -movflags +faststart " << quote_if_needed(output);

    pipe_ = _popen(cmd.str().c_str(), "wb");
    if (!pipe_) {
        error_ = "failed to start ffmpeg writer";
        return false;
    }
    _setmode(_fileno(pipe_), _O_BINARY);
    return true;
}

bool FfmpegWriter::write(const cv::Mat& bgr) {
    if (!pipe_) return false;
    if (bgr.empty() || bgr.cols != width_ || bgr.rows != height_) {
        error_ = "frame size mismatch";
        return false;
    }
    cv::Mat frame = bgr;
    if (!frame.isContinuous()) frame = frame.clone();
    if (frame.channels() != 3) {
        error_ = "expected 3-channel BGR";
        return false;
    }
    const size_t bytes = frame.total() * frame.elemSize();
    const size_t written = std::fwrite(frame.data, 1, bytes, pipe_);
    if (written != bytes) {
        error_ = "short write to ffmpeg";
        return false;
    }
    return true;
}

bool FfmpegWriter::close() {
    if (!pipe_) return true;
    std::fflush(pipe_);
    const int rc = _pclose(pipe_);
    pipe_ = nullptr;
    if (rc != 0) {
        error_ = "ffmpeg writer exited with code " + std::to_string(rc);
        return false;
    }
    return true;
}

}  // namespace obs
