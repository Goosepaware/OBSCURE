#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace obs {

struct PreviewPlayerCommand {
    enum class Type { None, TogglePause, ToggleLoop, Seek };
    Type type = Type::None;
    double value = 0.0;
};

// Resizable top-level preview window used when the playback picture is
// detached from the main console. The picture is letterboxed to preserve its
// aspect ratio. A compact player bar (play/pause, seek, time, loop) sits below
// the video. F8 toggles always-on-top; WM_CLOSE hides the window so the
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

    // Player bar. visible=false hides the bar and gives the full client area to
    // the video. Commands are applied by the main loop via take_player_command.
    void set_player_state(bool visible, bool running, bool paused, bool loop,
                          double position_ms, double duration_ms, double fps);
    bool take_player_command(PreviewPlayerCommand* command);

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool create_swapchain();
    void resize_swapchain(int width, int height);
    void layout_player();
    void update_player_controls();
    static std::wstring format_time(double ms);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> rtvs_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    HWND hwnd_ = nullptr;
    HWND video_hwnd_ = nullptr;
    HWND play_button_ = nullptr;
    HWND loop_check_ = nullptr;
    HWND trackbar_ = nullptr;
    HWND time_label_ = nullptr;
    HBRUSH dark_brush_ = nullptr;
    bool topmost_ = false;
    std::wstring title_;

    bool player_visible_ = false;
    bool player_running_ = false;
    bool player_paused_ = false;
    bool player_loop_ = true;
    double player_position_ms_ = 0.0;
    double player_duration_ms_ = 0.0;
    double player_fps_ = 0.0;
    double drag_position_ms_ = 0.0;
    bool trackbar_dragging_ = false;
    ULONGLONG last_seek_tick_ = 0;
    int video_w_ = 0;
    int video_h_ = 0;
    std::wstring last_time_text_;
    bool last_paused_ = false;
    bool last_loop_ = true;
    bool last_visible_ = false;
    PreviewPlayerCommand pending_command_;
};

}  // namespace obs
