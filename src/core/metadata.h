#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/layout.h"

namespace obs {

struct Metadata {
    int version = 0;
    int canvas_w = 0;
    int canvas_h = 0;
    int content_x = 0;
    int content_y = 0;
    int content_w = 0;
    int content_h = 0;
    int cell = 0;
    int guard = 0;
    int cols = 0;
    int rows = 0;
    uint32_t key_id = 0;
    int flags = 0;
    int inner = 0;
    int reserved = 0;
    uint16_t crc = 0;
};

uint16_t crc16_ccitt(const uint8_t* data, size_t size);

std::vector<int> build_metadata_bits(
    const Layout& layout,
    uint32_t key_id,
    int flags,
    bool use_color,
    bool use_dihedral,
    double extra_scale);

std::optional<Metadata> parse_metadata(const std::vector<int>& bits);

}  // namespace obs
