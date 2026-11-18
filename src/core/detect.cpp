#include "core/detect.h"

#include <opencv2/aruco.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <vector>

namespace obs {

namespace {

cv::aruco::ArucoDetector& detector() {
    static cv::aruco::DetectorParameters params = [] {
        cv::aruco::DetectorParameters p;
        p.cornerRefinementMethod = cv::aruco::CORNER_REFINE_SUBPIX;
        p.cornerRefinementWinSize = 5;
        p.cornerRefinementMaxIterations = 50;
        p.cornerRefinementMinAccuracy = 0.01;
        return p;
    }();
    static cv::aruco::Dictionary dict =
        cv::aruco::getPredefinedDictionary(cv::aruco::DICT_4X4_50);
    static cv::aruco::ArucoDetector det(dict, params);
    return det;
}

std::vector<cv::Point2f> metadata_module_centers() {
    std::vector<cv::Point2f> pts;
    pts.reserve(2 * kMetaCols * kMetaRows);
    for (int strip = 0; strip < 2; ++strip) {
        const int y0 = strip == 0 ? kMetaY0Top : kMetaY0Bottom;
        for (int i = 0; i < kMetaCols * kMetaRows; ++i) {
            const int r = i / kMetaCols;
            const int c = i % kMetaCols;
            pts.emplace_back(
                static_cast<float>(kMetaX0 + (c + 0.5) * kMetaModule),
                static_cast<float>(y0 + (r + 0.5) * kMetaModule));
        }
    }
    return pts;
}

}  // namespace

DetectResult detect_homography(const cv::Mat& transport, double offset) {
    DetectResult result;
    if (transport.empty()) return result;

    cv::Mat gray;
    if (transport.channels() == 3) {
        cv::cvtColor(transport, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = transport;
    }

    std::vector<std::vector<cv::Point2f>> corners;
    std::vector<int> ids;
    std::vector<std::vector<cv::Point2f>> rejected;
    detector().detectMarkers(gray, corners, ids, rejected);

    std::vector<cv::Point2f> src;
    std::vector<cv::Point2f> dst;
    for (size_t i = 0; i < ids.size(); ++i) {
        const int id = ids[i];
        if (id < 0 || id > 3 || corners[i].size() != 4) continue;
        const auto canon = marker_corners_canonical(id, offset);
        result.corners[id] = {corners[i][0], corners[i][1], corners[i][2], corners[i][3]};
        for (int k = 0; k < 4; ++k) {
            src.push_back(canon[static_cast<size_t>(k)]);
            dst.push_back(corners[i][static_cast<size_t>(k)]);
        }
    }
    result.marker_count = static_cast<int>(result.corners.size());
    if (src.size() < 8) {
        return result;
    }

    cv::Mat H;
    if (src.size() >= 16) {
        H = cv::findHomography(src, dst, 0);
    } else {
        H = cv::findHomography(src, dst, cv::RANSAC, 3.0);
    }
    if (H.empty()) {
        return result;
    }

    std::vector<cv::Point2f> projected;
    cv::perspectiveTransform(src, projected, H);
    double err = 0.0;
    for (size_t i = 0; i < src.size(); ++i) {
        err += cv::norm(projected[i] - dst[i]);
    }
    result.reproj_error = err / std::max<size_t>(1, src.size());
    result.H = H;
    return result;
}

cv::Mat warp_to_canonical(const cv::Mat& transport, const cv::Mat& H,
                          const Layout& layout) {
    if (H.empty()) {
        return transport.clone();
    }
    cv::Mat Hinv;
    cv::invert(H, Hinv);
    cv::Mat canonical;
    cv::warpPerspective(transport, canonical, Hinv,
                        cv::Size(layout.canvas_w, layout.canvas_h),
                        cv::INTER_LINEAR);
    return canonical;
}

std::optional<Metadata> decode_metadata(const cv::Mat& transport, const cv::Mat& H,
                                        const Layout& layout) {
    (void)layout;
    if (transport.empty()) return std::nullopt;

    cv::Mat gray;
    if (transport.channels() == 3) {
        cv::cvtColor(transport, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = transport;
    }

    auto pts = metadata_module_centers();
    if (!H.empty()) {
        cv::perspectiveTransform(pts, pts, H);
    }

    std::vector<int> bits;
    bits.reserve(pts.size());
    const int h = gray.rows;
    const int w = gray.cols;
    for (const auto& p : pts) {
        const int px = cvRound(p.x);
        const int py = cvRound(p.y);
        const int x0 = std::max(0, px - 2);
        const int x1 = std::min(w, px + 3);
        const int y0 = std::max(0, py - 2);
        const int y1 = std::min(h, py + 3);
        if (x0 >= x1 || y0 >= y1) {
            bits.push_back(0);
            continue;
        }
        const double mean = cv::mean(gray(cv::Rect(x0, y0, x1 - x0, y1 - y0)))[0];
        bits.push_back(mean > 127.0 ? 1 : 0);
    }

    const int per_strip = kMetaCols * kMetaRows;
    std::vector<std::vector<int>> copies;
    copies.reserve(2 * kMetaCopies);
    for (int strip = 0; strip < 2; ++strip) {
        const int base = strip * per_strip;
        for (int k = 0; k < kMetaCopies; ++k) {
            const int start = base + k * kMetaPacketBits;
            if (start + kMetaPacketBits > static_cast<int>(bits.size())) {
                return std::nullopt;
            }
            copies.emplace_back(bits.begin() + start, bits.begin() + start + kMetaPacketBits);
        }
    }

    std::vector<int> voted(static_cast<size_t>(kMetaPacketBits), 0);
    for (int i = 0; i < kMetaPacketBits; ++i) {
        int ones = 0;
        for (const auto& c : copies) ones += c[static_cast<size_t>(i)];
        voted[static_cast<size_t>(i)] = ones * 2 >= static_cast<int>(copies.size()) ? 1 : 0;
    }
    return parse_metadata(voted);
}

int adaptive_margin(const cv::Mat& H) {
    if (H.empty() || H.rows < 2 || H.cols < 2) return 0;
    const double a = H.at<double>(0, 0);
    const double b = H.at<double>(0, 1);
    const double c = H.at<double>(1, 0);
    const double d = H.at<double>(1, 1);
    const double scale = std::sqrt(std::abs(a * d - b * c));
    if (scale > 1.005) return 1;  // any upscaled capture: inset to avoid cell-edge bleed
    if (scale >= 0.95) return 0;
    if (scale >= 0.65) return 1;
    return 2;
}

}  // namespace obs
