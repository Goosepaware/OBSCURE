#include "core/audio_scramble.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

#include <windows.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include "core/key.h"

namespace obs {

namespace {

constexpr int kBaseRate = 48000;
constexpr double kPi = 3.14159265358979323846;

constexpr double kStrongEdges[5] = {150.0, 1950.0, 3750.0, 5550.0, 7350.0};
constexpr double kMirrorEdges[2] = {150.0, 7350.0};

std::vector<float> design_lowpass(double cutoff_hz, int sample_rate, int length) {
    std::vector<float> h(static_cast<size_t>(length));
    const double fc = cutoff_hz / static_cast<double>(sample_rate);
    const int mid = (length - 1) / 2;
    constexpr double beta = 9.0;
    const double i0_beta = [] {
        double sum = 1.0;
        double term = 1.0;
        for (int k = 1; k < 32; ++k) {
            term *= (beta / (2.0 * k)) * (beta / (2.0 * k));
            sum += term;
        }
        return sum;
    }();
    auto i0 = [](double x) {
        double sum = 1.0;
        double term = 1.0;
        for (int k = 1; k < 32; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    double sum = 0.0;
    for (int i = 0; i < length; ++i) {
        const double x = 2.0 * fc * static_cast<double>(i - mid);
        const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double t = 2.0 * static_cast<double>(i) / (length - 1) - 1.0;
        const double kaiser = i0(beta * std::sqrt(std::max(0.0, 1.0 - t * t))) / i0_beta;
        const double v = 2.0 * fc * sinc * kaiser;
        h[static_cast<size_t>(i)] = static_cast<float>(v);
        sum += v;
    }
    if (std::abs(sum) > 1e-12) {
        for (float& v : h) v = static_cast<float>(v / sum);
    }
    return h;
}

}  // namespace

const char* audio_algorithm_id(int algorithm) {
    return algorithm == static_cast<int>(AudioAlgorithm::MirrorInversion)
               ? "mirror"
               : "subband";
}

int audio_algorithm_from_id(const std::string& id) {
    if (id == "mirror" || id == "inversion" || id == "hifi" || id == "m") {
        return static_cast<int>(AudioAlgorithm::MirrorInversion);
    }
    return static_cast<int>(AudioAlgorithm::SubbandShift);
}

AudioScrambleParams derive_audio_params(const std::string& key, int algorithm) {
    AudioScrambleParams params;
    params.algorithm = algorithm;
    if (algorithm == static_cast<int>(AudioAlgorithm::MirrorInversion)) {
        params.band_count = 1;
        params.perm = {{0, 0, 0, 0}};
        params.invert = {{1, 0, 0, 0}};
        return params;
    }

    params.band_count = 4;
    params.perm = {{0, 1, 2, 3}};
    const auto seed = sha256("OBSCure-audio-v1|" + key + "|subband");
    uint64_t seed64 = 0;
    for (int i = 0; i < 8; ++i) {
        seed64 |= static_cast<uint64_t>(seed[static_cast<size_t>(i)]) << (8 * i);
    }
    std::mt19937_64 rng(seed64);
    for (int i = 3; i > 0; --i) {
        const uint64_t bound = static_cast<uint64_t>(i) + 1;
        const uint64_t limit = UINT64_MAX - (UINT64_MAX % bound);
        uint64_t r = 0;
        do {
            r = rng();
        } while (r >= limit);
        const int j = static_cast<int>(r % bound);
        std::swap(params.perm[static_cast<size_t>(i)],
                  params.perm[static_cast<size_t>(j)]);
    }
    // A fixed point would leave one band in place; rotate until every band
    // actually moves. Four elements always allow a derangement.
    for (int attempt = 0; attempt < 4; ++attempt) {
        bool fixed = false;
        for (int i = 0; i < 4; ++i) {
            if (params.perm[static_cast<size_t>(i)] == i) {
                fixed = true;
                break;
            }
        }
        if (!fixed) break;
        std::rotate(params.perm.begin(), params.perm.begin() + 1, params.perm.end());
    }
    params.invert = {{1, 1, 1, 1}};
    return params;
}

AudioScrambleParams inverse_audio_params(const AudioScrambleParams& params) {
    AudioScrambleParams inv;
    inv.algorithm = params.algorithm;
    inv.band_count = params.band_count;
    inv.perm = {{0, 0, 0, 0}};
    inv.invert = {{0, 0, 0, 0}};
    for (int d = 0; d < params.band_count; ++d) {
        const int s = params.perm[static_cast<size_t>(d)];
        inv.perm[static_cast<size_t>(s)] = d;
        inv.invert[static_cast<size_t>(s)] = params.invert[static_cast<size_t>(d)];
    }
    return inv;
}

AudioScrambler::AudioScrambler() = default;
AudioScrambler::~AudioScrambler() = default;

int AudioScrambler::latency_samples() const {
    return fir_len_ > 0 ? (fir_len_ - 1) / 2 : 0;
}

void AudioScrambler::reset_channel(Channel* channel) {
    if (!channel) return;
    std::fill(channel->xbuf.begin(), channel->xbuf.end(), 0.0f);
    for (int b = 0; b < 4; ++b) {
        std::fill(channel->ibuf[b].begin(), channel->ibuf[b].end(), 0.0f);
        std::fill(channel->qbuf[b].begin(), channel->qbuf[b].end(), 0.0f);
        channel->ihead[b] = 0;
        channel->qhead[b] = 0;
    }
    channel->xhead = 0;
}

bool AudioScrambler::configure(int sample_rate, const std::string& key,
                               int algorithm, std::string* error) {
    return configure(sample_rate, derive_audio_params(key, algorithm), error);
}

bool AudioScrambler::configure(int sample_rate, const AudioScrambleParams& params,
                               std::string* error) {
    if (sample_rate < 16000 || sample_rate > 192000) {
        if (error) *error = "unsupported audio sample rate";
        return false;
    }
    sample_rate_ = sample_rate;
    params_ = params;
    band_count_ = params_.band_count;
    if (band_count_ < 1 || band_count_ > 4) {
        if (error) *error = "invalid audio band count";
        return false;
    }
    const int algorithm = params_.algorithm;
    fir_len_ = algorithm == static_cast<int>(AudioAlgorithm::MirrorInversion) ? 2049
                                                                            : 1025;

    const double scale = static_cast<double>(sample_rate_) / kBaseRate;
    std::vector<float> low = design_lowpass(kStrongEdges[0] * scale, sample_rate_,
                                            fir_len_);
    std::vector<float> high = design_lowpass(kStrongEdges[4] * scale, sample_rate_,
                                             fir_len_);
    h_band_ = high;
    for (int i = 0; i < fir_len_; ++i) {
        h_band_[static_cast<size_t>(i)] -= low[static_cast<size_t>(i)];
    }

    const double sub_cut = algorithm == static_cast<int>(AudioAlgorithm::MirrorInversion)
                               ? 0.5 * (kMirrorEdges[1] - kMirrorEdges[0])
                               : 0.5 * (kStrongEdges[1] - kStrongEdges[0]);
    std::vector<float> sub = design_lowpass(sub_cut * scale, sample_rate_, fir_len_);
    for (int d = 0; d < band_count_; ++d) {
        h_sub_[d] = sub;
    }

    std::array<double, 4> centers{};
    if (algorithm == static_cast<int>(AudioAlgorithm::MirrorInversion)) {
        centers[0] = 0.5 * (kMirrorEdges[0] + kMirrorEdges[1]) * scale;
    } else {
        for (int b = 0; b < 4; ++b) {
            centers[static_cast<size_t>(b)] =
                0.5 * (kStrongEdges[b] + kStrongEdges[b + 1]) * scale;
        }
    }

    for (int d = 0; d < band_count_; ++d) {
        const int s = params_.perm[static_cast<size_t>(d)];
        w_src_[static_cast<size_t>(d)] =
            2.0 * kPi * centers[static_cast<size_t>(s)] / sample_rate_;
        w_dst_[static_cast<size_t>(d)] =
            2.0 * kPi * centers[static_cast<size_t>(d)] / sample_rate_;
    }
    for (int i = 0; i < 4; ++i) {
        src_cos_[static_cast<size_t>(i)] = 1.0;
        src_sin_[static_cast<size_t>(i)] = 0.0;
        const double off = -w_dst_[static_cast<size_t>(i)] * ((fir_len_ - 1) / 2);
        dst_cos_[static_cast<size_t>(i)] = std::cos(off);
        dst_sin_[static_cast<size_t>(i)] = std::sin(off);
        src_step_c_[static_cast<size_t>(i)] = std::cos(w_src_[static_cast<size_t>(i)]);
        src_step_s_[static_cast<size_t>(i)] = std::sin(w_src_[static_cast<size_t>(i)]);
        dst_step_c_[static_cast<size_t>(i)] = std::cos(w_dst_[static_cast<size_t>(i)]);
        dst_step_s_[static_cast<size_t>(i)] = std::sin(w_dst_[static_cast<size_t>(i)]);
    }

    for (auto& channel : channels_) {
        channel.xbuf.assign(static_cast<size_t>(2 * fir_len_), 0.0f);
        for (int b = 0; b < 4; ++b) {
            channel.ibuf[b].assign(static_cast<size_t>(2 * fir_len_), 0.0f);
            channel.qbuf[b].assign(static_cast<size_t>(2 * fir_len_), 0.0f);
        }
    }
    phase_tick_ = 0;
    reset();
    return true;
}

void AudioScrambler::reset() {
    for (auto& channel : channels_) reset_channel(&channel);
    const int delay = (fir_len_ - 1) / 2;
    for (int i = 0; i < 4; ++i) {
        src_cos_[static_cast<size_t>(i)] = 1.0;
        src_sin_[static_cast<size_t>(i)] = 0.0;
        const double off = -w_dst_[static_cast<size_t>(i)] * delay;
        dst_cos_[static_cast<size_t>(i)] = std::cos(off);
        dst_sin_[static_cast<size_t>(i)] = std::sin(off);
    }
    phase_tick_ = 0;
}

float AudioScrambler::fir(const std::vector<float>& history, int head,
                          const std::vector<float>& coeffs) const {
    // history is duplicated: the last L samples live at [head+1, head+L].
    const float* window = history.data() + head + 1;
    const float* h = coeffs.data();
    float acc = 0.0f;
#if defined(__AVX2__)
    __m256 vacc = _mm256_setzero_ps();
    int i = 0;
    for (; i + 8 <= fir_len_; i += 8) {
        vacc = _mm256_fmadd_ps(_mm256_loadu_ps(h + i),
                               _mm256_loadu_ps(window + i), vacc);
    }
    __m128 lo = _mm256_castps256_ps128(vacc);
    __m128 hi = _mm256_extractf128_ps(vacc, 1);
    __m128 sum = _mm_add_ps(lo, hi);
    sum = _mm_hadd_ps(sum, sum);
    sum = _mm_hadd_ps(sum, sum);
    acc = _mm_cvtss_f32(sum);
    for (; i < fir_len_; ++i) acc += h[i] * window[i];
#else
    for (int i = 0; i < fir_len_; ++i) {
        acc += h[i] * window[i];
    }
#endif
    return acc;
}

void AudioScrambler::process(const float* input, float* output, int frames) {
    if (!input || !output || frames <= 0 || fir_len_ <= 0) return;
    const int n = band_count_;
    const int delay = (fir_len_ - 1) / 2;
    for (int frame = 0; frame < frames; ++frame) {
        for (int ch = 0; ch < 2; ++ch) {
            Channel& c = channels_[ch];
            const float x = input[static_cast<size_t>(frame) * 2 + ch];
            c.xhead = (c.xhead + 1) % fir_len_;
            c.xbuf[static_cast<size_t>(c.xhead)] = x;
            c.xbuf[static_cast<size_t>(c.xhead + fir_len_)] = x;
            const float* xw = c.xbuf.data() + c.xhead + 1;
            const float band = fir(c.xbuf, c.xhead, h_band_);
            float y = xw[delay] - band;  // dry and band are both centred

            for (int d = 0; d < n; ++d) {
                const double cs = src_cos_[static_cast<size_t>(d)];
                const double ss = src_sin_[static_cast<size_t>(d)];
                const double cd = dst_cos_[static_cast<size_t>(d)];
                const double sd = dst_sin_[static_cast<size_t>(d)];
                const float i = static_cast<float>(2.0 * x * cs);
                const float q = static_cast<float>(-2.0 * x * ss);
                c.ihead[d] = (c.ihead[d] + 1) % fir_len_;
                c.qhead[d] = (c.qhead[d] + 1) % fir_len_;
                c.ibuf[d][static_cast<size_t>(c.ihead[d])] = i;
                c.ibuf[d][static_cast<size_t>(c.ihead[d] + fir_len_)] = i;
                c.qbuf[d][static_cast<size_t>(c.qhead[d])] = q;
                c.qbuf[d][static_cast<size_t>(c.qhead[d] + fir_len_)] = q;
                float I = fir(c.ibuf[d], c.ihead[d], h_sub_[d]);
                float Q = fir(c.qbuf[d], c.qhead[d], h_sub_[d]);
                if (params_.invert[static_cast<size_t>(d)]) Q = -Q;
                y += static_cast<float>(I * cd - Q * sd);
            }
            output[static_cast<size_t>(frame) * 2 + ch] =
                std::clamp(y, -1.0f, 1.0f);
        }

        for (int d = 0; d < n; ++d) {
            const double c0 = src_cos_[static_cast<size_t>(d)];
            const double s0 = src_sin_[static_cast<size_t>(d)];
            src_cos_[static_cast<size_t>(d)] =
                c0 * src_step_c_[static_cast<size_t>(d)] -
                s0 * src_step_s_[static_cast<size_t>(d)];
            src_sin_[static_cast<size_t>(d)] =
                s0 * src_step_c_[static_cast<size_t>(d)] +
                c0 * src_step_s_[static_cast<size_t>(d)];

            const double c1 = dst_cos_[static_cast<size_t>(d)];
            const double s1 = dst_sin_[static_cast<size_t>(d)];
            dst_cos_[static_cast<size_t>(d)] =
                c1 * dst_step_c_[static_cast<size_t>(d)] -
                s1 * dst_step_s_[static_cast<size_t>(d)];
            dst_sin_[static_cast<size_t>(d)] =
                s1 * dst_step_c_[static_cast<size_t>(d)] +
                c1 * dst_step_s_[static_cast<size_t>(d)];
        }
        ++phase_tick_;
        if ((phase_tick_ & 0xFFFFu) == 0) {
            for (int d = 0; d < n; ++d) {
                const double mag = std::hypot(src_cos_[static_cast<size_t>(d)],
                                              src_sin_[static_cast<size_t>(d)]);
                if (mag > 1e-12) {
                    src_cos_[static_cast<size_t>(d)] /= mag;
                    src_sin_[static_cast<size_t>(d)] /= mag;
                }
                const double dmag = std::hypot(dst_cos_[static_cast<size_t>(d)],
                                               dst_sin_[static_cast<size_t>(d)]);
                if (dmag > 1e-12) {
                    dst_cos_[static_cast<size_t>(d)] /= dmag;
                    dst_sin_[static_cast<size_t>(d)] /= dmag;
                }
            }
        }
    }
}

}  // namespace obs
