#include "live/preview_window.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "live/d3d_helpers.h"

namespace obs {

namespace {

constexpr wchar_t kClassName[] = L"OBScurePreviewWindow";
constexpr int kPlayButton = 1001;
constexpr int kLoopCheck = 1002;
constexpr int kTrackbar = 1003;
constexpr int kTimeLabel = 1004;

int scaled(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

}  // namespace

PreviewWindow::~PreviewWindow() {
    shutdown();
}

std::wstring PreviewWindow::format_time(double ms) {
    if (ms < 0.0) ms = 0.0;
    const long long total = static_cast<long long>(ms / 1000.0);
    const long long h = total / 3600;
    const long long m = (total % 3600) / 60;
    const long long s = total % 60;
    wchar_t buffer[32]{};
    if (h > 0) {
        swprintf_s(buffer, L"%lld:%02lld:%02lld", h, m, s);
    } else {
        swprintf_s(buffer, L"%02lld:%02lld", m, s);
    }
    return buffer;
}

LRESULT CALLBACK PreviewWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PreviewWindow* self =
        reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_SIZE:
            if (self && wp != SIZE_MINIMIZED) self->layout_player();
            return 0;
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_KEYDOWN:
            if (!self) break;
            if (wp == VK_F8) {
                self->set_topmost(!self->topmost_);
                return 0;
            }
            if (wp == VK_SPACE) {
                self->pending_command_.type = PreviewPlayerCommand::Type::TogglePause;
                return 0;
            }
            if (wp == VK_LEFT || wp == VK_RIGHT) {
                if (self->player_duration_ms_ > 0.0) {
                    const double delta = wp == VK_LEFT ? -5000.0 : 5000.0;
                    const double target = std::clamp(
                        self->player_position_ms_ + delta, 0.0,
                        self->player_duration_ms_);
                    self->pending_command_.type = PreviewPlayerCommand::Type::Seek;
                    self->pending_command_.value = target;
                }
                return 0;
            }
            break;
        case WM_COMMAND: {
            if (!self) break;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == kPlayButton && code == BN_CLICKED) {
                self->pending_command_.type = PreviewPlayerCommand::Type::TogglePause;
            } else if (id == kLoopCheck && code == BN_CLICKED) {
                self->pending_command_.type = PreviewPlayerCommand::Type::ToggleLoop;
            }
            return 0;
        }
        case WM_HSCROLL: {
            if (!self || reinterpret_cast<HWND>(lp) != self->trackbar_) break;
            const int code = LOWORD(wp);
            const int pos = static_cast<int>(SendMessageW(self->trackbar_, TBM_GETPOS, 0, 0));
            const double ms = self->player_duration_ms_ > 0.0
                                  ? self->player_duration_ms_ * pos / 1000.0
                                  : 0.0;
            if (code == TB_THUMBTRACK) {
                self->trackbar_dragging_ = true;
                self->drag_position_ms_ = ms;
                const ULONGLONG now = GetTickCount64();
                if (now - self->last_seek_tick_ >= 120) {
                    self->pending_command_.type = PreviewPlayerCommand::Type::Seek;
                    self->pending_command_.value = ms;
                    self->last_seek_tick_ = now;
                }
                self->update_player_controls();
            } else if (code == TB_ENDTRACK) {
                self->trackbar_dragging_ = false;
                self->drag_position_ms_ = ms;
                self->pending_command_.type = PreviewPlayerCommand::Type::Seek;
                self->pending_command_.value = ms;
                self->update_player_controls();
            } else if (code == TB_THUMBPOSITION || code == TB_LINEUP ||
                       code == TB_LINEDOWN || code == TB_PAGEUP ||
                       code == TB_PAGEDOWN || code == TB_TOP || code == TB_BOTTOM) {
                self->drag_position_ms_ = ms;
                self->pending_command_.type = PreviewPlayerCommand::Type::Seek;
                self->pending_command_.value = ms;
                self->update_player_controls();
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            if (!self || !self->dark_brush_) break;
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(220, 225, 235));
            return reinterpret_cast<LRESULT>(self->dark_brush_);
        }
        case WM_CTLCOLORBTN: {
            if (!self || !self->dark_brush_) break;
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(18, 20, 26));
            SetTextColor(hdc, RGB(220, 225, 235));
            return reinterpret_cast<LRESULT>(self->dark_brush_);
        }
        case WM_ERASEBKGND: {
            if (self && self->dark_brush_) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, self->dark_brush_);
            }
            return 1;
        }
        case WM_DPICHANGED: {
            auto* rc = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                         rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize.x = 480;
            info->ptMinTrackSize.y = 300;
            return 0;
        }
        case WM_DESTROY:
            if (self) self->hwnd_ = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool PreviewWindow::init(ID3D11Device* device, ID3D11DeviceContext* context,
                         const RECT& rect, const std::wstring& title,
                         const std::wstring& shader_path, std::string* error) {
    shutdown();
    device_ = device;
    context_ = context;
    title_ = title;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    dark_brush_ = CreateSolidBrush(RGB(18, 20, 26));
    if (!dark_brush_) {
        if (error) *error = "preview window: brush creation failed";
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PreviewWindow::wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (error) *error = "preview window: register class failed";
        return false;
    }

    int x = rect.left;
    int y = rect.top;
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width < 320 || height < 180) {
        x = CW_USEDEFAULT;
        y = CW_USEDEFAULT;
        width = 960;
        height = 540;
    }
    hwnd_ = CreateWindowExW(0, kClassName, title_.c_str(), WS_OVERLAPPEDWINDOW, x, y,
                            width, height, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) {
        if (error) *error = "preview window: create failed";
        return false;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    video_hwnd_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                  0, 0, 1, 1, hwnd_, nullptr, instance, nullptr);
    if (!video_hwnd_) {
        if (error) *error = "preview window: video host creation failed";
        return false;
    }

    play_button_ = CreateWindowExW(
        0, L"BUTTON", L"暂停", WS_CHILD | BS_PUSHBUTTON, 0, 0, 1, 1, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlayButton)), instance, nullptr);
    loop_check_ = CreateWindowExW(
        0, L"BUTTON", L"循环", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 1, 1, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLoopCheck)), instance, nullptr);
    trackbar_ = CreateWindowExW(
        0, TRACKBAR_CLASSW, L"", WS_CHILD | TBS_HORZ | TBS_NOTICKS, 0, 0, 1, 1, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTrackbar)), instance, nullptr);
    time_label_ = CreateWindowExW(
        0, L"STATIC", L"00:00 / 00:00", WS_CHILD | SS_LEFT | SS_CENTERIMAGE, 0, 0, 1, 1,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTimeLabel)), instance,
        nullptr);
    if (!play_button_ || !loop_check_ || !trackbar_ || !time_label_) {
        if (error) *error = "preview window: player controls creation failed";
        return false;
    }

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    for (HWND control : {play_button_, loop_check_, trackbar_, time_label_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    SendMessageW(trackbar_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    SendMessageW(trackbar_, TBM_SETPAGESIZE, 0, 50);
    SendMessageW(loop_check_, BM_SETCHECK, BST_CHECKED, 0);
    last_loop_ = true;

    player_visible_ = false;
    layout_player();
    for (HWND control : {play_button_, loop_check_, trackbar_, time_label_}) {
        ShowWindow(control, SW_HIDE);
    }

    auto vs_blob = compile_shader(shader_path, "VSMain", "vs_5_0", error);
    auto ps_blob = compile_shader(shader_path, "PSMain", "ps_5_0", error);
    if (!vs_blob || !ps_blob) return false;
    if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(), nullptr,
                                          vs_.GetAddressOf())) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                         ps_blob->GetBufferSize(), nullptr,
                                         ps_.GetAddressOf()))) {
        if (error) *error = "preview window: shader creation failed";
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler_desc, sampler_.GetAddressOf()))) {
        if (error) *error = "preview window: sampler creation failed";
        return false;
    }

    if (!create_swapchain()) {
        if (error) *error = "preview window: swapchain creation failed";
        return false;
    }
    return true;
}

