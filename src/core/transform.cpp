#include "core/transform.h"

#include <opencv2/imgproc.hpp>

#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace obs {

namespace {

void invert_channels(cv::Mat& img) {
    cv::bitwise_not(img, img);
}

}  // namespace

int inverse_dihedral_code(int code) {
    if (code < 0 || code > 7) {
        throw std::runtime_error("bad dihedral code");
    }
    static const std::array<int, 8> inverse = [] {
        cv::Mat probe(4, 4, CV_8UC1);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                probe.at<uchar>(y, x) = static_cast<uchar>(y * 4 + x);
            }
        }
        std::array<int, 8> mapping{};
        for (int c = 0; c < 8; ++c) {
            cv::Mat t;
            apply_dihedral(probe, t, c);
            bool found = false;
            for (int inv = 0; inv < 8; ++inv) {
                cv::Mat back;
                apply_dihedral(t, back, inv);
                if (cv::norm(back, probe, cv::NORM_INF) == 0.0) {
                    mapping[static_cast<size_t>(c)] = inv;
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw std::runtime_error("dihedral group is incomplete");
            }
        }
        return mapping;
    }();
    return inverse[static_cast<size_t>(code)];
}

void apply_dihedral(const cv::Mat& src, cv::Mat& dst, int code) {
    switch (code) {
        case 0:
            src.copyTo(dst);
            break;
        case 1:
            cv::rotate(src, dst, cv::ROTATE_90_CLOCKWISE);
            break;
        case 2:
            cv::rotate(src, dst, cv::ROTATE_180);
            break;
        case 3:
            cv::rotate(src, dst, cv::ROTATE_90_COUNTERCLOCKWISE);
            break;
        case 4:
            cv::flip(src, dst, 0);
            break;
        case 5:
        case 6:
        case 7: {
            cv::Mat tmp;
            cv::flip(src, tmp, 0);
            if (code == 5) {
                cv::rotate(tmp, dst, cv::ROTATE_90_CLOCKWISE);
            } else if (code == 6) {
                cv::rotate(tmp, dst, cv::ROTATE_180);
            } else {
                cv::rotate(tmp, dst, cv::ROTATE_90_COUNTERCLOCKWISE);
            }
            break;
        }
        default:
            throw std::runtime_error("bad dihedral code");
    }
}

void apply_color(cv::Mat& img, int code) {
    if (code == 0) return;
    if (code == 1) {
        invert_channels(img);
        return;
    }

    std::vector<cv::Mat> ch;
    cv::split(img, ch);
    bool invert = false;
    switch (code) {
        case 2:
            std::swap(ch[0], ch[2]);
            break;
        case 3:
            std::swap(ch[0], ch[2]);
            invert = true;
            break;
        case 4:
            std::swap(ch[1], ch[2]);  // swap R/G
            break;
        case 5:
            std::swap(ch[1], ch[2]);  // swap R/G
            invert = true;
            break;
        case 6:
            std::swap(ch[0], ch[1]);  // swap G/B
            break;
        case 7:
            std::swap(ch[0], ch[1]);  // swap G/B
            invert = true;
            break;
        default:
            throw std::runtime_error("bad color code");
    }
    if (invert) {
        for (auto& c : ch) invert_channels(c);
    }
    cv::merge(ch, img);
}

cv::Mat make_reference(const cv::Mat& source_bgr, const Layout& layout,
                       double extra_scale) {
    if (source_bgr.empty()) {
        throw std::runtime_error("empty source frame");
    }
    if (extra_scale > 1.0 + 1e-6) {
        const int sw = std::max(1, static_cast<int>(std::lround(layout.ref_w() / extra_scale)));
        const int sh = std::max(1, static_cast<int>(std::lround(layout.ref_h() / extra_scale)));
        cv::Mat small;
        cv::resize(source_bgr, small, cv::Size(sw, sh), 0, 0, cv::INTER_AREA);
        cv::Mat ref;
        cv::resize(small, ref, cv::Size(layout.ref_w(), layout.ref_h()), 0, 0,
                   cv::INTER_LINEAR);
        return ref;
    }
    cv::Mat ref;
    cv::resize(source_bgr, ref, cv::Size(layout.ref_w(), layout.ref_h()), 0, 0,
               cv::INTER_AREA);
    return ref;
}

cv::Mat obfuscate_content(const cv::Mat& source_bgr, const Layout& layout,
                          const KeyMaterial& key, double extra_scale,
                          bool use_color, bool use_dihedral) {
    layout.validate();
    cv::Mat ref = make_reference(source_bgr, layout, extra_scale);
    const int rows = layout.rows();
    const int cols = layout.cols();
    const int inner = layout.inner();
    const int cell = layout.cell;
    const int guard = layout.guard;

    std::vector<cv::Mat> tiles(static_cast<size_t>(rows * cols));
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            tiles[static_cast<size_t>(y * cols + x)] =
                ref(cv::Rect(x * inner, y * inner, inner, inner)).clone();
        }
    }

    cv::Mat content(layout.content_h, layout.content_w, CV_8UC3, cv::Scalar(12, 12, 12));
    for (int d = 0; d < rows * cols; ++d) {
        const int b = key.perm[static_cast<size_t>(d)];
        cv::Mat tile;
        apply_dihedral(tiles[static_cast<size_t>(b)], tile,
                       use_dihedral ? key.dihedral[static_cast<size_t>(d)] : 0);
        if (use_color) {
            apply_color(tile, key.color[static_cast<size_t>(d)]);
        }
        const int dx = d % cols;
        const int dy = d / cols;
        tile.copyTo(content(cv::Rect(dx * cell + guard, dy * cell + guard, inner, inner)));
    }
    return content;
}

cv::Mat deobfuscate_content(const cv::Mat& content, const Layout& layout,
                            const KeyMaterial& key, bool use_color,
                            bool use_dihedral, int margin) {
    const int rows = layout.rows();
    const int cols = layout.cols();
    const int inner = layout.inner();
    const int cell = layout.cell;
    const int sample = inner - 2 * margin;
    if (sample < 8) {
        throw std::runtime_error("decoder margin too large");
    }
    const int g = layout.guard + margin;

    cv::Mat restored(rows * sample, cols * sample, CV_8UC3, cv::Scalar(0, 0, 0));
    for (int d = 0; d < rows * cols; ++d) {
        const int dx = d % cols;
        const int dy = d / cols;
        cv::Mat tile =
            content(cv::Rect(dx * cell + g, dy * cell + g, sample, sample)).clone();
        if (use_color) {
            apply_color(tile, key.color[static_cast<size_t>(d)]);
        }
        if (use_dihedral) {
            cv::Mat inv;
            apply_dihedral(tile, inv,
                           inverse_dihedral_code(key.dihedral[static_cast<size_t>(d)]));
            tile = inv;
        }
        const int b = key.perm[static_cast<size_t>(d)];
        const int bx = b % cols;
        const int by = b / cols;
        tile.copyTo(restored(cv::Rect(bx * sample, by * sample, sample, sample)));
    }

    if (sample != inner) {
        cv::Mat out;
        cv::resize(restored, out, cv::Size(layout.ref_w(), layout.ref_h()), 0, 0,
                   cv::INTER_LINEAR);
        return out;
    }
    return restored;
}

cv::Mat compose_transport(const cv::Mat& content, const cv::Mat& border,
                          const Layout& layout) {
    cv::Mat transport = border.clone();
    content.copyTo(transport(layout.content_rect()));
    return transport;
}

cv::Mat crop_content(const cv::Mat& transport, const Layout& layout) {
    return transport(layout.content_rect()).clone();
}

}  // namespace obs
