#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <string>

namespace obs {

// Sends the transport texture to the DirectShow virtual camera through the
// UnityCapture-compatible shared memory ring. The camera device is registered
// per-user by the GUI; this class only produces frames.
class VcamSink {
public:
    VcamSink() = default;
    ~VcamSink();
    VcamSink(const VcamSink&) = delete;
    VcamSink& operator=(const VcamSink&) = delete;

    bool start(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height,
               std::string* error);
    void stop();
    void send(ID3D11Texture2D* texture);
    bool ready() const { return ready_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    bool try_open();
    void close_handles();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    int width_ = 0;
    int height_ = 0;
    bool ready_ = false;
    void* mutex_ = nullptr;
    void* want_event_ = nullptr;
    void* sent_event_ = nullptr;
    void* mapping_ = nullptr;
    void* shared_ = nullptr;
    std::chrono::steady_clock::time_point next_retry_{};
};

}  // namespace obs
