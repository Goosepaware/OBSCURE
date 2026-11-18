#pragma once

#include <array>
#include <map>
#include <optional>

#include <opencv2/core.hpp>

#include "core/layout.h"
#include "core/metadata.h"

namespace obs {

struct DetectResult {
    cv::Mat H;  // canonical -> captured
    int marker_count = 0;
    double reproj_error = 0.0;
    std::map<int, std::array<cv::Point2f, 4>> corners;
};

DetectResult detect_homography(const cv::Mat& transport, double offset = -0.5);

cv::Mat warp_to_canonical(const cv::Mat& transport, const cv::Mat& H,
                          const Layout& layout);

std::optional<Metadata> decode_metadata(const cv::Mat& transport, const cv::Mat& H,
                                        const Layout& layout);

int adaptive_margin(const cv::Mat& H);

}  // namespace obs
