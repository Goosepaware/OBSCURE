#include "platform/wasapi_audio.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <wrl/client.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

using Microsoft::WRL::ComPtr;

namespace obs {

namespace {

const PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd,
     {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14};

void com_init() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr;
}

WAVEFORMATEXTENSIBLE make_format(int sample_rate) {
    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = 2;
    wf.Format.nSamplesPerSec = static_cast<DWORD>(sample_rate);
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = static_cast<WORD>(wf.Format.nChannels * 4);
    wf.Format.nAvgBytesPerSec =
        wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
    wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return wf;
}

bool is_float_stereo(const WAVEFORMATEX* fmt, int* sample_rate, int* channels) {
    if (!fmt) return false;
    if (sample_rate) *sample_rate = static_cast<int>(fmt->nSamplesPerSec);
    if (channels) *channels = fmt->nChannels;
    if (fmt->wBitsPerSample != 32) return false;
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return false;
}

std::wstring friendly_name(IMMDevice* device) {
    if (!device) return L"";
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.GetAddressOf()))) {
        return L"";
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(store->GetValue(kPkeyDeviceFriendlyName, &value)) &&
        value.vt == VT_LPWSTR) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

}  // namespace

std::vector<AudioDeviceInfo> list_audio_render_devices() {
    com_init();
    std::vector<AudioDeviceInfo> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(enumerator.GetAddressOf())))) {
        return out;
    }
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                              collection.GetAddressOf()))) {
        return out;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.GetAddressOf()))) continue;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id)) || !id) continue;
        AudioDeviceInfo info;
        info.id = id;
        info.name = friendly_name(device.Get());
        CoTaskMemFree(id);
        if (info.name.empty()) info.name = L"音频设备 " + std::to_wstring(i + 1);
        out.push_back(std::move(info));
    }
    return out;
}

struct WasapiRender::Impl {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    UINT32 buffer_frames = 0;
    std::thread thread;
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::vector<float> ring;
    size_t read_pos = 0;
    size_t write_pos = 0;
    size_t size = 0;
    int sample_rate = 48000;

    void render_loop() {
        com_init();
        while (running.load()) {
            UINT32 padding = 0;
            const HRESULT hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            const UINT32 available = buffer_frames - padding;
            if (available == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            BYTE* data = nullptr;
            const HRESULT gb = render->GetBuffer(available, &data);
            if (FAILED(gb) || !data) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            float* out = reinterpret_cast<float*>(data);
            size_t produced = 0;
            {
                std::lock_guard lock(mutex);
                const size_t want = static_cast<size_t>(available) * 2;
                const size_t take = std::min(want, size);
                for (size_t i = 0; i < take; ++i) {
                    out[i] = ring[read_pos];
                    read_pos = (read_pos + 1) % ring.size();
                }
                size -= take;
                produced = take / 2;
            }
            if (produced < available) {
                std::fill(out + produced * 2, out + static_cast<size_t>(available) * 2,
                          0.0f);
            }
            render->ReleaseBuffer(available, 0);
        }
    }
};

WasapiRender::~WasapiRender() {
    close();
}

bool WasapiRender::open(const std::wstring& device_id, int sample_rate,
                        std::string* error) {
    close();
    com_init();
    impl_ = new Impl();
    impl_->sample_rate = sample_rate;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(enumerator.GetAddressOf())))) {
        if (error) *error = "WASAPI enumerator creation failed";
        close();
        return false;
    }
    ComPtr<IMMDevice> device;
    HRESULT hr = device_id.empty()
                     ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                           device.GetAddressOf())
                     : enumerator->GetDevice(device_id.c_str(),
                                             device.GetAddressOf());
    if (FAILED(hr) || !device) {
        if (error) *error = "WASAPI audio device not found";
        close();
        return false;
    }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(impl_->client.GetAddressOf()));
    if (FAILED(hr)) {
        if (error) *error = "WASAPI audio client activation failed";
        close();
        return false;
    }
    WAVEFORMATEXTENSIBLE wf = make_format(sample_rate);
    hr = impl_->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        200000, 0, &wf.Format, nullptr);
    if (FAILED(hr)) {
        if (error) *error = "WASAPI audio format not supported";
        close();
        return false;
    }
    if (FAILED(impl_->client->GetService(
            IID_PPV_ARGS(impl_->render.GetAddressOf()))) ||
        FAILED(impl_->client->GetBufferSize(&impl_->buffer_frames))) {
        if (error) *error = "WASAPI render client setup failed";
        close();
        return false;
    }
    impl_->ring.assign(static_cast<size_t>(sample_rate) * 2, 0.0f);
    impl_->running = true;
    if (FAILED(impl_->client->Start())) {
        if (error) *error = "WASAPI render start failed";
        impl_->running = false;
        close();
        return false;
    }
    impl_->thread = std::thread([this] { impl_->render_loop(); });
    return true;
}

