#include "live/output_window.h"

#include <windows.h>

#include <algorithm>
#include <stdexcept>

#include "live/d3d_helpers.h"

namespace obs {

namespace {

constexpr wchar_t kClassName[] = L"OBScureOutputWindow";

struct DpiScope {
    DPI_AWARENESS_CONTEXT old = nullptr;
    DpiScope() { old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); }
    ~DpiScope() {
        if (old) SetThreadDpiAwarenessContext(old);
    }
};

struct MonitorEnum {
    std::vector<RECT>* rects = nullptr;
};

BOOL CALLBACK enum_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<MonitorEnum*>(lp);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        ctx->rects->push_back(info.rcWork);
    }
    return TRUE;
}

}  // namespace

OutputWindow::~OutputWindow() {
    shutdown();
}

LRESULT CALLBACK OutputWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OutputWindow* self =
        reinterpret_cast<OutputWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            if (self) self->hwnd_ = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool OutputWindow::init(ID3D11Device* device, ID3D11DeviceContext* context, int width,
                        int height, const std::wstring& title,
                        const std::wstring& shader_path, std::string* error) {
    shutdown();
    device_ = device;
    context_ = context;
    width_ = width;
    height_ = height;
    title_ = title;
    DpiScope dpi_scope;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &OutputWindow::wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (error) *error = "failed to register output window class";
        return false;
    }

    hwnd_ = CreateWindowExW(0, kClassName, title_.c_str(), WS_POPUP, 0, 0, width_, height_,
                            nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) {
        if (error) *error = "failed to create output window";
        return false;
    }
    SetWindowTextW(hwnd_, title_.c_str());
    center_on_primary();

    auto vs_blob = compile_shader(shader_path, "VSMain", "vs_5_0", error);
    auto ps_blob = compile_shader(shader_path, "PSMain", "ps_5_0", error);
    if (!vs_blob || !ps_blob) return false;
    if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(), nullptr,
                                          vs_.GetAddressOf())) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                         ps_blob->GetBufferSize(), nullptr,
                                         ps_.GetAddressOf()))) {
        if (error) *error = "failed to create blit shaders";
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
        if (error) *error = "failed to create blit sampler";
        return false;
    }

    if (!create_swapchain()) {
        if (error) *error = "failed to create output swapchain";
        return false;
    }
    return true;
}

bool OutputWindow::create_swapchain() {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf())))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf()))) return false;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
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
    swapchain_.As(&swapchain3_);

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

void OutputWindow::shutdown() {
    rtvs_.clear();
    swapchain3_.Reset();
    swapchain_.Reset();
    vs_.Reset();
    ps_.Reset();
    sampler_.Reset();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void OutputWindow::present(ID3D11ShaderResourceView* texture) {
    if (!swapchain_ || !texture || !hwnd_ || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
        return;
    }
    if (rtvs_.empty()) return;
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MaxDepth = 1.0f;
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context_->ClearRenderTargetView(rtvs_[0].Get(), clear);
    context_->OMSetRenderTargets(1, rtvs_[0].GetAddressOf(), nullptr);
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
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    swapchain_->Present(1, 0);
}

void OutputWindow::show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOWNA);
    SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void OutputWindow::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void OutputWindow::set_bottom() {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void OutputWindow::set_topmost(bool topmost) {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void OutputWindow::move_to_monitor(int index) {
    if (!hwnd_) return;
    std::vector<RECT> rects;
    MonitorEnum ctx{&rects};
    EnumDisplayMonitors(nullptr, nullptr, enum_monitor, reinterpret_cast<LPARAM>(&ctx));
    if (rects.empty()) return;
    const int i = std::clamp(index, 0, static_cast<int>(rects.size()) - 1);
    const RECT& rc = rects[static_cast<size_t>(i)];
    const int x = rc.left + ((rc.right - rc.left) - width_) / 2;
    const int y = rc.top + ((rc.bottom - rc.top) - height_) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
}

void OutputWindow::center_on_primary() {
    if (!hwnd_) return;
    // GetMonitorInfo can be DPI-virtualized on mixed-DPI setups; the screen
    // metrics here are physical and match the borderless output surface.
    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);
    const int x = std::max(0, (screen_w - width_) / 2);
    const int y = std::max(0, (screen_h - height_) / 2);
    SetWindowPos(hwnd_, nullptr, x, y, width_, height_, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool OutputWindow::visible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

bool OutputWindow::minimized() const {
    return hwnd_ && IsIconic(hwnd_);
}

}  // namespace obs
