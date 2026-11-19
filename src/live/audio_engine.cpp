#include "live/audio_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include "core/audio_scramble.h"
#include "platform/wasapi_audio.h"
#include "platform/wav_reader.h"
#include "platform/wav_writer.h"

namespace obs {

struct AudioEngine::Impl {
    AudioEngineConfig config;
    std::atomic<bool> running{false};
    std::atomic<bool> playing{true};
    std::atomic<bool> loop{true};
    std::atomic<double> target_ms{-1.0};
    std::atomic<float> volume{1.0f};
    std::atomic<double> engine_ms{0.0};

    std::unique_ptr<WavReader> reader;
    std::unique_ptr<WavWriter> writer;
    std::unique_ptr<WasapiRender> render;
    std::unique_ptr<WasapiLoopback> loopback;
    std::thread worker;
    std::mutex writer_mutex;
    AudioScrambler scrambler;
    std::vector<float> in_buf;
    std::vector<float> out_buf;

    void apply_volume(float* data, int frames) {
        const float v = volume.load();
        if (std::abs(v - 1.0f) < 1e-4f) return;
        for (int i = 0; i < frames * 2; ++i) data[i] *= v;
    }

    void deliver(const float* data, int frames) {
        if (frames <= 0) return;
        if (render && render->active()) render->write(data, frames);
        if (writer) {
            std::lock_guard lock(writer_mutex);
            writer->write(data, frames);
        }
    }

    void process_file() {
        constexpr int kBlock = 1024;
        in_buf.assign(static_cast<size_t>(kBlock) * 2, 0.0f);
        out_buf.assign(static_cast<size_t>(kBlock) * 2, 0.0f);
        auto next = std::chrono::steady_clock::now();
        while (running.load()) {
            if (!playing.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                next = std::chrono::steady_clock::now();
                continue;
            }
            const double target = target_ms.load();
            if (target >= 0.0) {
                const double engine = engine_ms.load();
                if (std::abs(target - engine) > 80.0) {
                    const int64_t frame = static_cast<int64_t>(target * 48.0);
                    if (reader->seek(frame)) engine_ms = target;
                    next = std::chrono::steady_clock::now();
                }
            }
            const int got = reader->read(in_buf.data(), kBlock);
            if (got <= 0) {
                if (loop.load()) {
                    reader->seek(0);
                    engine_ms = 0.0;
                    next = std::chrono::steady_clock::now();
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            scrambler.process(in_buf.data(), out_buf.data(), got);
            apply_volume(out_buf.data(), got);
            deliver(out_buf.data(), got);
            engine_ms = engine_ms.load() + static_cast<double>(got) / 48.0;

            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(static_cast<double>(got) / 48000.0));
            const auto now = std::chrono::steady_clock::now();
            if (next > now) {
                std::this_thread::sleep_until(next);
            } else if (now - next > std::chrono::milliseconds(120)) {
                next = now;
            }
        }
    }

    static void loopback_callback(void* user, const float* data, int frames) {
        auto* self = static_cast<Impl*>(user);
        if (!self || !self->running.load() || frames <= 0) return;
        self->in_buf.resize(static_cast<size_t>(frames) * 2);
        self->out_buf.resize(static_cast<size_t>(frames) * 2);
        std::copy(data, data + static_cast<size_t>(frames) * 2,
                  self->in_buf.begin());
        self->scrambler.process(self->in_buf.data(), self->out_buf.data(), frames);
        self->apply_volume(self->out_buf.data(), frames);
        self->deliver(self->out_buf.data(), frames);
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::start(const AudioEngineConfig& config, std::string* error) {
    stop();
    impl_->config = config;
    impl_->volume = config.volume;
    impl_->playing = true;
    impl_->loop = true;
    impl_->target_ms = -1.0;
    impl_->engine_ms = 0.0;

    AudioScrambleParams params =
        derive_audio_params(config.key, config.algorithm);
    if (config.decode_mode) params = inverse_audio_params(params);
    if (!impl_->scrambler.configure(48000, params, error)) return false;

    if (config.output_device_id != L"none") {
        impl_->render = std::make_unique<WasapiRender>();
        if (!impl_->render->open(config.output_device_id, 48000, error)) {
            if (error && error->empty()) *error = "WASAPI render open failed";
            impl_->render.reset();
            return false;
        }
    }
    if (!config.record_wav.empty()) {
        impl_->writer = std::make_unique<WavWriter>();
        if (!impl_->writer->open(config.record_wav, 48000, 2, error)) {
            if (impl_->render) impl_->render->close();
            impl_->render.reset();
            impl_->writer.reset();
            return false;
        }
    }

    impl_->running = true;
    if (!config.source_wav.empty()) {
        impl_->reader = std::make_unique<WavReader>();
        if (!impl_->reader->open(config.source_wav, error)) {
            if (error && error->empty()) *error = "wav reader open failed";
            impl_->running = false;
            if (impl_->render) impl_->render->close();
            impl_->render.reset();
            if (impl_->writer) impl_->writer->close();
            impl_->writer.reset();
            impl_->reader.reset();
            return false;
        }
        impl_->worker = std::thread([this] { impl_->process_file(); });
        return true;
    }

    impl_->loopback = std::make_unique<WasapiLoopback>();
    impl_->loopback->set_callback(&Impl::loopback_callback, impl_.get());
    if (!impl_->loopback->open(48000, error)) {
        impl_->running = false;
        if (impl_->render) impl_->render->close();
        impl_->render.reset();
        if (impl_->writer) impl_->writer->close();
        impl_->writer.reset();
        impl_->loopback.reset();
        return false;
    }
    return true;
}

void AudioEngine::stop() {
    if (!impl_) return;
    impl_->running = false;
    if (impl_->loopback) {
        impl_->loopback->close();
        impl_->loopback.reset();
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->reader) impl_->reader.reset();
    if (impl_->render) {
        impl_->render->close();
        impl_->render.reset();
    }
    if (impl_->writer) {
        std::lock_guard lock(impl_->writer_mutex);
        impl_->writer->close();
        impl_->writer.reset();
    }
    impl_->in_buf.clear();
    impl_->out_buf.clear();
}

void AudioEngine::set_position_ms(double ms) {
    if (impl_) impl_->target_ms = ms;
}

void AudioEngine::set_playing(bool playing) {
    if (impl_) impl_->playing = playing;
}

void AudioEngine::set_loop(bool loop) {
    if (impl_) impl_->loop = loop;
}

void AudioEngine::set_volume(float volume) {
    if (impl_) impl_->volume = std::clamp(volume, 0.0f, 2.0f);
}

bool AudioEngine::active() const {
    return impl_ && impl_->running.load();
}

bool AudioEngine::using_loopback() const {
    return impl_ && impl_->loopback && impl_->loopback->active();
}

std::wstring AudioEngine::record_wav() const {
    return impl_ ? impl_->config.record_wav : std::wstring();
}

}  // namespace obs
