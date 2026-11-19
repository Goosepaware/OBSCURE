#include "platform/wav_writer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

namespace obs {

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

}  // namespace

WavWriter::~WavWriter() {
    close();
}

bool WavWriter::open(const std::wstring& path, int sample_rate, int channels,
                     std::string* error) {
    close();
    if (sample_rate <= 0 || channels <= 0) {
        if (error) *error = "invalid wav parameters";
        return false;
    }
    channels_ = channels;
    data_bytes_ = 0;
    file_.open(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!file_) {
        if (error) *error = "cannot create wav file";
        return false;
    }
    const uint16_t block_align = static_cast<uint16_t>(channels_ * 4);
    std::vector<uint8_t> header;
    header.reserve(44);
    const char riff[] = "RIFF";
    header.insert(header.end(), riff, riff + 4);
    put32(header, 36);
    const char wave[] = "WAVEfmt ";
    header.insert(header.end(), wave, wave + 8);
    put32(header, 16);
    put16(header, 3);  // IEEE float
    put16(header, static_cast<uint16_t>(channels_));
    put32(header, static_cast<uint32_t>(sample_rate));
    put32(header, static_cast<uint32_t>(sample_rate) * block_align);
    put16(header, block_align);
    put16(header, 32);
    const char data[] = "data";
    header.insert(header.end(), data, data + 4);
    put32(header, 0);
    file_.write(reinterpret_cast<const char*>(header.data()),
                static_cast<std::streamsize>(header.size()));
    return true;
}

bool WavWriter::write(const float* interleaved_stereo, int frames) {
    if (!file_ || !interleaved_stereo || frames <= 0) return false;
    const size_t samples = static_cast<size_t>(frames) * channels_;
    file_.write(reinterpret_cast<const char*>(interleaved_stereo),
                static_cast<std::streamsize>(samples * sizeof(float)));
    if (!file_) return false;
    data_bytes_ += samples * sizeof(float);
    return true;
}

bool WavWriter::close() {
    if (!file_.is_open()) return true;
    const uint64_t data_bytes = data_bytes_;
    const uint32_t riff_size =
        static_cast<uint32_t>(std::min<uint64_t>(36 + data_bytes, 0xFFFFFFFFu));
    const uint32_t data_size = static_cast<uint32_t>(
        std::min<uint64_t>(data_bytes, 0xFFFFFFFFu));
    file_.seekp(4);
    std::vector<uint8_t> tmp;
    put32(tmp, riff_size);
    file_.write(reinterpret_cast<const char*>(tmp.data()), 4);
    tmp.clear();
    file_.seekp(40);
    put32(tmp, data_size);
    file_.write(reinterpret_cast<const char*>(tmp.data()), 4);
    file_.close();
    return true;
}

}  // namespace obs
