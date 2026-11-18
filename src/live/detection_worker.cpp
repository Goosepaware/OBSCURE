#include "live/detection_worker.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "core/detect.h"

namespace obs {

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
    cv::Mat image = bgr;
    double sx = 1.0;
    double sy = 1.0;
    if (bgr.cols > 1920) {
        const int dw = 1920;
        const int dh = static_cast<int>(std::lround(bgr.rows * (1920.0 / bgr.cols)));
        cv::resize(bgr, image, cv::Size(dw, dh), 0, 0, cv::INTER_AREA);
        sx = static_cast<double>(bgr.cols) / dw;
        sy = static_cast<double>(bgr.rows) / dh;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto res = detect_homography(image);
    out->ms = std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    out->markers = res.marker_count;
    if (res.H.empty()) {
        out->ok = false;
        out->key_ok = false;
        return false;
    }
    cv::Mat scaled = (cv::Mat_<double>(3, 3) << sx, 0, 0, 0, sy, 0, 0, 0, 1) * res.H;
    out->H = scaled;
    out->margin = adaptive_margin(scaled);
    const auto metadata = decode_metadata(image, res.H, layout);
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
