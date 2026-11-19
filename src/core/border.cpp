#include "core/border.h"

#include <opencv2/aruco.hpp>
#include <opencv2/imgproc.hpp>

#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "core/key.h"
#include "core/metadata.h"
#include "core/presets.h"

namespace obs {

namespace {

cv::aruco::Dictionary& aruco_dictionary() {
    static cv::aruco::Dictionary dict =
        cv::aruco::getPredefinedDictionary(cv::aruco::DICT_4X4_50);
    return dict;
}

void draw_metadata_strip(cv::Mat& canvas, const std::vector<int>& bits, int x0, int y0) {
    const int total = kMetaCols * kMetaRows;
    for (int i = 0; i < total && i < static_cast<int>(bits.size()); ++i) {
        const int r = i / kMetaCols;
        const int c = i % kMetaCols;
        const cv::Point p{x0 + c * kMetaModule, y0 + r * kMetaModule};
        const cv::Scalar color = bits[static_cast<size_t>(i)] ? cv::Scalar(255, 255, 255)
                                                              : cv::Scalar(0, 0, 0);
        cv::rectangle(canvas, cv::Rect(p, cv::Size(kMetaModule, kMetaModule)), color,
                      cv::FILLED);
    }
}

}  // namespace

cv::Mat build_border(
    const Layout& layout,
    const KeyMaterial& key,
    const TransportOptions& options) {
    layout.validate();
    cv::Mat canvas(layout.canvas_h, layout.canvas_w, CV_8UC3, cv::Scalar(18, 18, 18));

    cv::rectangle(canvas,
                  cv::Rect(layout.content_x - 2, layout.content_y - 2,
                           layout.content_w + 4, layout.content_h + 4),
                  cv::Scalar(70, 70, 70), 1);

    for (int id = 0; id < 4; ++id) {
        cv::Mat marker;
        cv::aruco::generateImageMarker(aruco_dictionary(), id, kMarkerSize, marker, 1);
        cv::Mat box(kMarkerBox, kMarkerBox, CV_8UC1, cv::Scalar(255));
        marker.copyTo(box(cv::Rect(kMarkerQuiet, kMarkerQuiet, kMarkerSize, kMarkerSize)));
        cv::Mat box_bgr;
        cv::cvtColor(box, box_bgr, cv::COLOR_GRAY2BGR);
        const auto& p = marker_positions().at(static_cast<size_t>(id));
        box_bgr.copyTo(canvas(cv::Rect(p.x, p.y, kMarkerBox, kMarkerBox)));
    }

    const auto bits = build_metadata_bits(layout, key.key_id, options);
    draw_metadata_strip(canvas, bits, kMetaX0, kMetaY0Top);
    draw_metadata_strip(canvas, bits, kMetaX0, kMetaY0Bottom);

    for (int i = 0; i < 10; ++i) {
        const int v = static_cast<int>(std::lround(i * 255.0 / 9.0));
        const int y = 200 + i * 52;
        cv::rectangle(canvas, cv::Rect(24, y, 44, 40), cv::Scalar(v, v, v), cv::FILLED);
    }

    const cv::Scalar colors[] = {
        {0, 0, 255}, {0, 255, 0}, {255, 0, 0},
        {0, 255, 255}, {255, 255, 0}, {255, 0, 255},
    };
    for (int i = 0; i < 6; ++i) {
        const int y = 200 + i * 80;
        cv::rectangle(canvas, cv::Rect(1780, y, 60, 60), colors[i], cv::FILLED);
    }

    std::ostringstream label;
    const char* preset_id = "custom";
    if (const auto* preset = preset_by_id(options.preset_id)) {
        preset_id = preset->id;
    }
    label << "OBSCure v1  " << layout.content_w << "x" << layout.content_h
          << "  cell " << layout.cell << " guard " << layout.guard << "  "
          << preset_id << "  " << scramble_algorithm_id(layout.algorithm) << "  color "
          << (options.use_color ? "on" : "off")
          << "  " << (options.public_mode ? "public" : "key") << " "
          << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
          << key.key_id;
    cv::putText(canvas, label.str(), cv::Point(kMetaX0, 88),
                cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(150, 150, 150), 1,
                cv::LINE_AA);
    return canvas;
}

}  // namespace obs
