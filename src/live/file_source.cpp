#include "live/file_source.h"

#include <algorithm>
#include <cmath>

#include "platform/win_util.h"

namespace obs {

bool FileSource::open(const std::wstring& path, std::string* error) {
    close();
    path_ = path;
    const std::string ansi = wide_to_acp(path);
    ansi_path_ = ansi;
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
    dropped_ = 0;
    return true;
}

void FileSource::close() {
    if (cap_.isOpened()) cap_.release();
    ansi_path_.clear();
    width_ = 0;
    height_ = 0;
    fps_ = 60.0;
}

bool FileSource::read_next(cv::Mat& out) {
    if (!cap_.isOpened()) return false;
    if (!cap_.read(out) || out.empty()) {
        // MSMF does not reliably rewind with CAP_PROP_POS_FRAMES after EOF
        // (it can keep returning a stale black frame). Reopen instead.
        cap_.release();
        bool reopened = cap_.open(ansi_path_, cv::CAP_MSMF);
        if (!reopened) reopened = cap_.open(ansi_path_, cv::CAP_ANY);
        if (!reopened) return false;
        next_ = std::chrono::steady_clock::now();
        if (!cap_.read(out) || out.empty()) return false;
    }
    return true;
}

bool FileSource::tick(cv::Mat& out, bool force) {
    if (!cap_.isOpened()) return false;
    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / std::max(1.0, fps_)));

    if (!force) {
        if (now < next_) return false;

        // We are behind schedule. Grab and discard a few frames so a slow
        // encoder still follows the source clock. Avoid seeking per frame:
        // MSMF seeks are far more expensive than decoding a handful of frames.
        const auto late = now - next_;
        if (late > interval * 2) {
            const long long want = late / interval;
            const long long max_grab = 12;
            long long grabbed = 0;
            for (; grabbed < want && grabbed < max_grab; ++grabbed) {
                if (!cap_.grab()) break;
                ++dropped_;
                next_ += interval;
            }
            // If we are still far behind, restart the cadence instead of
            // accumulating an unbounded backlog.
            if (want > grabbed) next_ = now;
        }
    }

    if (!read_next(out)) return false;
    if (force) {
        next_ = now + interval;
    } else {
        next_ += interval;
        // Never accumulate an unbounded backlog after a failed seek or a long
        // stall; restart the cadence from now.
        if (next_ < now - interval * 4) next_ = now;
    }
    return true;
}

}  // namespace obs
