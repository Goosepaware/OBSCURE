#include "core/metadata.h"

#include <algorithm>
#include <cassert>

namespace obs {

namespace {

class BitWriter {
public:
    void write(uint32_t value, int width) {
        for (int i = width - 1; i >= 0; --i) {
            bits_.push_back(static_cast<int>((value >> i) & 1u));
        }
    }

    const std::vector<int>& bits() const { return bits_; }

    std::vector<uint8_t> bytes() const {
        std::vector<int> padded = bits_;
        while (padded.size() % 8 != 0) {
            padded.push_back(1);
        }
        std::vector<uint8_t> out;
        out.reserve(padded.size() / 8);
        for (size_t i = 0; i < padded.size(); i += 8) {
            uint8_t b = 0;
            for (size_t j = 0; j < 8; ++j) {
                b = static_cast<uint8_t>((b << 1) | padded[i + j]);
            }
            out.push_back(b);
        }
        return out;
    }

private:
    std::vector<int> bits_;
};

uint32_t read_bits(const std::vector<int>& bits, size_t& pos, int width) {
    uint32_t v = 0;
    for (int i = 0; i < width; ++i) {
        v = (v << 1) | static_cast<uint32_t>(bits[pos++]);
    }
    return v;
}

bool valid_geometry(const Metadata& m) {
    if (m.canvas_w < 320 || m.canvas_w > 4096) return false;
    if (m.canvas_h < 180 || m.canvas_h > 2304) return false;
    if (m.content_w <= 0 || m.content_h <= 0) return false;
    if (m.content_x < 0 || m.content_y < 0) return false;
    if (m.content_x + m.content_w > m.canvas_w) return false;
    if (m.content_y + m.content_h > m.canvas_h) return false;
    if (m.cell < 8 || m.cell > 128) return false;
    if (m.guard < 0 || m.guard > 8) return false;
    if (m.cell - 2 * m.guard < 8) return false;
    if (m.content_w % m.cell != 0 || m.content_h % m.cell != 0) return false;
    if (m.cols <= 0 || m.rows <= 0) return false;
    if (m.cols != m.content_w / m.cell || m.rows != m.content_h / m.cell) {
        return false;
    }
    if (m.inner != m.cell - 2 * m.guard) return false;
    return true;
}

}  // namespace

uint16_t crc16_ccitt(const uint8_t* data, size_t size) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

std::vector<int> build_metadata_bits(
    const Layout& layout,
    uint32_t key_id,
    const TransportOptions& options) {
    int flags = 0;
    if (options.use_color) flags |= 0x01;
    if (options.use_dihedral) flags |= 0x02;
    if (options.scale_x10 > 10) flags |= 0x04;
    if (options.public_mode) flags |= 0x08;
    if (options.algorithm == 1) flags |= 0x10;

    BitWriter bw;
    bw.write(0xB5C3, 16);
    bw.write(1, 8);
    bw.write(static_cast<uint32_t>(layout.canvas_w), 12);
    bw.write(static_cast<uint32_t>(layout.canvas_h), 12);
    bw.write(static_cast<uint32_t>(layout.content_x), 12);
    bw.write(static_cast<uint32_t>(layout.content_y), 12);
    bw.write(static_cast<uint32_t>(layout.content_w), 12);
    bw.write(static_cast<uint32_t>(layout.content_h), 12);
    bw.write(static_cast<uint32_t>(layout.cell), 8);
    bw.write(static_cast<uint32_t>(layout.guard), 8);
    bw.write(static_cast<uint32_t>(layout.cols()), 8);
    bw.write(static_cast<uint32_t>(layout.rows()), 8);
    bw.write(key_id, 32);
    bw.write(static_cast<uint32_t>(flags), 8);
    bw.write(static_cast<uint32_t>(layout.inner()), 8);
    bw.write(static_cast<uint32_t>(std::clamp(options.preset_id, 0, 255)), 8);
    bw.write(static_cast<uint32_t>(std::clamp(options.scale_x10, 0, 255)), 8);

    const auto payload = bw.bytes();
    assert(payload.size() == 24);
    const uint16_t crc = crc16_ccitt(payload.data(), payload.size());

    BitWriter full;
    for (int b : bw.bits()) full.write(static_cast<uint32_t>(b), 1);
    full.write(crc, 16);
    assert(static_cast<int>(full.bits().size()) == kMetaPacketBits);

    std::vector<int> out;
    out.reserve(static_cast<size_t>(kMetaCols * kMetaRows));
    for (int copy = 0; copy < kMetaCopies; ++copy) {
        out.insert(out.end(), full.bits().begin(), full.bits().end());
    }
    out.resize(static_cast<size_t>(kMetaCols * kMetaRows), 0);
    return out;
}

std::optional<Metadata> parse_metadata(const std::vector<int>& bits) {
    if (static_cast<int>(bits.size()) != kMetaPacketBits) {
        return std::nullopt;
    }
    size_t pos = 0;
    Metadata m;
    const uint32_t magic = read_bits(bits, pos, 16);
    m.version = static_cast<int>(read_bits(bits, pos, 8));
    m.canvas_w = static_cast<int>(read_bits(bits, pos, 12));
    m.canvas_h = static_cast<int>(read_bits(bits, pos, 12));
    m.content_x = static_cast<int>(read_bits(bits, pos, 12));
    m.content_y = static_cast<int>(read_bits(bits, pos, 12));
    m.content_w = static_cast<int>(read_bits(bits, pos, 12));
    m.content_h = static_cast<int>(read_bits(bits, pos, 12));
    m.cell = static_cast<int>(read_bits(bits, pos, 8));
    m.guard = static_cast<int>(read_bits(bits, pos, 8));
    m.cols = static_cast<int>(read_bits(bits, pos, 8));
    m.rows = static_cast<int>(read_bits(bits, pos, 8));
    m.key_id = read_bits(bits, pos, 32);
    m.flags = static_cast<int>(read_bits(bits, pos, 8));
    m.inner = static_cast<int>(read_bits(bits, pos, 8));
    m.preset_id = static_cast<int>(read_bits(bits, pos, 8));
    m.scale_x10 = static_cast<int>(read_bits(bits, pos, 8));
    m.crc = static_cast<uint16_t>(read_bits(bits, pos, 16));

    if (magic != 0xB5C3 || m.version != 1) {
        return std::nullopt;
    }
    if (!valid_geometry(m)) {
        return std::nullopt;
    }

    std::vector<uint8_t> payload(24, 0);
    for (int i = 0; i < 192; i += 8) {
        uint8_t b = 0;
        for (int j = 0; j < 8; ++j) {
            b = static_cast<uint8_t>((b << 1) | bits[static_cast<size_t>(i + j)]);
        }
        payload[static_cast<size_t>(i / 8)] = b;
    }
    if (crc16_ccitt(payload.data(), payload.size()) != m.crc) {
        return std::nullopt;
    }
    return m;
}

std::optional<Layout> layout_from_metadata(const Metadata& metadata) {
    if (!valid_geometry(metadata)) return std::nullopt;
    Layout layout;
    layout.canvas_w = metadata.canvas_w;
    layout.canvas_h = metadata.canvas_h;
    layout.content_x = metadata.content_x;
    layout.content_y = metadata.content_y;
    layout.content_w = metadata.content_w;
    layout.content_h = metadata.content_h;
    layout.cell = metadata.cell;
    layout.guard = metadata.guard;
    layout.algorithm = metadata.algorithm();
    return layout;
}

}  // namespace obs
