#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include <opencv2/core.hpp>

#include "core/key.h"
#include "core/layout.h"
#include "core/metadata.h"

namespace obs {

struct DetectionOutcome {
    bool ok = false;
    cv::Mat H;
    int margin = 0;
    bool key_ok = false;
    int markers = 0;
    double ms = 0.0;
    std::optional<Metadata> metadata;
};

// Runs ArUco detection away from the render thread. Submit at ~1 Hz; take()
// returns the latest completed result without blocking.
class DetectionWorker {
public:
    DetectionWorker(Layout layout, KeyMaterial key);
    ~DetectionWorker();
    DetectionWorker(const DetectionWorker&) = delete;
    DetectionWorker& operator=(const DetectionWorker&) = delete;

    void start();
    void stop();
    void submit(const cv::Mat& bgr);
    bool take(DetectionOutcome* out);

    static bool detect(const cv::Mat& bgr, const Layout& layout, const KeyMaterial& key,
                       DetectionOutcome* out);

private:
    void run();

    Layout layout_;
    KeyMaterial key_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool has_frame_ = false;
    cv::Mat frame_;
    bool has_result_ = false;
    DetectionOutcome result_;
};

}  // namespace obs