void WasapiRender::close() {
    if (!impl_) return;
    impl_->running = false;
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->client) impl_->client->Stop();
    delete impl_;
    impl_ = nullptr;
}

bool WasapiRender::write(const float* interleaved_stereo, int frames) {
    if (!impl_ || !interleaved_stereo || frames <= 0) return false;
    std::lock_guard lock(impl_->mutex);
    const size_t cap = impl_->ring.size();
    if (cap == 0) return false;
    for (int i = 0; i < frames; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            if (impl_->size == cap) {
                impl_->read_pos = (impl_->read_pos + 1) % cap;
                --impl_->size;
            }
            impl_->ring[impl_->write_pos] =
                interleaved_stereo[static_cast<size_t>(i) * 2 + ch];
            impl_->write_pos = (impl_->write_pos + 1) % cap;
            ++impl_->size;
        }
    }
    return true;
}

bool WasapiRender::active() const {
    return impl_ && impl_->running.load();
}

struct WasapiLoopback::Impl {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    UINT32 buffer_frames = 0;
    std::thread thread;
    std::atomic<bool> running{false};
    int sample_rate = 48000;
    int channels = 2;
    Callback callback = nullptr;
    void* user = nullptr;

    void capture_loop() {
        com_init();
        std::vector<float> stereo;
        while (running.load()) {
            UINT32 packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet)) || packet == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            while (packet > 0 && running.load()) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr,
                                              nullptr))) {
                    break;
                }
                stereo.resize(static_cast<size_t>(frames) * 2);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(stereo.begin(), stereo.end(), 0.0f);
                } else {
                    const float* src = reinterpret_cast<const float*>(data);
                    for (UINT32 i = 0; i < frames; ++i) {
                        const float l = src[static_cast<size_t>(i) * channels];
                        const float r = channels >= 2
                                            ? src[static_cast<size_t>(i) * channels + 1]
                                            : l;
                        stereo[static_cast<size_t>(i) * 2] = l;
                        stereo[static_cast<size_t>(i) * 2 + 1] = r;
                    }
                }
                if (callback) callback(user, stereo.data(), static_cast<int>(frames));
                capture->ReleaseBuffer(frames);
                if (FAILED(capture->GetNextPacketSize(&packet))) break;
            }
        }
    }
};

WasapiLoopback::~WasapiLoopback() {
    close();
}

bool WasapiLoopback::open(int sample_rate, std::string* error) {
    close();
    com_init();
    impl_ = new Impl();
    impl_->sample_rate = sample_rate;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(enumerator.GetAddressOf())))) {
        if (error) *error = "WASAPI enumerator creation failed";
        close();
        return false;
    }
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                   device.GetAddressOf())) ||
        !device) {
        if (error) *error = "WASAPI default render device not found";
        close();
        return false;
    }
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(impl_->client.GetAddressOf())))) {
        if (error) *error = "WASAPI capture client activation failed";
        close();
        return false;
    }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(impl_->client->GetMixFormat(&mix)) || !mix) {
        if (error) *error = "WASAPI mix format unavailable";
        close();
        return false;
    }
    int rate = 0;
    int channels = 0;
    const bool float_format = is_float_stereo(mix, &rate, &channels);
    if (!float_format || rate != sample_rate || channels < 1) {
        CoTaskMemFree(mix);
        if (error) *error = "WASAPI loopback requires 48 kHz float output";
        close();
        return false;
    }
    impl_->channels = channels;
    const HRESULT hr = impl_->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, mix,
        nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) {
        if (error) *error = "WASAPI loopback initialize failed";
        close();
        return false;
    }
    if (FAILED(impl_->client->GetService(
            IID_PPV_ARGS(impl_->capture.GetAddressOf()))) ||
        FAILED(impl_->client->GetBufferSize(&impl_->buffer_frames))) {
        if (error) *error = "WASAPI loopback setup failed";
        close();
        return false;
    }
    impl_->running = true;
    if (FAILED(impl_->client->Start())) {
        if (error) *error = "WASAPI loopback start failed";
        impl_->running = false;
        close();
        return false;
    }
    impl_->thread = std::thread([this] { impl_->capture_loop(); });
    return true;
}

void WasapiLoopback::close() {
    if (!impl_) return;
    impl_->running = false;
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->client) impl_->client->Stop();
    delete impl_;
    impl_ = nullptr;
}

void WasapiLoopback::set_callback(Callback callback, void* user) {
    if (!impl_) return;
    impl_->callback = callback;
    impl_->user = user;
}

bool WasapiLoopback::active() const {
    return impl_ && impl_->running.load();
}

}  // namespace obs
