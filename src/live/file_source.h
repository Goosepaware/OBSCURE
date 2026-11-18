#pragma once

#include <chrono>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

namespace obs {

// Paced, looping video file source for the encoder/decoder preview.
class FileSource {
public:
    bool open(const std::wstring& path, std::string* error);
    void close();
    bool is_open() const { return cap_.isOpened(); }

    // Returns true when a new frame was produced. If the source is not due yet,
    // returns false and leaves the previous frame untouched.
    bool tick(cv::Mat& out, bool force = false);

    int width() const { return width_; }
    int height() const { return height_; }
    double fps() const { return fps_; }
    uint64_t dropped() const { return dropped_; }
    double position_ms() { return cap_.isOpened() ? cap_.get(cv::CAP_PROP_POS_MSEC) : 0.0; }
    const std::wstring& path() const { return path_; }

private:
    bool read_next(cv::Mat& out);

    cv::VideoCapture cap_;
    std::wstring path_;
    std::string ansi_path_;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 60.0;
    uint64_t dropped_ = 0;
    std::chrono::steady_clock::time_point next_{};
};

}  // namespace obs
