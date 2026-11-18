#pragma once

#include <opencv2/core.hpp>

#include "core/key.h"
#include "core/layout.h"
#include "core/metadata.h"

namespace obs {

cv::Mat build_border(
    const Layout& layout,
    const KeyMaterial& key,
    const TransportOptions& options);

}  // namespace obs
