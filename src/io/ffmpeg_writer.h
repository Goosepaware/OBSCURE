#pragma once

#include <cstdio>
#include <string>

#include <opencv2/core.hpp>

namespace obs {

class FfmpegWriter {
public:
    FfmpegWriter() = default;
    ~FfmpegWriter();

    FfmpegWriter(const FfmpegWriter&) = delete;
    FfmpegWriter& operator=(const FfmpegWriter&) = delete;

    bool open(const std::string& ffmpeg, const std::string& output, int width,
              int height, double fps, double bitrate_mbps, bool use_crf = false,
              const std::string& encoder = std::string());
    bool write(const cv::Mat& bgr);
    bool close();
    bool is_open() const { return pipe_ != nullptr; }
    const std::string& error() const { return error_; }

private:
    FILE* pipe_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    std::string error_;
};

}  // namespace obs
