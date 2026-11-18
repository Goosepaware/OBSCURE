#include "core/metadata.h"

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
    int flags,
    bool use_color,
    bool use_dihedral,
    double extra_scale) {
    flags &= 0xFF;
    if (use_color) flags |= 0x01;
    if (use_dihedral) flags |= 0x02;
    if (extra_scale > 1.0 + 1e-6) flags |= 0x04;

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
    bw.write(0, 16);  // reserved

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
    m.reserved = static_cast<int>(read_bits(bits, pos, 16));
    m.crc = static_cast<uint16_t>(read_bits(bits, pos, 16));

    if (magic != 0xB5C3 || m.version != 1) {
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

}  // namespace obs
