#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/layout.h"

namespace obs {

struct TransportOptions {
    bool use_color = true;
    bool use_dihedral = true;
    bool public_mode = false;
    int preset_id = 0;   // 1-based stable preset id, 0 = custom/unknown
    int scale_x10 = 0;   // 0 = unknown, otherwise 10/12/14...
    int algorithm = 0;   // 0 = classic grid, 1 = Hilbert spatial curve
    bool use_audio = false;
    int audio_algorithm = 0;  // 0 = subband shift, 1 = mirror inversion
    int audio_version = 2;    // 1 = legacy, 2 = pre-emphasis + guard band
};

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
    int preset_id = 0;
    int scale_x10 = 0;
    uint16_t crc = 0;

    bool use_color() const { return (flags & 0x01) != 0; }
    bool use_dihedral() const { return (flags & 0x02) != 0; }
    bool extra_scaled() const { return (flags & 0x04) != 0; }
    bool public_mode() const { return (flags & 0x08) != 0; }
    int algorithm() const { return (flags & 0x10) != 0 ? 1 : 0; }
    bool use_audio() const { return (flags & 0x20) != 0; }
    int audio_algorithm() const { return (flags & 0x40) != 0 ? 1 : 0; }
    int audio_version() const { return (flags & 0x80) != 0 ? 2 : 1; }
};

uint16_t crc16_ccitt(const uint8_t* data, size_t size);

std::vector<int> build_metadata_bits(
    const Layout& layout,
    uint32_t key_id,
    const TransportOptions& options);

std::optional<Metadata> parse_metadata(const std::vector<int>& bits);

// Rebuild the transport geometry described by validated metadata. The caller
// is responsible for choosing the key text.
std::optional<Layout> layout_from_metadata(const Metadata& metadata);

}  // namespace obs
