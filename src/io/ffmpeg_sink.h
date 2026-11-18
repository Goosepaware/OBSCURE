#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace obs {

enum class VideoCodec {
    H264,
    H265,
    Av1,
};

// Encodes D3D11 BGRA frames to an H.264/H.265 file through ffmpeg's stdin.
// Readback and pipe writes happen on a worker; the render thread only copies
// the texture into a staging resource and queues the pixels.
class FfmpegSink {
public:
    FfmpegSink() = default;
    ~FfmpegSink();
    FfmpegSink(const FfmpegSink&) = delete;
    FfmpegSink& operator=(const FfmpegSink&) = delete;

    bool start(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height,
               double fps, double bitrate_mbps, const std::wstring& output,
               std::string* error, VideoCodec codec = VideoCodec::H264);
    void write(ID3D11Texture2D* texture);
    void stop();
    bool active() const { return process_ != nullptr; }
    const std::string& error() const { return error_; }

private:
    void writer_loop();
    void stderr_loop();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    int width_ = 0;
    int height_ = 0;
    VideoCodec codec_ = VideoCodec::H264;
    void* process_ = nullptr;
    void* stdin_write_ = nullptr;
    void* stderr_read_ = nullptr;
    std::thread writer_;
    std::thread stderr_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable space_cv_;
    std::deque<std::vector<uint8_t>> queue_;
    bool stop_ = false;
    bool failed_ = false;
    std::string error_;
    std::string stderr_text_;
};

}  // namespace obs
