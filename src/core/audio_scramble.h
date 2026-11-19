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
    SubbandShift = 0,    // strong: permute narrow subbands and invert each one
    MirrorInversion = 1, // high fidelity: mirrored passband
};

// Transport audio DSP version. Version 1 is the original four-band/one-band
// transform (kept for old recordings). Version 2 adds reversible pre-emphasis,
// a 7350-7800 Hz guard band, 8-band strong scrambling, and -6 dB headroom.
constexpr int kAudioVersionLegacy = 1;
constexpr int kAudioVersionCurrent = 2;

const char* audio_algorithm_id(int algorithm);
int audio_algorithm_from_id(const std::string& id);

struct AudioScrambleParams {
    int algorithm = 0;
    int version = kAudioVersionCurrent;
    bool decoder = false;  // true for the inverse (de-emphasis) side
    int band_count = 0;
    std::array<int, 8> perm{{0, 0, 0, 0, 0, 0, 0, 0}};       // dest -> source
    std::array<uint8_t, 8> invert{{0, 0, 0, 0, 0, 0, 0, 0}}; // dest -> invert
};

// Derive the subband permutation and inversion flags from the transport key.
AudioScrambleParams derive_audio_params(const std::string& key, int algorithm,
                                        int version = kAudioVersionCurrent);

// Build the decoder-side parameters (inverse permutation, matching flags, and
// de-emphasis enabled).
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
    int version() const { return params_.version; }
    int sample_rate() const { return sample_rate_; }
    int latency_samples() const;

private:
    struct Channel {
        std::vector<float> xbuf;  // duplicated ring for the band filters
        std::vector<float> ibuf[8];
        std::vector<float> qbuf[8];
        int xhead = 0;
        int ihead[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        int qhead[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        float pre_x1 = 0.0f;
        float pre_y1 = 0.0f;
        float de_x1 = 0.0f;
        float de_y1 = 0.0f;
    };

    void reset_channel(Channel* channel);
    float fir(const std::vector<float>& history, int head,
              const std::vector<float>& coeffs) const;

    int sample_rate_ = 48000;
    int version_ = kAudioVersionCurrent;
    int fir_len_ = 0;
    int band_count_ = 0;
    AudioScrambleParams params_;
    std::vector<float> h_band_;
    std::vector<float> h_low_;
    std::vector<float> h_high_;
    std::vector<float> h_sub_[8];
    std::array<double, 8> w_src_{};
    std::array<double, 8> w_dst_{};
    std::array<double, 8> src_cos_{};
    std::array<double, 8> src_sin_{};
    std::array<double, 8> dst_cos_{};
    std::array<double, 8> dst_sin_{};
    std::array<double, 8> src_step_c_{};
    std::array<double, 8> src_step_s_{};
    std::array<double, 8> dst_step_c_{};
    std::array<double, 8> dst_step_s_{};
    Channel channels_[2];
    uint64_t phase_tick_ = 0;
};

}  // namespace obs
