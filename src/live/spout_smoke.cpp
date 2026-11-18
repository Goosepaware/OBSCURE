// Spout/OBS connectivity smoke test: create a D3D11 texture and send it.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <vector>

#include "SpoutDX.h"

using Microsoft::WRL::ComPtr;

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, 2, D3D11_SDK_VERSION, device.GetAddressOf(), &level,
        context.GetAddressOf());
    if (FAILED(hr)) {
        std::printf("D3D11CreateDevice failed 0x%08lx\n", hr);
        return 1;
    }

    const int W = 1920;
    const int H = 1080;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = W;
    desc.Height = H;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<unsigned char> pixels(static_cast<size_t>(W) * H * 4);
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = pixels.data();
    init.SysMemPitch = W * 4;

    ComPtr<ID3D11Texture2D> texture;
    hr = device->CreateTexture2D(&desc, &init, texture.GetAddressOf());
    if (FAILED(hr)) {
        std::printf("CreateTexture2D failed 0x%08lx\n", hr);
        return 1;
    }

    spoutDX sender;
    if (!sender.OpenDirectX11(device.Get())) {
        std::printf("Spout OpenDirectX11 failed\n");
        return 1;
    }
    sender.SetSenderFormat(DXGI_FORMAT_B8G8R8A8_UNORM);
    sender.SetSenderName("OBScureTest");
    std::printf("sender initialized=%d name=%s\n", sender.IsInitialized() ? 1 : 0,
                sender.GetName());

    for (int frame = 0; frame < 600; ++frame) {
        for (int y = 0; y < H; ++y) {
            unsigned char* row = pixels.data() + static_cast<size_t>(y) * W * 4;
            for (int x = 0; x < W; ++x) {
                const int bar = (x + frame * 8) % W;
                row[x * 4 + 0] = static_cast<unsigned char>(bar * 255 / W);
                row[x * 4 + 1] = static_cast<unsigned char>(y * 255 / H);
                row[x * 4 + 2] = static_cast<unsigned char>((x + y) & 255);
                row[x * 4 + 3] = 255;
            }
        }
        context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), W * 4, 0);
        sender.SendTexture(texture.Get());
        if ((frame % 60) == 0) {
            std::printf("frame %d initialized=%d fps=%.1f\n", frame,
                        sender.IsInitialized() ? 1 : 0, sender.GetFps());
        }
        Sleep(16);
    }
    sender.ReleaseSender();
    CoUninitialize();
    std::printf("done\n");
    return 0;
}
