#pragma once

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace obs {

// A borderless output surface with an exact pixel client area. The window is
// intentionally opaque and non-layered so OBS Window Capture (WGC) can pick it
// up even when it is covered by OBS itself.
class OutputWindow {
public:
    OutputWindow() = default;
    ~OutputWindow();
    OutputWindow(const OutputWindow&) = delete;
    OutputWindow& operator=(const OutputWindow&) = delete;

    bool init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height,
              const std::wstring& title, const std::wstring& shader_path,
              std::string* error);
    void shutdown();

    void present(ID3D11ShaderResourceView* texture);
    void show();
    void hide();
    void set_bottom();
    void set_topmost(bool topmost);
    void move_to_monitor(int index);
    void center_on_primary();

    HWND hwnd() const { return hwnd_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool visible() const;
    bool minimized() const;

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool create_swapchain();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain3_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> rtvs_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    std::wstring title_;
};

}  // namespace obs
