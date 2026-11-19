#include "platform/wav_reader.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

namespace obs {

namespace {

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

WavReader::~WavReader() {
    close();
}

bool WavReader::open(const std::wstring& path, std::string* error) {
    close();
    file_.open(std::filesystem::path(path), std::ios::binary);
    if (!file_) {
        if (error) *error = "cannot open wav";
        return false;
    }
    uint8_t header[12]{};
    file_.read(reinterpret_cast<char*>(header), sizeof(header));
    if (file_.gcount() != 12 || std::memcmp(header, "RIFF", 4) != 0 ||
        std::memcmp(header + 8, "WAVE", 4) != 0) {
        if (error) *error = "not a riff/wave file";
        close();
        return false;
    }

    bool have_fmt = false;
    bool have_data = false;
    while (file_) {
        uint8_t chunk[8]{};
        file_.read(reinterpret_cast<char*>(chunk), sizeof(chunk));
        if (file_.gcount() != 8) break;
        const uint32_t size = rd32(chunk + 4);
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            std::vector<uint8_t> fmt(size);
            file_.read(reinterpret_cast<char*>(fmt.data()), size);
            if (file_.gcount() != static_cast<std::streamsize>(size) || size < 16) {
                break;
            }
            uint16_t format = rd16(fmt.data());
            channels_ = rd16(fmt.data() + 2);
            sample_rate_ = static_cast<int>(rd32(fmt.data() + 4));
            block_align_ = rd16(fmt.data() + 12);
            bits_ = rd16(fmt.data() + 14);
            if (format == 0xFFFE && size >= 40) {
                const uint16_t sub = rd16(fmt.data() + 24);
                format = sub;
            }
            is_float_ = format == 3;
            have_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data_offset_ = static_cast<int64_t>(file_.tellg());
            data_size_ = size;
            have_data = true;
            file_.seekg(static_cast<std::streamoff>(size), std::ios::cur);
        } else {
            file_.seekg(static_cast<std::streamoff>(size + (size & 1)),
                        std::ios::cur);
        }
    }
    if (!have_fmt || !have_data || channels_ < 1 || sample_rate_ <= 0 ||
        block_align_ == 0 || (bits_ != 16 && bits_ != 32)) {
        if (error) *error = "unsupported wav format";
        close();
        return false;
    }
    total_frames_ = data_size_ / block_align_;
    position_ = 0;
    if (!seek(0)) {
        if (error) *error = "wav data seek failed";
        close();
        return false;
    }
    return true;
}

void WavReader::close() {
    if (file_.is_open()) file_.close();
    sample_rate_ = 0;
    channels_ = 0;
    bits_ = 0;
    is_float_ = false;
    block_align_ = 0;
    data_offset_ = 0;
    data_size_ = 0;
    total_frames_ = 0;
    position_ = 0;
}

int WavReader::read(float* out_stereo, int frames) {
    if (!file_ || !out_stereo || frames <= 0) return 0;
    if (position_ >= total_frames_) return 0;
    const int want = static_cast<int>(
        std::min<int64_t>(frames, total_frames_ - position_));
    std::vector<uint8_t> raw(static_cast<size_t>(want) * block_align_);
    file_.read(reinterpret_cast<char*>(raw.data()),
               static_cast<std::streamsize>(raw.size()));
    const int got = static_cast<int>(file_.gcount() / block_align_);
    if (got <= 0) return 0;

    for (int i = 0; i < got; ++i) {
        const uint8_t* src = raw.data() + static_cast<size_t>(i) * block_align_;
        float l = 0.0f;
        float r = 0.0f;
        if (is_float_) {
            l = *reinterpret_cast<const float*>(src);
            r = channels_ >= 2 ? *reinterpret_cast<const float*>(src + 4) : l;
        } else {
            const int16_t sl = *reinterpret_cast<const int16_t*>(src);
            const int16_t sr =
                channels_ >= 2 ? *reinterpret_cast<const int16_t*>(src + 2) : sl;
            l = static_cast<float>(sl) / 32768.0f;
            r = static_cast<float>(sr) / 32768.0f;
        }
        out_stereo[static_cast<size_t>(i) * 2] = l;
        out_stereo[static_cast<size_t>(i) * 2 + 1] = r;
    }
    position_ += got;
    return got;
}

bool WavReader::seek(int64_t frame) {
    if (!file_.is_open()) return false;
    file_.clear();
    frame = std::clamp<int64_t>(frame, 0, total_frames_);
    file_.seekg(static_cast<std::streamoff>(data_offset_ + frame * block_align_));
    if (!file_) return false;
    position_ = frame;
    return true;
}

}  // namespace obs
