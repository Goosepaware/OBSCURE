#pragma once

#include <array>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace obs {

// Transport frame geometry. The defaults are the phase-0 validated layout.
struct Layout {
    int canvas_w = 1920;
    int canvas_h = 1080;
    int content_x = 160;
    int content_y = 92;
    int content_w = 1600;
    int content_h = 896;
    int cell = 32;
    int guard = 2;
    int algorithm = 0;  // 0 = classic grid, 1 = Hilbert spatial curve
    std::string key = "obscure-demo-key";

    int cols() const { return content_w / cell; }
    int rows() const { return content_h / cell; }
    int inner() const { return cell - 2 * guard; }
    int blocks() const { return cols() * rows(); }
    int ref_w() const { return cols() * inner(); }
    int ref_h() const { return rows() * inner(); }

    cv::Rect content_rect() const {
        return {content_x, content_y, content_w, content_h};
    }

    void validate() const;
};

// ArUco marker layout. Marker is 64x64 inside an 80x80 white quiet box.
constexpr int kMarkerSize = 64;
constexpr int kMarkerQuiet = 8;
constexpr int kMarkerBox = kMarkerSize + 2 * kMarkerQuiet;
constexpr int kMetaX0 = 200;
constexpr int kMetaY0Top = 20;
constexpr int kMetaY0Bottom = 1012;
constexpr int kMetaCols = 190;
constexpr int kMetaRows = 6;
constexpr int kMetaModule = 8;
constexpr int kMetaPacketBits = 208;
constexpr int kMetaCopies = 5;

// Quiet-box top-left positions, IDs 0..3 clockwise from top-left.
const std::array<cv::Point, 4>& marker_positions();

// The marker's own 64x64 rectangle in canvas coordinates.
cv::Rect marker_rect(int id);

// Marker corners in the canvas, order TL, TR, BR, BL.
std::array<cv::Point2f, 4> marker_corners_canonical(int id, double offset = -0.5);

cv::Rect metadata_rect_top();
cv::Rect metadata_rect_bottom();

}  // namespace obs
