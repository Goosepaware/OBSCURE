#pragma once

#include <opencv2/core.hpp>

#include "core/key.h"
#include "core/layout.h"

namespace obs {

cv::Mat build_border(
    const Layout& layout,
    const KeyMaterial& key,
    bool use_color = true,
    bool use_dihedral = true,
    double extra_scale = 1.0);

}  // namespace obs
