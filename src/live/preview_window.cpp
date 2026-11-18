#include "live/preview_window.h"

#include <windows.h>

#include <algorithm>

#include "live/d3d_helpers.h"

namespace obs {

namespace {

constexpr wchar_t kClassName[] = L"OBScurePreviewWindow";

}  // namespace

PreviewWindow::~PreviewWindow() {
    shutdown();
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
            if (self && wp != SIZE_MINIMIZED) {
                self->resize_swapchain(LOWORD(lp), HIWORD(lp));
            }
            return 0;
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_KEYDOWN:
            if (self && wp == VK_F8) {
                self->set_topmost(!self->topmost_);
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            auto* rc = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                         rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize.x = 320;
            info->ptMinTrackSize.y = 180;
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
    RECT client{};
    GetClientRect(hwnd_, &client);
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
    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr,
                                               nullptr, swapchain_.GetAddressOf()))) {
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

void PreviewWindow::present(ID3D11ShaderResourceView* texture, int src_w, int src_h) {
    if (!swapchain_ || !hwnd_ || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) return;
    if (rtvs_.empty()) return;

    RECT client{};
    GetClientRect(hwnd_, &client);
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
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    device_.Reset();
    context_.Reset();
}

}  // namespace obs
