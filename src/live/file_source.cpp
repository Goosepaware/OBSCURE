#include "live/file_source.h"

#include <algorithm>
#include <cmath>

#include "platform/win_util.h"

namespace obs {

namespace {
using Clock = std::chrono::steady_clock;
}

bool FileSource::open(const std::wstring& path, std::string* error) {
    close();
    path_ = path;
    ansi_path_ = wide_to_acp(path);
    if (!reopen()) {
        if (error) *error = "cannot open video file";
        return false;
    }
    width_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
    height_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
    fps_ = cap_.get(cv::CAP_PROP_FPS);
    if (fps_ <= 0.0 || fps_ > 240.0) fps_ = 60.0;
    total_frames_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_COUNT));
    if (total_frames_ < 0) total_frames_ = 0;
    duration_ms_ = total_frames_ > 0
                       ? static_cast<double>(total_frames_) * 1000.0 / fps_
                       : 0.0;
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
    next_ = Clock::now();
    dropped_ = 0;
    position_ms_ = 0.0;
    frame_index_ = 0;
    paused_ = false;
    return true;
}

bool FileSource::reopen() {
    cap_.release();
    bool opened = cap_.open(ansi_path_, cv::CAP_MSMF);
    if (!opened) opened = cap_.open(ansi_path_, cv::CAP_ANY);
    return opened;
}

void FileSource::close() {
    if (cap_.isOpened()) cap_.release();
    ansi_path_.clear();
    width_ = 0;
    height_ = 0;
    fps_ = 60.0;
    duration_ms_ = 0.0;
    position_ms_ = 0.0;
    total_frames_ = 0;
    frame_index_ = 0;
    paused_ = false;
}

void FileSource::update_position() {
    const double pos = cap_.get(cv::CAP_PROP_POS_MSEC);
    if (pos > 0.0) {
        position_ms_ = pos;
    } else {
        position_ms_ = static_cast<double>(frame_index_ + 1) * 1000.0 / fps_;
    }
    ++frame_index_;
    if (duration_ms_ > 0.0) {
        position_ms_ = std::min(position_ms_, duration_ms_);
    }
}

bool FileSource::read_next(cv::Mat& out) {
    if (!cap_.isOpened()) return false;
    if (!cap_.read(out) || out.empty()) {
        if (!loop_) return false;
        // MSMF does not reliably rewind with CAP_PROP_POS_FRAMES after EOF
        // (it can keep returning a stale black frame). Reopen instead.
        if (!reopen()) return false;
        next_ = Clock::now();
        position_ms_ = 0.0;
        frame_index_ = 0;
        if (!cap_.read(out) || out.empty()) return false;
    }
    update_position();
    return true;
}

bool FileSource::tick(cv::Mat& out, bool force) {
    if (!cap_.isOpened()) return false;
    if (paused_ && !force) return false;
    const auto now = Clock::now();
    const auto interval = std::chrono::duration_cast<Clock::duration>(
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
                ++frame_index_;
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

bool FileSource::read_sequential(cv::Mat& out) {
    if (!cap_.isOpened()) return false;
    if (!cap_.read(out) || out.empty()) return false;
    update_position();
    return true;
}

bool FileSource::seek_ms(double ms, cv::Mat& out) {
    if (!cap_.isOpened()) return false;
    ms = std::max(0.0, ms);
    if (duration_ms_ > 0.0) ms = std::min(ms, duration_ms_);
    int target = static_cast<int>(std::llround(ms / 1000.0 * fps_));
    target = std::max(0, target);

    bool ok = false;
    if (total_frames_ > 0) {
        ok = cap_.set(cv::CAP_PROP_POS_FRAMES, target);
    }
    if (!ok) {
        ok = cap_.set(cv::CAP_PROP_POS_MSEC, ms);
    }
    if (!ok && reopen()) {
        if (total_frames_ > 0) {
            cap_.set(cv::CAP_PROP_POS_FRAMES, target);
        } else {
            cap_.set(cv::CAP_PROP_POS_MSEC, ms);
        }
    }

    if (!cap_.read(out) || out.empty()) {
        if (!reopen()) return false;
        if (total_frames_ > 0) {
            cap_.set(cv::CAP_PROP_POS_FRAMES, target);
        } else {
            cap_.set(cv::CAP_PROP_POS_MSEC, ms);
        }
        if (!cap_.read(out) || out.empty()) return false;
    }

    frame_index_ = target;
    position_ms_ = ms;
    next_ = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                               std::chrono::duration<double>(1.0 / std::max(1.0, fps_)));
    return true;
}

void FileSource::set_paused(bool paused) {
    paused_ = paused;
    if (!paused_) next_ = Clock::now();
}

}  // namespace obs
