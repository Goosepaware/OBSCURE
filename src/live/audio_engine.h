#pragma once

#include <memory>
#include <string>

namespace obs {

struct AudioEngineConfig {
    bool decode_mode = false;
    int algorithm = 0;
    std::string key;
    std::wstring output_device_id;  // empty = system default, L"none" = muted
    float volume = 1.0f;
    std::wstring source_wav;        // empty = WASAPI loopback capture
    std::wstring record_wav;        // optional processed-audio recording
};

// Runs the audio pipeline on a worker thread (file source) or on the WASAPI
// loopback capture thread (live source). The scrambler is time-invariant, so
// the decoder can start at any position without a sync channel.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool start(const AudioEngineConfig& config, std::string* error);
    void stop();

    void set_position_ms(double ms);
    void set_playing(bool playing);
    void set_loop(bool loop);
    void set_volume(float volume);

    bool active() const;
    bool using_loopback() const;
    std::wstring record_wav() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace obs
