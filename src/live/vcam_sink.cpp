#include "live/vcam_sink.h"

#include <windows.h>

#include <opencv2/imgproc.hpp>

#include "live/d3d_helpers.h"

namespace obs {

namespace {

struct SharedHeader {
    DWORD max_size;
    int width;
    int height;
    int stride;
    int format;
    int resize_mode;
    int mirror_mode;
    int timeout;
    uint8_t data[1];
};

constexpr int kFormatUint8 = 0;

void close_handle(void*& handle) {
    if (handle) {
        CloseHandle(static_cast<HANDLE>(handle));
        handle = nullptr;
    }
}

}  // namespace

VcamSink::~VcamSink() {
    stop();
}

bool VcamSink::start(ID3D11Device* device, ID3D11DeviceContext* context, int width,
                     int height, std::string* error) {
    stop();
    if (!device || !context || width <= 0 || height <= 0) {
        if (error) *error = "virtual camera: bad frame size";
        return false;
    }
    device_ = device;
    context_ = context;
    width_ = width;
    height_ = height;
    staging_ = create_staging_texture(device, width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!staging_) {
        if (error) *error = "virtual camera: staging texture failed";
        return false;
    }
    try_open();
    return true;
}

void VcamSink::stop() {
    close_handles();
    staging_.Reset();
    device_.Reset();
    context_.Reset();
}

void VcamSink::close_handles() {
    ready_ = false;
    if (shared_) {
        UnmapViewOfFile(static_cast<LPCVOID>(shared_));
        shared_ = nullptr;
    }
    close_handle(mapping_);
    close_handle(sent_event_);
    close_handle(want_event_);
    close_handle(mutex_);
}

bool VcamSink::try_open() {
    if (ready_) return true;

    // Match UnityCapture's sender handshake: keep the want-event alive across
    // retries. The filter creates the sent-event and shared mapping only after
    // it can open the want-event, so closing it on a partial failure deadlocks
    // both sides.
    if (!mutex_) {
        mutex_ = OpenMutexA(SYNCHRONIZE, FALSE, "UnityCapture_Mutx");
        if (!mutex_) return false;
    }
    if (!want_event_) {
        want_event_ = CreateEventA(nullptr, FALSE, FALSE, "UnityCapture_Want");
        if (!want_event_) return false;
    }
    if (!sent_event_) {
        sent_event_ = OpenEventA(EVENT_MODIFY_STATE, FALSE, "UnityCapture_Sent");
        if (!sent_event_) return false;
    }
    if (!mapping_) {
        mapping_ = OpenFileMappingA(FILE_MAP_WRITE, FALSE, "UnityCapture_Data");
        if (!mapping_) return false;
    }
    if (!shared_) {
        shared_ = MapViewOfFile(static_cast<HANDLE>(mapping_), FILE_MAP_WRITE, 0, 0, 0);
        if (!shared_) return false;
    }
    auto* header = static_cast<SharedHeader*>(shared_);
    const DWORD required = static_cast<DWORD>(width_) * height_ * 4;
    if (header->max_size != 0 && header->max_size < required) {
        close_handles();
        return false;
    }
    ready_ = true;
    return true;
}

void VcamSink::send(ID3D11Texture2D* texture) {
    if (!texture || !context_ || !staging_) return;
    if (!ready_) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_retry_) return;
        next_retry_ = now + std::chrono::milliseconds(250);
        if (!try_open()) return;
    }

    context_->CopyResource(staging_.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return;
    }

    const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(mutex_), 1000);
    if (wait != WAIT_OBJECT_0) {
        context_->Unmap(staging_.Get(), 0);
        close_handles();
        return;
    }

    auto* header = static_cast<SharedHeader*>(shared_);
    header->width = width_;
    header->height = height_;
    // UnityCapture's filter treats stride as a pixel count (uint32_t* row
    // advance), not a byte pitch.
    header->stride = width_;
    header->format = kFormatUint8;
    header->resize_mode = 1;
    header->mirror_mode = 0;
    header->timeout = 1000;

    cv::Mat bgra(height_, width_, CV_8UC4, mapped.pData, mapped.RowPitch);
    cv::Mat rgba_out(height_, width_, CV_8UC4, header->data, width_ * 4);
    cv::cvtColor(bgra, rgba_out, cv::COLOR_BGRA2RGBA);

    ReleaseMutex(static_cast<HANDLE>(mutex_));
    SetEvent(static_cast<HANDLE>(sent_event_));
    WaitForSingleObject(static_cast<HANDLE>(want_event_), 0);
    context_->Unmap(staging_.Get(), 0);
}

}  // namespace obs
