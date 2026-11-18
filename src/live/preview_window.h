#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace obs {

// Resizable top-level preview window used when the playback picture is
// detached from the main console. The picture is letterboxed to preserve its
// aspect ratio. F8 toggles always-on-top; WM_CLOSE hides the window so the
// console can show it again.
class PreviewWindow {
public:
    PreviewWindow() = default;
    ~PreviewWindow();
    PreviewWindow(const PreviewWindow&) = delete;
    PreviewWindow& operator=(const PreviewWindow&) = delete;

    bool init(ID3D11Device* device, ID3D11DeviceContext* context, const RECT& rect,
              const std::wstring& title, const std::wstring& shader_path,
              std::string* error);
    void shutdown();

    void present(ID3D11ShaderResourceView* texture, int src_w, int src_h);
    void show();
    void hide();
    void set_topmost(bool topmost);
    bool topmost() const { return topmost_; }
    bool visible() const;
    bool minimized() const;
    HWND hwnd() const { return hwnd_; }
    void get_window_rect(RECT* rect) const;
    void set_window_rect(const RECT& rect);
    void set_title(const std::wstring& title);

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool create_swapchain();
    void resize_swapchain(int width, int height);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> rtvs_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    HWND hwnd_ = nullptr;
    bool topmost_ = false;
    std::wstring title_;
};

}  // namespace obs
