#include "live/file_source.h"

#include <algorithm>
#include <cmath>

#include "platform/win_util.h"

namespace obs {

bool FileSource::open(const std::wstring& path, std::string* error) {
    close();
    path_ = path;
    const std::string ansi = wide_to_acp(path);
    bool opened = cap_.open(ansi, cv::CAP_MSMF);
    if (!opened) opened = cap_.open(ansi, cv::CAP_ANY);
    if (!opened) {
        if (error) *error = "cannot open video file";
        return false;
    }
    width_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
    height_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
    fps_ = cap_.get(cv::CAP_PROP_FPS);
    if (fps_ <= 0.0 || fps_ > 240.0) fps_ = 60.0;
    if (width_ <= 0 || height_ <= 0) {
        cv::Mat first;
        if (!cap_.read(first) || first.empty()) {
            if (error) *error = "video has no readable frames";
            close();
            return false;
        }
        width_ = first.cols;
        height_ = first.rows;
        cap_.set(cv::CAP_PROP_POS_FRAMES, 0);
    }
    next_ = std::chrono::steady_clock::now();
    return true;
}

void FileSource::close() {
    if (cap_.isOpened()) cap_.release();
    width_ = 0;
    height_ = 0;
    fps_ = 60.0;
}

bool FileSource::read_next(cv::Mat& out) {
    if (!cap_.isOpened()) return false;
    if (!cap_.read(out) || out.empty()) {
        cap_.set(cv::CAP_PROP_POS_FRAMES, 0);
        if (!cap_.read(out) || out.empty()) return false;
    }
    return true;
}

bool FileSource::tick(cv::Mat& out, bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now < next_) return false;
    if (!read_next(out)) return false;
    const auto interval = std::chrono::duration<double>(1.0 / std::max(1.0, fps_));
    next_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
    return true;
}

}  // namespace obs
