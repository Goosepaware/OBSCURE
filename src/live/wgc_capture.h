#pragma once

#include <d3d11.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace obs {

bool find_window_by_title(const std::wstring& needle, HWND* out, std::wstring* title);
bool monitor_by_index(int index, HMONITOR* out, std::wstring* name);

class WgcCapture {
public:
    WgcCapture() = default;
    ~WgcCapture();
    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;

    bool start(ID3D11Device* device, ID3D11DeviceContext* context, HWND window,
               HMONITOR monitor, std::string* error);
    void stop();

    // Caller must hold mutex().
    bool copy_latest(ID3D11Texture2D* destination);
    bool has_frame() const { return frame_count_.load() > 0; }
    uint64_t frame_count() const { return frame_count_.load(); }
    int width() const { return width_; }
    int height() const { return height_; }
    DXGI_FORMAT format() const { return format_; }
    std::mutex& mutex() { return mutex_; }

private:
    void on_frame();
    bool ensure_capture_texture(ID3D11Texture2D* source);
    void close_winrt();

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    std::mutex mutex_;
    winrt::com_ptr<ID3D11Texture2D> capture_tex_;
    int width_ = 0;
    int height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    std::atomic<uint64_t> frame_count_{0};
    std::chrono::steady_clock::time_point last_copy_{};

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device_{nullptr};
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat pixel_format_{
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized};
    int32_t pool_w_ = 0;
    int32_t pool_h_ = 0;
    winrt::event_token frame_token_{};
    bool started_ = false;
};

}  // namespace obs
