#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace obs {

// Streaming 48 kHz stereo float WAV writer used for recording/export audio.
class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool open(const std::wstring& path, int sample_rate, int channels,
              std::string* error);
    bool write(const float* interleaved_stereo, int frames);
    bool close();
    bool is_open() const { return file_.is_open(); }

private:
    std::ofstream file_;
    int channels_ = 2;
    uint64_t data_bytes_ = 0;
};

}  // namespace obs
