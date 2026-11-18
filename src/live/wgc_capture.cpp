#include "live/wgc_capture.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <iostream>
#include <vector>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace obs {

namespace {

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

struct WindowSearch {
    std::wstring needle;
    HWND found = nullptr;
    std::wstring title;
};

BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lp) {
    auto* search = reinterpret_cast<WindowSearch*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    wchar_t title[512]{};
    const int n = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    if (n <= 0) return TRUE;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    if (rc.right - rc.left < 32 || rc.bottom - rc.top < 32) return TRUE;
    const std::wstring lowered = lower(title);
    if (lowered == search->needle) {
        search->found = hwnd;
        search->title = title;
        return FALSE;
    }
    if (!search->found && lowered.find(search->needle) != std::wstring::npos) {
        search->found = hwnd;
        search->title = title;
    }
    return TRUE;
}

struct MonitorSearch {
    int index = 0;
    int current = 0;
    HMONITOR found = nullptr;
    std::wstring name;
};

BOOL CALLBACK enum_monitors_proc(HMONITOR monitor, HDC, LPRECT, LPARAM lp) {
    auto* search = reinterpret_cast<MonitorSearch*>(lp);
    if (search->current == search->index) {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
            search->found = monitor;
            search->name = info.szDevice;
            return FALSE;
        }
    }
    ++search->current;
    return TRUE;
}

}  // namespace

bool find_window_by_title(const std::wstring& needle, HWND* out, std::wstring* title) {
    WindowSearch search;
    search.needle = lower(needle);
    EnumWindows(enum_windows_proc, reinterpret_cast<LPARAM>(&search));
    if (!search.found) return false;
    *out = search.found;
    if (title) *title = search.title;
    return true;
}

bool monitor_by_index(int index, HMONITOR* out, std::wstring* name) {
    MonitorSearch search;
    search.index = index;
    EnumDisplayMonitors(nullptr, nullptr, enum_monitors_proc,
                        reinterpret_cast<LPARAM>(&search));
    if (!search.found) return false;
    *out = search.found;
    if (name) *name = search.name;
    return true;
}

WgcCapture::~WgcCapture() {
    stop();
}

bool WgcCapture::start(ID3D11Device* device, ID3D11DeviceContext* context, HWND window,
                       HMONITOR monitor, std::string* error) {
    stop();
    device_ = device;
    context_ = context;
    try {
        const auto interop = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        if (window) {
            winrt::check_hresult(interop->CreateForWindow(
                window,
                winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(item_)));
        } else {
            winrt::check_hresult(interop->CreateForMonitor(
                monitor,
                winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(item_)));
        }

        winrt::com_ptr<IDXGIDevice> dxgi;
        winrt::check_hresult(device_->QueryInterface(IID_PPV_ARGS(dxgi.put())));
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(
            CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        auto winrt_device =
            inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        const auto size = item_.Size();
        pool_w_ = size.Width;
        pool_h_ = size.Height;
        pixel_format_ = winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
        winrt_device_ = winrt_device;
        frame_pool_ =
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrt_device,
                pixel_format_,
                2, size);
        session_ = frame_pool_.CreateCaptureSession(item_);
        session_.IsCursorCaptureEnabled(false);
        try {
            session_.IsBorderRequired(false);
        } catch (...) {
            // Older Windows builds do not expose IsBorderRequired.
        }

        frame_token_ = frame_pool_.FrameArrived(
            [this](auto&&, auto&&) { on_frame(); });
        session_.StartCapture();
        started_ = true;
        return true;
    } catch (const winrt::hresult_error& e) {
        if (error) *error = winrt::to_string(e.message());
        close_winrt();
        return false;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        close_winrt();
        return false;
    }
}

void WgcCapture::close_winrt() {
    try {
        if (session_) {
            session_.Close();
            session_ = nullptr;
        }
        if (frame_pool_) {
            frame_pool_.FrameArrived(frame_token_);
            frame_pool_.Close();
            frame_pool_ = nullptr;
        }
        winrt_device_ = nullptr;
        pool_w_ = 0;
        pool_h_ = 0;
        item_ = nullptr;
    } catch (...) {
    }
}

void WgcCapture::stop() {
    if (!started_) {
        close_winrt();
        return;
    }
    started_ = false;
    close_winrt();
    std::lock_guard lock(mutex_);
    capture_tex_ = nullptr;
}

bool WgcCapture::ensure_capture_texture(ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (capture_tex_ && width_ == static_cast<int>(desc.Width) &&
        height_ == static_cast<int>(desc.Height) && format_ == desc.Format) {
        return true;
    }
    D3D11_TEXTURE2D_DESC dst = desc;
    dst.Usage = D3D11_USAGE_DEFAULT;
    dst.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dst.CPUAccessFlags = 0;
    dst.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> tex;
    if (FAILED(device_->CreateTexture2D(&dst, nullptr, tex.put()))) {
        return false;
    }
    capture_tex_ = tex;
    width_ = static_cast<int>(desc.Width);
    height_ = static_cast<int>(desc.Height);
    format_ = desc.Format;
    return true;
}

void WgcCapture::on_frame() {
    try {
        auto frame = frame_pool_.TryGetNextFrame();
        if (!frame) return;

        // A window capture keeps its original pool size unless we recreate it.
        // Resizing the player would otherwise leave the decoder locked to the
        // old size and miss the markers.
        const auto content = frame.ContentSize();
        if (content.Width > 0 && content.Height > 0 &&
            (content.Width != pool_w_ || content.Height != pool_h_)) {
            try {
                frame_pool_.Recreate(winrt_device_, pixel_format_, 2, content);
                pool_w_ = content.Width;
                pool_h_ = content.Height;
            } catch (...) {
                // Fall back to the existing pool; the GUI will retry on resize.
            }
            frame.Close();
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (last_copy_.time_since_epoch().count() != 0 &&
            now - last_copy_ < std::chrono::milliseconds(15)) {
            frame.Close();
            return;
        }
        last_copy_ = now;

        auto surface = frame.Surface();
        auto access =
            surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> tex;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(tex.put())));

        std::lock_guard lock(mutex_);
        if (ensure_capture_texture(tex.get())) {
            context_->CopyResource(capture_tex_.get(), tex.get());
            frame_count_.fetch_add(1, std::memory_order_relaxed);
        }
        frame.Close();
    } catch (...) {
        // A closed window or a transient capture error: keep the last frame.
    }
}

bool WgcCapture::copy_latest(ID3D11Texture2D* destination) {
    if (!destination) return false;
    if (!capture_tex_) return false;
    D3D11_TEXTURE2D_DESC src{};
    D3D11_TEXTURE2D_DESC dst{};
    capture_tex_->GetDesc(&src);
    destination->GetDesc(&dst);
    if (src.Width != dst.Width || src.Height != dst.Height || src.Format != dst.Format) {
        return false;
    }
    context_->CopyResource(destination, capture_tex_.get());
    return true;
}

}  // namespace obs
