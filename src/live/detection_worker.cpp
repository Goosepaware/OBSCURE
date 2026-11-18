#include "live/detection_worker.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "core/detect.h"

namespace obs {

namespace {

struct DetectionAttempt {
    int markers = 0;
    double error = 1e9;
    cv::Mat image;    // scaled, unpadded input
    cv::Mat H_image;  // canonical -> image
    cv::Mat H_orig;   // canonical -> original capture
};

DetectionAttempt try_detect(const cv::Mat& bgr, int max_width, int pad) {
    DetectionAttempt attempt;
    cv::Mat image = bgr;
    double sx = 1.0;
    double sy = 1.0;
    if (max_width > 0 && bgr.cols > max_width) {
        const int dw = max_width;
        const int dh = static_cast<int>(std::lround(bgr.rows * (static_cast<double>(max_width) / bgr.cols)));
        cv::resize(bgr, image, cv::Size(dw, dh), 0, 0, cv::INTER_AREA);
        sx = static_cast<double>(bgr.cols) / dw;
        sy = static_cast<double>(bgr.rows) / dh;
    }

    cv::Mat input;
    if (pad > 0) {
        cv::copyMakeBorder(image, input, pad, pad, pad, pad, cv::BORDER_CONSTANT,
                           cv::Scalar(0, 0, 0));
    } else {
        input = image;
    }

    const auto res = detect_homography(input);
    attempt.image = image;
    attempt.markers = res.marker_count;
    attempt.error = res.reproj_error;
    if (res.H.empty()) return attempt;

    cv::Mat H = res.H;
    if (pad > 0) {
        const cv::Mat shift = (cv::Mat_<double>(3, 3) << 1, 0, -pad, 0, 1, -pad, 0, 0, 1);
        H = shift * H;
    }
    attempt.H_image = H.clone();
    const cv::Mat scale = (cv::Mat_<double>(3, 3) << sx, 0, 0, 0, sy, 0, 0, 0, 1);
    attempt.H_orig = scale * H;
    return attempt;
}

}  // namespace

DetectionWorker::DetectionWorker(Layout layout, KeyMaterial key)
    : layout_(std::move(layout)), key_(std::move(key)) {}

DetectionWorker::~DetectionWorker() {
    stop();
}

void DetectionWorker::start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void DetectionWorker::stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DetectionWorker::submit(const cv::Mat& bgr) {
    if (bgr.empty()) return;
    {
        std::lock_guard lock(mutex_);
        frame_ = bgr.clone();
        has_frame_ = true;
    }
    cv_.notify_one();
}

bool DetectionWorker::take(DetectionOutcome* out) {
    if (!out) return false;
    std::lock_guard lock(mutex_);
    if (!has_result_) return false;
    *out = std::move(result_);
    has_result_ = false;
    return true;
}

bool DetectionWorker::detect(const cv::Mat& bgr, const Layout& layout,
                             const KeyMaterial& key, DetectionOutcome* out) {
    if (bgr.empty() || !out) return false;

    struct AttemptSpec {
        int max_width;
        int pad;
    };
    std::vector<AttemptSpec> attempts;
    attempts.push_back({1920, 32});
    if (bgr.cols > 1920 && bgr.cols <= 2560) attempts.push_back({0, 32});
    if (bgr.cols > 2200) attempts.push_back({1280, 32});

    const auto t0 = std::chrono::steady_clock::now();
    DetectionAttempt best;
    for (const auto& spec : attempts) {
        DetectionAttempt current = try_detect(bgr, spec.max_width, spec.pad);
        if (current.markers > best.markers ||
            (current.markers == best.markers && current.error < best.error)) {
            best = std::move(current);
        }
    }
    out->ms = std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    out->markers = best.markers;
    if (best.H_orig.empty()) {
        out->ok = false;
        out->key_ok = false;
        return false;
    }
    out->H = best.H_orig;
    out->margin = adaptive_margin(best.H_orig);
    const auto metadata = decode_metadata(best.image, best.H_image, layout);
    out->key_ok = metadata.has_value() && metadata->key_id == key.key_id;
    out->ok = true;
    return true;
}

void DetectionWorker::run() {
    for (;;) {
        cv::Mat frame;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || has_frame_; });
            if (stop_ && !has_frame_) return;
            frame = std::move(frame_);
            has_frame_ = false;
        }
        DetectionOutcome outcome;
        detect(frame, layout_, key_, &outcome);
        {
            std::lock_guard lock(mutex_);
            result_ = std::move(outcome);
            has_result_ = true;
        }
    }
}

}  // namespace obs