bool PreviewWindow::create_swapchain() {
    if (!video_hwnd_) return false;
    RECT client{};
    GetClientRect(video_hwnd_, &client);
    const UINT width = static_cast<UINT>(std::max(1L, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max(1L, client.bottom - client.top));

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf())))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf()))) return false;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), video_hwnd_, &desc,
                                               nullptr, nullptr,
                                               swapchain_.GetAddressOf()))) {
        return false;
    }

    rtvs_.clear();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf())))) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device_->CreateRenderTargetView(buffer.Get(), nullptr,
                                               rtv.GetAddressOf()))) {
        return false;
    }
    rtvs_.push_back(std::move(rtv));
    return true;
}

void PreviewWindow::resize_swapchain(int width, int height) {
    if (!swapchain_ || width <= 0 || height <= 0) return;
    if (context_) context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtvs_.clear();
    if (FAILED(swapchain_->ResizeBuffers(0, static_cast<UINT>(width),
                                         static_cast<UINT>(height),
                                         DXGI_FORMAT_UNKNOWN, 0))) {
        return;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> buffer;
    if (SUCCEEDED(swapchain_->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf())))) {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(device_->CreateRenderTargetView(buffer.Get(), nullptr,
                                                      rtv.GetAddressOf()))) {
            rtvs_.push_back(std::move(rtv));
        }
    }
}

