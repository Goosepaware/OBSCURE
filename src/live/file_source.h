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

    // Reads the next frame without wall-clock pacing and without looping.
    bool read_sequential(cv::Mat& out);

    // Seeks to a timestamp and reads the frame at that position. Paused state is
    // preserved; the caller decides when to resume.
    bool seek_ms(double ms, cv::Mat& out);

    int width() const { return width_; }
    int height() const { return height_; }
    double fps() const { return fps_; }
    double duration_ms() const { return duration_ms_; }
    double position_ms() const { return position_ms_; }
    int total_frames() const { return total_frames_; }
    uint64_t dropped() const { return dropped_; }
    bool paused() const { return paused_; }
    void set_paused(bool paused);
    bool loop() const { return loop_; }
    void set_loop(bool loop) { loop_ = loop; }
    const std::wstring& path() const { return path_; }

private:
    bool read_next(cv::Mat& out);
    void update_position();
    bool reopen();

    cv::VideoCapture cap_;
    std::wstring path_;
    std::string ansi_path_;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 60.0;
    double duration_ms_ = 0.0;
    double position_ms_ = 0.0;
    int total_frames_ = 0;
    int frame_index_ = 0;
    uint64_t dropped_ = 0;
    bool paused_ = false;
    bool loop_ = true;
    std::chrono::steady_clock::time_point next_{};
};

}  // namespace obs
