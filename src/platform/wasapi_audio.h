#pragma once

#include <string>
#include <vector>

namespace obs {

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
};

// Active WASAPI render endpoints (no synthetic "default"/"none" entries).
std::vector<AudioDeviceInfo> list_audio_render_devices();

// Event-driven WASAPI renderer for 48 kHz stereo float. write() is
// non-blocking and drops the oldest samples if the caller gets ahead.
class WasapiRender {
public:
    WasapiRender() = default;
    ~WasapiRender();
    WasapiRender(const WasapiRender&) = delete;
    WasapiRender& operator=(const WasapiRender&) = delete;

    bool open(const std::wstring& device_id, int sample_rate, std::string* error);
    void close();
    bool write(const float* interleaved_stereo, int frames);
    bool active() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// WASAPI loopback capture of the default render endpoint. The callback runs on
// the capture thread and receives interleaved stereo float samples.
class WasapiLoopback {
public:
    using Callback = void (*)(void* user, const float* interleaved_stereo, int frames);

    WasapiLoopback() = default;
    ~WasapiLoopback();
    WasapiLoopback(const WasapiLoopback&) = delete;
    WasapiLoopback& operator=(const WasapiLoopback&) = delete;

    bool open(int sample_rate, std::string* error);
    void close();
    void set_callback(Callback callback, void* user);
    bool active() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace obs
