#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace obs {

// Minimal streaming WAV reader for 48 kHz stereo float files produced by
// ffmpeg. PCM16 and mono files are converted to stereo float on the fly.
class WavReader {
public:
    WavReader() = default;
    ~WavReader();
    WavReader(const WavReader&) = delete;
    WavReader& operator=(const WavReader&) = delete;

    bool open(const std::wstring& path, std::string* error);
    void close();

    int sample_rate() const { return sample_rate_; }
    int channels() const { return channels_; }
    int64_t total_frames() const { return total_frames_; }
    int64_t position() const { return position_; }

    // Reads interleaved stereo float. Returns the number of frames read.
    int read(float* out_stereo, int frames);
    bool seek(int64_t frame);

private:
    std::ifstream file_;
    int sample_rate_ = 0;
    int channels_ = 0;
    int bits_ = 0;
    bool is_float_ = false;
    uint16_t block_align_ = 0;
    int64_t data_offset_ = 0;
    int64_t data_size_ = 0;
    int64_t total_frames_ = 0;
    int64_t position_ = 0;
};

}  // namespace obs
