#pragma once

#include <opencv2/core.hpp>

#include "core/key.h"
#include "core/layout.h"

namespace obs {

cv::Mat make_reference(const cv::Mat& source_bgr, const Layout& layout,
                       double extra_scale = 1.0);

cv::Mat obfuscate_content(const cv::Mat& source_bgr, const Layout& layout,
                          const KeyMaterial& key, double extra_scale = 1.0,
                          bool use_color = true, bool use_dihedral = true);

cv::Mat deobfuscate_content(const cv::Mat& content, const Layout& layout,
                            const KeyMaterial& key, bool use_color = true,
                            bool use_dihedral = true, int margin = 0);

cv::Mat compose_transport(const cv::Mat& content, const cv::Mat& border,
                          const Layout& layout);

cv::Mat crop_content(const cv::Mat& transport, const Layout& layout);

void apply_dihedral(const cv::Mat& src, cv::Mat& dst, int code);
int inverse_dihedral_code(int code);
void apply_color(cv::Mat& img, int code);

}  // namespace obs