void PreviewWindow::layout_player() {
    if (!hwnd_ || !video_hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int client_w = std::max(1L, client.right - client.left);
    const int client_h = std::max(1L, client.bottom - client.top);
    const UINT dpi = GetDpiForWindow(hwnd_);
    const int bar_h = player_visible_ ? scaled(56, dpi) : 0;
    const int video_h = std::max(1, client_h - bar_h);

    SetWindowPos(video_hwnd_, nullptr, 0, 0, client_w, video_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (client_w != video_w_ || video_h != video_h_) {
        video_w_ = client_w;
        video_h_ = video_h;
        if (swapchain_) resize_swapchain(video_w_, video_h_);
    }

    if (!player_visible_ || !play_button_) return;

    const int margin = scaled(8, dpi);
    const int control_h = scaled(30, dpi);
    const int y = video_h + scaled(13, dpi);
    const int play_w = scaled(64, dpi);
    const int time_w = scaled(120, dpi);
    const int loop_w = scaled(70, dpi);
    const int loop_x = client_w - margin - loop_w;
    const int play_x = margin;
    const int time_x = play_x + play_w + margin;
    const int track_x = time_x + time_w + margin;
    const int track_w = std::max(scaled(80, dpi), loop_x - margin - track_x);

    SetWindowPos(play_button_, nullptr, play_x, y, play_w, control_h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(time_label_, nullptr, time_x, y, time_w, control_h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(trackbar_, nullptr, track_x, y, track_w, control_h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(loop_check_, nullptr, loop_x, y, loop_w, control_h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void PreviewWindow::update_player_controls() {
    if (!play_button_) return;
    EnableWindow(play_button_, player_running_);
    EnableWindow(loop_check_, player_running_);
    EnableWindow(trackbar_, player_running_ && player_duration_ms_ > 0.0);

    if (player_paused_ != last_paused_) {
        SetWindowTextW(play_button_, player_paused_ ? L"播放" : L"暂停");
        last_paused_ = player_paused_;
    }
    if (player_loop_ != last_loop_) {
        SendMessageW(loop_check_, BM_SETCHECK,
                     player_loop_ ? BST_CHECKED : BST_UNCHECKED, 0);
        last_loop_ = player_loop_;
    }

    const double shown_position =
        trackbar_dragging_ ? drag_position_ms_ : player_position_ms_;
    const std::wstring text =
        format_time(shown_position) + L" / " + format_time(player_duration_ms_);
    if (text != last_time_text_) {
        SetWindowTextW(time_label_, text.c_str());
        last_time_text_ = text;
    }

    if (!trackbar_dragging_ && player_duration_ms_ > 0.0) {
        const int pos = static_cast<int>(std::llround(
            player_position_ms_ / player_duration_ms_ * 1000.0));
        SendMessageW(trackbar_, TBM_SETPOS, TRUE, std::clamp(pos, 0, 1000));
    }
}

void PreviewWindow::set_player_state(bool visible, bool running, bool paused,
                                     bool loop, double position_ms,
                                     double duration_ms, double fps) {
    const bool visibility_changed = visible != player_visible_;
    player_visible_ = visible;
    player_running_ = running;
    player_paused_ = paused;
    player_loop_ = loop;
    player_position_ms_ = position_ms;
    player_duration_ms_ = duration_ms;
    player_fps_ = fps;
    if (visibility_changed) layout_player();
    if (player_visible_) update_player_controls();
}

bool PreviewWindow::take_player_command(PreviewPlayerCommand* command) {
    if (!command || pending_command_.type == PreviewPlayerCommand::Type::None) {
        return false;
    }
    *command = pending_command_;
    pending_command_ = {};
    return true;
}

void PreviewWindow::present(ID3D11ShaderResourceView* texture, int src_w, int src_h) {
    if (!swapchain_ || !hwnd_ || !video_hwnd_ || !IsWindowVisible(hwnd_) ||
        IsIconic(hwnd_)) {
        return;
    }
    if (rtvs_.empty()) return;

    RECT client{};
    GetClientRect(video_hwnd_, &client);
    const int cw = client.right - client.left;
    const int ch = client.bottom - client.top;
    if (cw <= 0 || ch <= 0) return;

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context_->ClearRenderTargetView(rtvs_[0].Get(), clear);
    context_->OMSetRenderTargets(1, rtvs_[0].GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp{};
    vp.MaxDepth = 1.0f;
    if (texture && src_w > 0 && src_h > 0) {
        const float scale = std::min(static_cast<float>(cw) / src_w,
                                     static_cast<float>(ch) / src_h);
        const float vw = static_cast<float>(src_w) * scale;
        const float vh = static_cast<float>(src_h) * scale;
        vp.TopLeftX = (static_cast<float>(cw) - vw) * 0.5f;
        vp.TopLeftY = (static_cast<float>(ch) - vh) * 0.5f;
        vp.Width = vw;
        vp.Height = vh;
        context_->RSSetViewports(1, &vp);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[1] = {texture};
        ID3D11SamplerState* samplers[1] = {sampler_.Get()};
        context_->PSSetShaderResources(0, 1, srvs);
        context_->PSSetSamplers(0, 1, samplers);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[1] = {nullptr};
        context_->PSSetShaderResources(0, 1, nulls);
    }
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    swapchain_->Present(1, 0);
}

void PreviewWindow::show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOW);
    SetWindowPos(hwnd_, topmost_ ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void PreviewWindow::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void PreviewWindow::set_topmost(bool topmost) {
    topmost_ = topmost;
    if (!hwnd_) return;
    SetWindowPos(hwnd_, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool PreviewWindow::visible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

bool PreviewWindow::minimized() const {
    return hwnd_ && IsIconic(hwnd_);
}

void PreviewWindow::get_window_rect(RECT* rect) const {
    if (!rect) return;
    if (hwnd_ && GetWindowRect(hwnd_, rect)) return;
    *rect = RECT{};
}

void PreviewWindow::set_window_rect(const RECT& rect) {
    if (!hwnd_) return;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width < 320 || height < 180) return;
    SetWindowPos(hwnd_, nullptr, rect.left, rect.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void PreviewWindow::set_title(const std::wstring& title) {
    if (title_ == title) return;
    title_ = title;
    if (hwnd_) SetWindowTextW(hwnd_, title_.c_str());
}

void PreviewWindow::shutdown() {
    rtvs_.clear();
    swapchain_.Reset();
    vs_.Reset();
    ps_.Reset();
    sampler_.Reset();
    play_button_ = nullptr;
    loop_check_ = nullptr;
    trackbar_ = nullptr;
    time_label_ = nullptr;
    video_hwnd_ = nullptr;
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (dark_brush_) {
        DeleteObject(dark_brush_);
        dark_brush_ = nullptr;
    }
    device_.Reset();
    context_.Reset();
}

}  // namespace obs
