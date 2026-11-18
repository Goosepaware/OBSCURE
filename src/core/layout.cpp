#include "core/layout.h"

#include <opencv2/imgproc.hpp>

#include <stdexcept>

namespace obs {

void Layout::validate() const {
    if (content_w % cell != 0 || content_h % cell != 0) {
        throw std::runtime_error("content size must be cell aligned");
    }
    if (inner() < 8) {
        throw std::runtime_error("guard too large for cell");
    }
    if (content_x < 0 || content_y < 0 ||
        content_x + content_w > canvas_w ||
        content_y + content_h > canvas_h) {
        throw std::runtime_error("content rect outside canvas");
    }
}

const std::array<cv::Point, 4>& marker_positions() {
    static const std::array<cv::Point, 4> pos = {
        cv::Point{80, 12},
        cv::Point{1760, 12},
        cv::Point{1760, 988},
        cv::Point{80, 988},
    };
    return pos;
}

cv::Rect marker_rect(int id) {
    const auto& p = marker_positions().at(static_cast<size_t>(id));
    return {p.x + kMarkerQuiet, p.y + kMarkerQuiet, kMarkerSize, kMarkerSize};
}

std::array<cv::Point2f, 4> marker_corners_canonical(int id, double offset) {
    const cv::Rect r = marker_rect(id);
    const float x = static_cast<float>(r.x + offset);
    const float y = static_cast<float>(r.y + offset);
    const float w = static_cast<float>(r.width);
    const float h = static_cast<float>(r.height);
    return {
        cv::Point2f{x, y},
        cv::Point2f{x + w, y},
        cv::Point2f{x + w, y + h},
        cv::Point2f{x, y + h},
    };
}

cv::Rect metadata_rect_top() {
    return {kMetaX0, kMetaY0Top, kMetaCols * kMetaModule, kMetaRows * kMetaModule};
}

cv::Rect metadata_rect_bottom() {
    return {kMetaX0, kMetaY0Bottom, kMetaCols * kMetaModule, kMetaRows * kMetaModule};
}

}  // namespace obs
