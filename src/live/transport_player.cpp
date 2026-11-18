// Minimal D3D11 transport player window.
// Used to validate the decoder's WGC window capture without browser/OBS quirks.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "live/d3d_helpers.h"

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {
std::atomic<bool> g_close{false};

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_close.store(true);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}
}  // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2) {
        std::cout << "transport-player <transport.mp4>\n";
        return 0;
    }
    cv::VideoCapture cap;
    cap.open(argv[1], cv::CAP_MSMF);
    if (!cap.isOpened()) cap.open(argv[1], cv::CAP_ANY);
    if (!cap.isOpened()) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 1;
    }
    const int width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    const int height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    if (width <= 0 || height <= 0) return 1;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::string error;
    if (!obs::create_device(&device, &context, &error)) {
        std::cerr << error << "\n";
        return 1;
    }

    const wchar_t kClass[] = L"OBScureTransportPlayer";
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, kClass, L"OBScure Transport Player", WS_POPUP, 0, 0,
                                width, height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOWNA);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    UpdateWindow(hwnd);

    ComPtr<IDXGIDevice> dxgi_device;
    device->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()));
    ComPtr<IDXGIAdapter> adapter;
    dxgi_device->GetAdapter(adapter.GetAddressOf());
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> swapchain;
    if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr,
                                               swapchain.GetAddressOf()))) {
        return 1;
    }
    ComPtr<ID3D11Texture2D> source_tex = obs::create_texture(
        device.Get(), width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
        nullptr, 0);
    if (!source_tex) return 1;

    const auto frame_duration = std::chrono::duration<double>(1.0 / 60.0);
    const auto start = Clock::now();
    uint64_t frame_index = 0;
    while (!g_close.load()) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_close.store(true);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_close.load()) break;

        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) {
            cap.release();
            cap.open(argv[1], cv::CAP_MSMF);
            if (!cap.isOpened()) cap.open(argv[1], cv::CAP_ANY);
            if (!cap.isOpened() || !cap.read(frame) || frame.empty()) break;
        }
        if (frame.cols != width || frame.rows != height) {
            cv::resize(frame, frame, cv::Size(width, height));
        }
        cv::Mat bgra;
        cv::cvtColor(frame, bgra, cv::COLOR_BGR2BGRA);
        if (!bgra.isContinuous()) bgra = bgra.clone();
        context->UpdateSubresource(source_tex.Get(), 0, nullptr, bgra.data, width * 4, 0);

        ComPtr<ID3D11Texture2D> backbuffer;
        swapchain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()));
        context->CopyResource(backbuffer.Get(), source_tex.Get());
        swapchain->Present(1, 0);
        ++frame_index;

        const auto next = start + std::chrono::duration_cast<Clock::duration>(
                                      frame_duration * static_cast<double>(frame_index));
        std::this_thread::sleep_until(next);
    }
    return 0;
}
