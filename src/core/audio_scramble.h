#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace obs {

// Audio scrambling algorithms. Both are time-invariant analytic single-sideband
// (SSB) subband transforms: they do not depend on frame boundaries, so a
// decoder can start at any sample offset without a synchronisation channel.
enum class AudioAlgorithm : int {
    SubbandShift = 0,    // strong: permute 4 subbands and invert each spectrum
    MirrorInversion = 1, // high fidelity: invert the whole passband once
};

const char* audio_algorithm_id(int algorithm);
int audio_algorithm_from_id(const std::string& id);

struct AudioScrambleParams {
    int algorithm = 0;
    int band_count = 0;
    std::array<int, 4> perm{{0, 1, 2, 3}};       // destination -> source
    std::array<uint8_t, 4> invert{{1, 1, 1, 1}}; // destination -> invert flag
};

// Derive the subband permutation and inversion flags from the transport key.
AudioScrambleParams derive_audio_params(const std::string& key, int algorithm);

// Build the decoder-side parameters (inverse permutation and matching flags).
AudioScrambleParams inverse_audio_params(const AudioScrambleParams& params);

// Stereo 48 kHz streaming audio scrambler. The transform is sample-invariant;
// state is kept across calls. Input and output are interleaved float stereo.
class AudioScrambler {
public:
    AudioScrambler();
    ~AudioScrambler();
    AudioScrambler(const AudioScrambler&) = delete;
    AudioScrambler& operator=(const AudioScrambler&) = delete;

    bool configure(int sample_rate, const std::string& key, int algorithm,
                   std::string* error);
    bool configure(int sample_rate, const AudioScrambleParams& params,
                   std::string* error);
    void reset();

    void process(const float* input, float* output, int frames);

    int algorithm() const { return params_.algorithm; }
    int sample_rate() const { return sample_rate_; }
    int latency_samples() const;

private:
    struct Channel {
        std::vector<float> xbuf;   // duplicated ring for the band filter
        std::vector<float> ibuf[4];
        std::vector<float> qbuf[4];
        int xhead = 0;
        int ihead[4] = {0, 0, 0, 0};
        int qhead[4] = {0, 0, 0, 0};
    };

    void reset_channel(Channel* channel);
    float fir(const std::vector<float>& history, int head,
              const std::vector<float>& coeffs) const;

    int sample_rate_ = 48000;
    int fir_len_ = 0;
    int band_count_ = 0;
    AudioScrambleParams params_;
    std::vector<float> h_band_;
    std::vector<float> h_sub_[4];
    std::array<double, 4> w_src_{};
    std::array<double, 4> w_dst_{};
    std::array<double, 4> src_cos_{};
    std::array<double, 4> src_sin_{};
    std::array<double, 4> dst_cos_{};
    std::array<double, 4> dst_sin_{};
    std::array<double, 4> src_step_c_{};
    std::array<double, 4> src_step_s_{};
    std::array<double, 4> dst_step_c_{};
    std::array<double, 4> dst_step_s_{};
    Channel channels_[2];
    uint64_t phase_tick_ = 0;
};

}  // namespace obs
