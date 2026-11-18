#include "gui/gui_app.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "core/detect.h"
#include "core/key.h"
#include "core/layout.h"
#include "io/ffmpeg_sink.h"
#include "live/d3d_helpers.h"
#include "live/decode_pipeline.h"
#include "live/detection_worker.h"
#include "live/encode_pipeline.h"
#include "live/file_source.h"
#include "live/output_window.h"
#include "live/shader_path.h"
#include "live/spout_sink.h"
#include "live/vcam_registry.h"
#include "live/vcam_sink.h"
#include "live/wgc_capture.h"
#include "platform/win_util.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace obs {

namespace {

const ImVec4 kAccent(0.10f, 0.72f, 0.64f, 1.0f);
const ImVec4 kAccentDim(0.08f, 0.45f, 0.42f, 1.0f);
const ImVec4 kRed(0.95f, 0.36f, 0.36f, 1.0f);
const ImVec4 kGreen(0.36f, 0.85f, 0.50f, 1.0f);
const ImVec4 kYellow(0.95f, 0.75f, 0.30f, 1.0f);
const ImVec4 kMuted(0.55f, 0.60f, 0.66f, 1.0f);

const wchar_t kVideoFilter[] =
    L"视频文件\0*.mp4;*.mkv;*.mov;*.avi;*.ts\0所有文件\0*.*\0";

struct DpiScope {
    DPI_AWARENESS_CONTEXT old = nullptr;
    DpiScope() { old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); }
    ~DpiScope() {
        if (old) SetThreadDpiAwarenessContext(old);
    }
};

struct Preset {
    const char* id;
    const char* label;
    int cell;
    int guard;
    double scale;
    int bitrate;
};

constexpr Preset kPresets[] = {
    {"c32_g2_s100", "高质量 32px / 1.0x", 32, 2, 1.0, 10},
    {"c32_g2_s120", "平衡 32px / 1.2x（推荐）", 32, 2, 1.2, 8},
    {"c32_g2_s140", "隐私 32px / 1.4x", 32, 2, 1.4, 6},
    {"c16_g2_s100", "强隐私 16px / 2px", 16, 2, 1.0, 10},
    {"c16_g1_s100", "强隐私 16px / 1px", 16, 1, 1.0, 10},
    {"c64_g2_s100", "画质优先 64px / 1.0x", 64, 2, 1.0, 12},
    {"c64_g2_s120", "画质优先 64px / 1.2x", 64, 2, 1.2, 12},
};
constexpr int kPresetCount = static_cast<int>(std::size(kPresets));

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string now_text() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &tm);
    return buffer;
}

cv::Mat identity_scaled(int width, int height) {
    cv::Mat H = cv::Mat::eye(3, 3, CV_64F);
    H.at<double>(0, 0) = static_cast<double>(width) / 1920.0;
    H.at<double>(1, 1) = static_cast<double>(height) / 1080.0;
    return H;
}

cv::Point2d map_point(const cv::Mat& M, double x, double y) {
    const double w = M.at<double>(2, 0) * x + M.at<double>(2, 1) * y + M.at<double>(2, 2);
    return cv::Point2d(
        (M.at<double>(0, 0) * x + M.at<double>(0, 1) * y + M.at<double>(0, 2)) / w,
        (M.at<double>(1, 0) * x + M.at<double>(1, 1) * y + M.at<double>(1, 2)) / w);
}

double content_delta(const cv::Mat& a, const cv::Mat& b, const Layout& layout) {
    if (a.empty() || b.empty()) return 999.0;
    const double xs[4] = {
        static_cast<double>(layout.content_x),
        static_cast<double>(layout.content_x + layout.content_w),
        static_cast<double>(layout.content_x + layout.content_w),
        static_cast<double>(layout.content_x),
    };
    const double ys[4] = {
        static_cast<double>(layout.content_y),
        static_cast<double>(layout.content_y),
        static_cast<double>(layout.content_y + layout.content_h),
        static_cast<double>(layout.content_y + layout.content_h),
    };
    double max_delta = 0.0;
    for (int i = 0; i < 4; ++i) {
        const auto pa = map_point(a, xs[i], ys[i]);
        const auto pb = map_point(b, xs[i], ys[i]);
        max_delta = std::max(max_delta, std::hypot(pa.x - pb.x, pa.y - pb.y));
    }
    return max_delta;
}

uint32_t key_id_for_text(const std::string& key) {
    const auto digest = sha256("OBSCure-key-id|" + key);
    return (static_cast<uint32_t>(digest[0]) << 24) |
           (static_cast<uint32_t>(digest[1]) << 16) |
           (static_cast<uint32_t>(digest[2]) << 8) |
           static_cast<uint32_t>(digest[3]);
}

std::string window_label(const WindowInfo& info) {
    std::ostringstream os;
    os << wide_to_utf8(info.title) << "  ·  " << wide_to_utf8(info.process) << "  ·  "
       << info.width << "x" << info.height;
    if (info.minimized) os << "  [最小化]";
    return os.str();
}

}  // namespace

struct GuiApp::Impl {
    enum class Mode { Encode, Decode };

    struct EncodeState {
        enum class Source { Window, Monitor, File };
        Source source = Source::Window;
        HWND hwnd = nullptr;
        std::wstring hwnd_title;
        int monitor_index = 0;
        std::wstring file_path;
        bool out_window = true;
        bool out_spout = false;
        bool out_vcam = false;
        bool out_record = false;
        std::wstring record_path;
        std::string spout_name = "OBScure";
        bool running = false;
        std::unique_ptr<EncodePipeline> encoder;
        std::unique_ptr<OutputWindow> output_window;
        std::unique_ptr<SpoutSink> spout;
        std::unique_ptr<VcamSink> vcam;
        std::unique_ptr<FfmpegSink> recorder;
        std::unique_ptr<WgcCapture> capture;
        FileSource file;
        cv::Mat last_file_frame;
        ComPtr<ID3D11Texture2D> source_tex;
        ComPtr<ID3D11ShaderResourceView> source_srv;
        int tex_w = 0;
        int tex_h = 0;
        DXGI_FORMAT tex_format = DXGI_FORMAT_UNKNOWN;
        int source_w = 0;
        int source_h = 0;
        std::string error;
        bool show_transport = false;
        uint64_t stat_frames = 0;
        double fps = 0.0;
        Clock::time_point stat_start{};
        Clock::time_point last_file_log{};
        Layout layout;
        KeyMaterial key;
    };

    struct DecodeState {
        enum class Source { File, Window, Monitor };
        Source source = Source::File;
        std::wstring file_path;
        HWND hwnd = nullptr;
        std::wstring hwnd_title;
        int monitor_index = 0;
        bool detect_file = false;
        bool fullscreen = false;
        bool running = false;
        std::unique_ptr<DecodePipeline> decoder;
        std::unique_ptr<WgcCapture> capture;
        std::unique_ptr<DetectionWorker> detector;
        FileSource file;
        cv::Mat last_file_frame;
        ComPtr<ID3D11Texture2D> source_tex;
        ComPtr<ID3D11ShaderResourceView> source_srv;
        ComPtr<ID3D11Texture2D> staging;
        int tex_w = 0;
        int tex_h = 0;
        DXGI_FORMAT tex_format = DXGI_FORMAT_UNKNOWN;
        int source_w = 0;
        int source_h = 0;
        cv::Mat H;
        cv::Mat H_initial;
        int margin = 0;
        bool locked = false;
        bool key_ok = false;
        int markers = 0;
        double detect_ms = 0.0;
        int detect_tick = 0;
        cv::Mat last_detect_frame;
        Clock::time_point last_fail_dump{};
        Clock::time_point last_detect_log{};
        std::unique_ptr<OutputWindow> fullscreen_window;
        std::string error;
        bool show_capture = false;
        uint64_t stat_frames = 0;
        double fps = 0.0;
        Clock::time_point stat_start{};
        Clock::time_point last_file_log{};
        Layout layout;
        KeyMaterial key;
    };

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    int win_w_ = 1280;
    int win_h_ = 820;
    bool running_ = true;
    bool pending_resize_ = false;
    int resize_w_ = 0;
    int resize_h_ = 0;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<IDXGISwapChain3> swapchain3_;
    std::vector<ComPtr<ID3D11RenderTargetView>> rtvs_;

    Mode mode_ = Mode::Encode;
    char key_buf_[128] = "obscure-demo-key";
    int preset_index_ = 1;
int record_bitrate_ = 8;
    EncodeState enc_;
    DecodeState dec_;
    std::vector<MonitorInfo> monitors_;
    std::string last_error_;
    std::ofstream log_;

    bool picker_open_ = false;
    bool picker_for_decode_ = false;
    std::vector<WindowInfo> picker_windows_;
    int picker_selected_ = -1;
    char picker_search_[128]{};
    std::unique_ptr<WgcCapture> picker_capture_;
    HWND picker_preview_hwnd_ = nullptr;
    ComPtr<ID3D11Texture2D> picker_tex_;
    ComPtr<ID3D11ShaderResourceView> picker_srv_;
    int picker_tex_w_ = 0;
    int picker_tex_h_ = 0;
    DXGI_FORMAT picker_tex_format_ = DXGI_FORMAT_UNKNOWN;
    std::string picker_error_;

    std::string ui_key_cache_;
    uint32_t ui_key_id_ = 0;
    Clock::time_point next_frame_{};

    // Development / shortcut startup options.
    bool auto_start_ = false;
    bool auto_decode_ = false;
    bool auto_detect_ = false;
    bool auto_spout_ = false;
    bool auto_vcam_ = false;
    bool auto_record_ = false;
    std::wstring auto_file_;
    std::wstring auto_window_;
    std::wstring auto_out_;

    int run();
    bool init(HINSTANCE instance);
    void shutdown();
    void open_log();
    void log_line(const std::string& text);
    void set_error(const std::string& text);

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool init_window(HINSTANCE instance);
    bool init_d3d();
    bool create_swapchain();
    bool init_imgui();
    void resize_swapchain(int width, int height);
    void render_frame();

    void draw();
    void draw_header();
    void draw_encode();
    void draw_decode();
    void draw_encode_source();
    void draw_encode_outputs();
    void draw_presets();
    void draw_key();
    void draw_encode_preview();
    void draw_decode_source();
    void draw_decode_preview();
    void draw_encode_actions();
    void draw_decode_actions();
    void draw_status_bar();
    void draw_picker();
    void draw_image_fit(ID3D11ShaderResourceView* srv, int width, int height,
                        const char* label, float footer = 28.0f);

    void open_picker(bool for_decode);
    void start_picker_preview(int index);
    void stop_picker_preview();
    void update_picker_preview();

    bool start_encode();
    void stop_encode();
    void update_encode();
    bool start_decode();
    void stop_decode();
    void update_decode();
    void apply_detection(const DetectionOutcome& outcome);
    bool read_source_bgr(cv::Mat& bgr);

    void upload_bgr(const cv::Mat& bgr, ComPtr<ID3D11Texture2D>& texture,
                    ComPtr<ID3D11ShaderResourceView>& srv, int& width, int& height);
    std::wstring make_record_path();
    uint32_t key_id();
    void parse_command_line();
    void apply_auto_start();
};

int GuiApp::Impl::run() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool winrt_ready = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        winrt_ready = true;
    } catch (const winrt::hresult_error&) {
        // WGC will report a useful error later if this really failed.
    }

    if (!init(GetModuleHandleW(nullptr))) {
        shutdown();
        if (winrt_ready) winrt::uninit_apartment();
        return 1;
    }

    next_frame_ = Clock::now();
    while (running_) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running_ = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running_) break;

        if (pending_resize_) {
            resize_swapchain(resize_w_, resize_h_);
            pending_resize_ = false;
        }

        try {
            if (enc_.running) update_encode();
            if (dec_.running) update_decode();
        } catch (const std::exception& e) {
            set_error(std::string("运行错误: ") + e.what());
            if (enc_.running) stop_encode();
            if (dec_.running) stop_decode();
        }
        if (picker_open_) update_picker_preview();

        if (!IsIconic(hwnd_) && win_w_ > 0 && win_h_ > 0) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            draw();
            ImGui::Render();
            render_frame();
        }

        next_frame_ += std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / 60.0));
        const auto now = Clock::now();
        if (next_frame_ > now) {
            std::this_thread::sleep_until(next_frame_);
        } else if (now - next_frame_ > std::chrono::milliseconds(250)) {
            next_frame_ = now;
        }
    }

    shutdown();
    if (winrt_ready) winrt::uninit_apartment();
    return 0;
}

bool GuiApp::Impl::init(HINSTANCE instance) {
    instance_ = instance;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    open_log();
    monitors_ = enumerate_monitors();
    ui_key_cache_ = key_buf_;
    ui_key_id_ = key_id_for_text(ui_key_cache_);
    if (!init_window(instance)) return false;
    if (!init_d3d()) return false;
    if (!init_imgui()) return false;
    log_line("GUI started");
    parse_command_line();
    apply_auto_start();
    return true;
}

void GuiApp::Impl::parse_command_line() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--file" && i + 1 < argc) {
            auto_file_ = argv[++i];
        } else if (arg == L"--window" && i + 1 < argc) {
            auto_window_ = argv[++i];
        } else if (arg == L"--out" && i + 1 < argc) {
            auto_out_ = argv[++i];
        } else if (arg == L"--decode") {
            auto_decode_ = true;
        } else if (arg == L"--detect") {
            auto_detect_ = true;
        } else if (arg == L"--preset" && i + 1 < argc) {
            const std::string id = wide_to_utf8(argv[++i]);
            for (int j = 0; j < kPresetCount; ++j) {
                if (id == kPresets[j].id) preset_index_ = j;
            }
        } else if (arg == L"--auto") {
            auto_start_ = true;
        } else if (arg == L"--spout") {
            auto_spout_ = true;
        } else if (arg == L"--vcam") {
            auto_vcam_ = true;
        } else if (arg == L"--record") {
            auto_record_ = true;
        }
    }
    LocalFree(argv);
}

void GuiApp::Impl::apply_auto_start() {
    if (!auto_start_) return;
    if (auto_file_.empty() && auto_window_.empty()) return;

    HWND hwnd = nullptr;
    std::wstring title;
    if (!auto_window_.empty()) {
        if (!find_window_by_title(auto_window_, &hwnd, &title)) {
            set_error("找不到启动参数指定的窗口。");
            return;
        }
    }

    if (auto_decode_) {
        mode_ = Mode::Decode;
        dec_.detect_file = auto_detect_;
        dec_.fullscreen = false;
        if (hwnd) {
            dec_.source = DecodeState::Source::Window;
            dec_.hwnd = hwnd;
            dec_.hwnd_title = title;
        } else {
            dec_.source = DecodeState::Source::File;
            dec_.file_path = auto_file_;
        }
        start_decode();
    } else {
        mode_ = Mode::Encode;
        enc_.out_window = true;
        enc_.out_spout = auto_spout_;
        enc_.out_vcam = auto_vcam_;
        enc_.out_record = auto_record_;
        if (hwnd) {
            enc_.source = EncodeState::Source::Window;
            enc_.hwnd = hwnd;
            enc_.hwnd_title = title;
        } else {
            enc_.source = EncodeState::Source::File;
            enc_.file_path = auto_file_;
        }
        if (!auto_out_.empty()) {
            enc_.record_path = auto_out_;
        } else if (auto_record_) {
            enc_.record_path = make_record_path();
        }
        start_encode();
    }
}

void GuiApp::Impl::shutdown() {
    stop_encode();
    stop_decode();
    stop_picker_preview();
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    rtvs_.clear();
    swapchain3_.Reset();
    swapchain_.Reset();
    context_.Reset();
    device_.Reset();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (log_.is_open()) {
        log_line("GUI stopped");
        log_.close();
    }
}

void GuiApp::Impl::open_log() {
    const fs::path dir = fs::path(app_data_dir()) / "logs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    log_.open(dir / "obscure-gui.log", std::ios::app | std::ios::binary);
}

void GuiApp::Impl::log_line(const std::string& text) {
    const std::string line = now_text() + " " + text + "\n";
    OutputDebugStringA(line.c_str());
    if (log_.is_open()) {
        log_ << line;
        log_.flush();
    }
}

void GuiApp::Impl::set_error(const std::string& text) {
    last_error_ = text;
    log_line("ERROR: " + text);
}

uint32_t GuiApp::Impl::key_id() {
    if (ui_key_cache_ != key_buf_) {
        ui_key_cache_ = key_buf_;
        ui_key_id_ = key_id_for_text(ui_key_cache_);
    }
    return ui_key_id_;
}

LRESULT CALLBACK GuiApp::Impl::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_SIZE:
            if (self && wp != SIZE_MINIMIZED) {
                self->pending_resize_ = true;
                self->resize_w_ = LOWORD(lp);
                self->resize_h_ = HIWORD(lp);
            }
            return 0;
        case WM_CLOSE:
            if (self) self->running_ = false;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_DPICHANGED: {
            auto* rc = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                         rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize.x = 1000;
            info->ptMinTrackSize.y = 680;
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool GuiApp::Impl::init_window(HINSTANCE instance) {
    DpiScope dpi_scope;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Impl::wnd_proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"OBScureGuiWindow";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        set_error("无法注册 GUI 窗口类");
        return false;
    }

    RECT rc{0, 0, win_w_, win_h_};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExW(0, L"OBScureGuiWindow", L"OBScure",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                            instance, this);
    if (!hwnd_) {
        set_error("无法创建主窗口");
        return false;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    win_w_ = client.right - client.left;
    win_h_ = client.bottom - client.top;
    SetWindowTextW(hwnd_, L"OBScure");
    PROCESS_DPI_AWARENESS awareness = PROCESS_DPI_UNAWARE;
    GetProcessDpiAwareness(nullptr, &awareness);
    RECT window_rect{};
    GetWindowRect(hwnd_, &window_rect);
    log_line("window client " + std::to_string(win_w_) + "x" + std::to_string(win_h_) +
             " rect " + std::to_string(window_rect.right - window_rect.left) + "x" +
             std::to_string(window_rect.bottom - window_rect.top) + " dpi " +
             std::to_string(GetDpiForWindow(hwnd_)) + " awareness " +
             std::to_string(static_cast<int>(awareness)) + " thread_awareness " +
             std::to_string(GetAwarenessFromDpiAwarenessContext(
                 GetThreadDpiAwarenessContext())));
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

bool GuiApp::Impl::init_d3d() {
    std::string error;
    if (!create_device(&device_, &context_, &error)) {
        set_error(error);
        return false;
    }
    if (!create_swapchain()) {
        set_error("无法创建主窗口交换链");
        return false;
    }
    return true;
}

bool GuiApp::Impl::create_swapchain() {
    ComPtr<IDXGIDevice> dxgi_device;
    HRESULT step_hr = device_->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()));
    if (FAILED(step_hr)) {
        char buffer[128]{};
        std::snprintf(buffer, sizeof(buffer), "Query IDXGIDevice failed 0x%08lX",
                      static_cast<unsigned long>(step_hr));
        log_line(buffer);
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    step_hr = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(step_hr)) {
        char buffer[128]{};
        std::snprintf(buffer, sizeof(buffer), "GetAdapter failed 0x%08lX",
                      static_cast<unsigned long>(step_hr));
        log_line(buffer);
        return false;
    }
    ComPtr<IDXGIFactory2> factory;
    step_hr = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(step_hr)) {
        char buffer[128]{};
        std::snprintf(buffer, sizeof(buffer), "GetParent factory failed 0x%08lX",
                      static_cast<unsigned long>(step_hr));
        log_line(buffer);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(win_w_);
    desc.Height = static_cast<UINT>(win_h_);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    const HRESULT create_hr = factory->CreateSwapChainForHwnd(
        device_.Get(), hwnd_, &desc, nullptr, nullptr, swapchain_.GetAddressOf());
    if (FAILED(create_hr)) {
        char buffer[128]{};
        std::snprintf(buffer, sizeof(buffer),
                      "CreateSwapChainForHwnd failed 0x%08lX (client %dx%d)",
                      static_cast<unsigned long>(create_hr), win_w_, win_h_);
        log_line(buffer);
        return false;
    }
    const HRESULT as_hr = swapchain_.As(&swapchain3_);
    if (FAILED(as_hr)) {
        char buffer[128]{};
        std::snprintf(buffer, sizeof(buffer), "Query IDXGISwapChain3 failed 0x%08lX",
                      static_cast<unsigned long>(as_hr));
        log_line(buffer);
        return false;
    }
    rtvs_.clear();
    ComPtr<ID3D11Texture2D> buffer;
    const HRESULT buffer_hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf()));
    if (FAILED(buffer_hr)) {
        char text[128]{};
        std::snprintf(text, sizeof(text), "GetBuffer(0) failed 0x%08lX",
                      static_cast<unsigned long>(buffer_hr));
        log_line(text);
        return false;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    const HRESULT rtv_hr =
        device_->CreateRenderTargetView(buffer.Get(), nullptr, rtv.GetAddressOf());
    if (FAILED(rtv_hr)) {
        char text[128]{};
        std::snprintf(text, sizeof(text), "CreateRenderTargetView(0) failed 0x%08lX",
                      static_cast<unsigned long>(rtv_hr));
        log_line(text);
        return false;
    }
    rtvs_.push_back(std::move(rtv));
    return true;
}

void GuiApp::Impl::resize_swapchain(int width, int height) {
    if (!swapchain_ || width <= 0 || height <= 0) return;
    if (width == win_w_ && height == win_h_ && !rtvs_.empty()) return;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtvs_.clear();
    if (FAILED(swapchain_->ResizeBuffers(0, static_cast<UINT>(width),
                                         static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN,
                                         0))) {
        set_error("交换链 resize 失败");
        return;
    }
    win_w_ = width;
    win_h_ = height;
    ComPtr<ID3D11Texture2D> buffer;
    if (SUCCEEDED(swapchain_->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf())))) {
        ComPtr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(device_->CreateRenderTargetView(buffer.Get(), nullptr,
                                                      rtv.GetAddressOf()))) {
            rtvs_.push_back(std::move(rtv));
        }
    }
}

bool GuiApp::Impl::init_imgui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.WindowPadding = ImVec2(14, 12);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.062f, 0.078f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.085f, 0.105f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.075f, 0.085f, 0.105f, 1.0f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.18f, 0.21f, 0.26f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.125f, 0.155f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.17f, 0.21f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.10f, 0.35f, 0.33f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.13f, 0.15f, 0.19f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.17f, 0.21f, 0.26f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = kAccentDim;
    style.Colors[ImGuiCol_Header] = ImVec4(0.10f, 0.30f, 0.29f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.12f, 0.40f, 0.38f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.08f, 0.50f, 0.46f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = kAccent;
    style.Colors[ImGuiCol_SliderGrab] = kAccent;
    style.Colors[ImGuiCol_SliderGrabActive] = kAccent;
    style.Colors[ImGuiCol_Separator] = ImVec4(0.18f, 0.21f, 0.26f, 1.0f);
    style.Colors[ImGuiCol_Text] = ImVec4(0.88f, 0.90f, 0.93f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = kMuted;

    ImFontConfig font_config;
    font_config.OversampleH = 2;
    font_config.OversampleV = 2;
    font_config.PixelSnapH = true;
    ImFont* font = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\msyh.ttc", 18.0f, &font_config,
        io.Fonts->GetGlyphRangesChineseFull());
    if (!font) {
        io.Fonts->AddFontDefault();
    }

    if (!ImGui_ImplWin32_Init(hwnd_)) {
        set_error("ImGui Win32 后端初始化失败");
        return false;
    }
    if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) {
        set_error("ImGui D3D11 后端初始化失败");
        return false;
    }
    return true;
}

void GuiApp::Impl::render_frame() {
    if (!swapchain_ || rtvs_.empty()) return;
    const float clear[4] = {0.045f, 0.052f, 0.065f, 1.0f};
    context_->OMSetRenderTargets(1, rtvs_[0].GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtvs_[0].Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT hr = swapchain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        set_error("D3D 设备丢失，请重启程序");
        running_ = false;
    }
}

void GuiApp::Impl::draw() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar;
    ImGui::Begin("##root", nullptr, flags);
    draw_header();

    if (!last_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.25f, 0.08f, 0.08f, 1.0f));
        ImGui::BeginChild("##error", ImVec2(0, 48), ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", last_error_.c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 76);
        if (ImGui::Button("关闭##err")) last_error_.clear();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    if (mode_ == Mode::Encode) {
        draw_encode();
    } else {
        draw_decode();
    }
    draw_status_bar();
    ImGui::End();

    if (picker_open_) draw_picker();
}

void GuiApp::Impl::draw_header() {
    const ImVec4 accent = kAccent;
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::TextUnformatted("OBSCure");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("实时画面混淆");
    ImGui::SameLine(220.0f);

    auto tab = [this](const char* label, Mode target) {
        const bool active = mode_ == target;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        }
        const bool clicked = ImGui::Button(label, ImVec2(84, 30));
        if (active) ImGui::PopStyleColor(2);
        if (clicked) mode_ = target;
    };
    tab("编码", Mode::Encode);
    ImGui::SameLine();
    tab("解码", Mode::Decode);

    ImGui::SameLine();
    ImGui::TextDisabled("keyID %08X", key_id());
    ImGui::SameLine();
    const bool running = enc_.running || dec_.running;
    ImGui::TextColored(running ? kGreen : kYellow, running ? "● 运行中" : "● 就绪");
}

void GuiApp::Impl::draw_encode() {
    const float content_h = std::max(240.0f, ImGui::GetContentRegionAvail().y - 62.0f);
    ImGui::BeginChild("##enc_left", ImVec2(370, content_h), ImGuiChildFlags_Borders);
    draw_encode_source();
    ImGui::Separator();
    draw_encode_outputs();
    ImGui::Separator();
    draw_presets();
    ImGui::Separator();
    draw_key();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##enc_preview", ImVec2(0, content_h), ImGuiChildFlags_Borders);
    draw_encode_preview();
    ImGui::EndChild();

    draw_encode_actions();
}

void GuiApp::Impl::draw_encode_source() {
    ImGui::TextDisabled("输入源");
    ImGui::BeginDisabled(enc_.running);
    int source = static_cast<int>(enc_.source);
    ImGui::RadioButton("窗口", &source, 0);
    ImGui::SameLine();
    ImGui::RadioButton("显示器", &source, 1);
    ImGui::SameLine();
    ImGui::RadioButton("视频文件", &source, 2);
    enc_.source = static_cast<EncodeState::Source>(source);

    if (enc_.source == EncodeState::Source::Window) {
        if (ImGui::Button("选择窗口...", ImVec2(-1, 0))) open_picker(false);
        if (enc_.hwnd && IsWindow(enc_.hwnd)) {
            ImGui::TextWrapped("%s", wide_to_utf8(enc_.hwnd_title).c_str());
            ImGui::TextDisabled("%dx%d", enc_.source_w, enc_.source_h);
            if (IsIconic(enc_.hwnd)) {
                ImGui::TextColored(kYellow, "窗口已最小化，恢复后可捕获");
            }
        } else if (enc_.hwnd) {
            ImGui::TextColored(kRed, "窗口已关闭，请重新选择");
        } else {
            ImGui::TextDisabled("未选择窗口");
        }
    } else if (enc_.source == EncodeState::Source::Monitor) {
        std::string preview = "选择显示器";
        for (const auto& m : monitors_) {
            if (m.index == enc_.monitor_index) {
                preview = "显示器 " + std::to_string(m.index + 1) + " (" +
                          std::to_string(m.width) + "x" + std::to_string(m.height) + ")";
                break;
            }
        }
        if (ImGui::BeginCombo("##monitor", preview.c_str())) {
            for (const auto& m : monitors_) {
                const std::string label =
                    "显示器 " + std::to_string(m.index + 1) + " (" +
                    std::to_string(m.width) + "x" + std::to_string(m.height) + ")";
                if (ImGui::Selectable(label.c_str(), enc_.monitor_index == m.index)) {
                    enc_.monitor_index = m.index;
                }
            }
            ImGui::EndCombo();
        }
    } else {
        if (ImGui::Button("打开视频文件...", ImVec2(-1, 0))) {
            const auto path = open_file_dialog(L"选择源视频", kVideoFilter);
            if (!path.empty()) {
                enc_.file_path = path;
                enc_.source_w = 0;
                enc_.source_h = 0;
            }
        }
        if (!enc_.file_path.empty()) {
            ImGui::TextWrapped("%s", wide_to_utf8(enc_.file_path).c_str());
        } else {
            ImGui::TextDisabled("未选择视频文件");
        }
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("窗口捕获需要窗口可见；独占全屏游戏建议用无边框模式。");
}

void GuiApp::Impl::draw_encode_outputs() {
    ImGui::TextDisabled("输出");
    ImGui::BeginDisabled(enc_.running);
    ImGui::Checkbox("输出窗口（OBS 窗口采集）", &enc_.out_window);
    ImGui::Checkbox("Spout（需 OBS 插件）", &enc_.out_spout);
    if (enc_.out_spout) {
        char name[128]{};
        strncpy_s(name, enc_.spout_name.c_str(), _TRUNCATE);
        if (ImGui::InputText("Spout 名称", name, sizeof(name))) {
            enc_.spout_name = name;
        }
    }
    ImGui::Checkbox("虚拟摄像头（OBScure Virtual Camera）", &enc_.out_vcam);
    ImGui::TextDisabled("独立 DirectShow 设备，输出 NV12（优先）/ YUY2 / RGB。");
    ImGui::TextDisabled("OBS 里添加“视频采集设备”即可，不需要窗口采集。");
    ImGui::Checkbox("录制到文件", &enc_.out_record);
    if (enc_.out_record) {
        if (enc_.record_path.empty()) enc_.record_path = make_record_path();
        ImGui::TextWrapped("%s", wide_to_utf8(enc_.record_path).c_str());
        if (ImGui::Button("选择录制路径...", ImVec2(-1, 0))) {
            const auto path = save_file_dialog(L"保存传输视频", kVideoFilter,
                                               L"transport.mp4");
            if (!path.empty()) enc_.record_path = path;
        }
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputInt("录制码率 (Mbps)", &record_bitrate_)) {
            record_bitrate_ = std::clamp(record_bitrate_, 2, 50);
        }
        ImGui::TextDisabled("直播参考：1080p60 约 6–8 Mbps，720p60 约 4–6 Mbps。");
    }
    ImGui::EndDisabled();

    if (enc_.output_window) {
        ImGui::TextDisabled("输出窗口：%s",
                            enc_.output_window->visible()
                                ? (enc_.output_window->minimized() ? "已最小化" : "显示中")
                                : "已隐藏");
        if (ImGui::Button("显示窗口")) enc_.output_window->show();
        ImGui::SameLine();
        if (ImGui::Button("置底")) enc_.output_window->set_bottom();
    }
    if (enc_.out_spout) {
        const bool ok = enc_.spout && enc_.spout->initialized();
        ImGui::TextColored(ok ? kGreen : kYellow, "Spout: %s",
                           ok ? "已连接（等待 OBS 也可）" : "等待连接");
    }
    if (enc_.out_record && enc_.recorder) {
        ImGui::TextColored(enc_.recorder->active() ? kGreen : kRed, "录制: %s",
                           enc_.recorder->active() ? "进行中" : "已停止");
    }
}

void GuiApp::Impl::draw_presets() {
    ImGui::TextDisabled("画质档位");
    ImGui::BeginDisabled(enc_.running || dec_.running);
    const char* preview = kPresets[preset_index_].label;
    if (ImGui::BeginCombo("##preset", preview)) {
        for (int i = 0; i < kPresetCount; ++i) {
            const std::string label =
                std::string(kPresets[i].label) + "  (" + kPresets[i].id + ")";
            if (ImGui::Selectable(label.c_str(), i == preset_index_)) {
                preset_index_ = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    const auto& p = kPresets[preset_index_];
    ImGui::TextDisabled("cell %d · guard %d · scale %.1fx", p.cell, p.guard, p.scale);
    ImGui::TextDisabled("建议码率 %d Mbps", p.bitrate);
    ImGui::TextDisabled("本阶段固定算法，画质/码率后续再调。");
}

void GuiApp::Impl::draw_key() {
    ImGui::TextDisabled("共享密钥");
    ImGui::BeginDisabled(enc_.running || dec_.running);
    ImGui::InputText("##key", key_buf_, sizeof(key_buf_),
                     ImGuiInputTextFlags_Password);
    ImGui::EndDisabled();
    ImGui::TextDisabled("keyID %08X", key_id());
    ImGui::TextWrapped("两端必须一致；修改密钥后需重新开始。");
}

void GuiApp::Impl::draw_image_fit(ID3D11ShaderResourceView* srv, int width, int height,
                                  const char* label, float footer) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (width <= 0 || height <= 0) {
        ImGui::TextDisabled("%s", label);
        return;
    }
    const float max_h = std::max(60.0f, avail.y - footer);
    const float scale = std::min(avail.x / static_cast<float>(width),
                                 max_h / static_cast<float>(height));
    const ImVec2 size(static_cast<float>(width) * scale,
                      static_cast<float>(height) * scale);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 pos(origin.x + (avail.x - size.x) * 0.5f,
                     origin.y + (max_h - size.y) * 0.5f);
    ImGui::SetCursorScreenPos(pos);
    if (srv) {
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(srv)), size);
        ImGui::GetWindowDrawList()->AddRect(
            ImVec2(pos.x - 1, pos.y - 1), ImVec2(pos.x + size.x + 1, pos.y + size.y + 1),
            IM_COL32(80, 95, 115, 255), 2.0f);
    } else {
        ImGui::GetWindowDrawList()->AddRectFilled(
            pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(14, 16, 20, 255),
            8.0f);
        const char* message = "未选择源";
        const ImVec2 text_size = ImGui::CalcTextSize(message);
        ImGui::SetCursorScreenPos(ImVec2(pos.x + (size.x - text_size.x) * 0.5f,
                                         pos.y + (size.y - text_size.y) * 0.5f));
        ImGui::TextDisabled("%s", message);
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + max_h + 4.0f));
    ImGui::TextDisabled("%s", label);
}

void GuiApp::Impl::draw_encode_preview() {
    ImGui::Checkbox("显示混淆输出", &enc_.show_transport);
    ImGui::SameLine();
    ImGui::TextDisabled("预览不会影响输出");
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0;
    int height = 0;
    std::string label;
    if (enc_.show_transport && enc_.encoder) {
        srv = enc_.encoder->output_srv();
        width = 1920;
        height = 1080;
        label = "传输帧 1920x1080 · 四角标记 + 元数据";
    } else if (enc_.source_srv) {
        srv = enc_.source_srv.Get();
        width = enc_.source_w;
        height = enc_.source_h;
        label = "源画面";
    } else {
        label = "等待输入源";
    }
    draw_image_fit(srv, width, height, label.c_str(), 52.0f);

    if (enc_.source_w > 0) {
        ImGui::TextDisabled("源 %dx%d · 渲染 %.1f fps", enc_.source_w, enc_.source_h,
                            enc_.fps);
        if (enc_.source == EncodeState::Source::File && enc_.running) {
            ImGui::SameLine();
            ImGui::TextDisabled("源 %.1f fps · 丢帧 %llu", enc_.file.fps(),
                                static_cast<unsigned long long>(enc_.file.dropped()));
        }
    } else {
        ImGui::TextDisabled("渲染 %.1f fps", enc_.fps);
    }
    if (enc_.output_window) {
        if (!enc_.output_window->visible()) {
            ImGui::TextColored(kYellow, "输出窗口已隐藏；OBS 窗口采集需要窗口可见。");
        } else if (enc_.output_window->minimized()) {
            ImGui::TextColored(kYellow, "输出窗口已最小化，恢复后才会刷新。");
        }
    }
    if (enc_.out_spout && enc_.spout) {
        ImGui::SameLine();
        ImGui::TextColored(enc_.spout->initialized() ? kGreen : kYellow, "Spout %s",
                           enc_.spout->initialized() ? "已连接" : "等待");
    }
    if (enc_.out_vcam && enc_.vcam) {
        ImGui::SameLine();
        ImGui::TextColored(enc_.vcam->ready() ? kGreen : kYellow, "Vcam %s",
                           enc_.vcam->ready() ? "已连接" : "等待打开");
    }
    if (!enc_.error.empty()) ImGui::TextColored(kRed, "%s", enc_.error.c_str());
}

void GuiApp::Impl::draw_encode_actions() {
    ImGui::BeginChild("##enc_actions", ImVec2(0, 56), ImGuiChildFlags_None);
    if (enc_.running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.16f, 1.0f));
        if (ImGui::Button("停止", ImVec2(150, 34))) stop_encode();
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
        if (ImGui::Button("开始混淆", ImVec2(150, 34))) start_encode();
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", enc_.running ? "编码中" : "已停止");
    ImGui::SameLine();
    ImGui::TextDisabled("渲染 %.1f fps", enc_.fps);
    if (enc_.output_window) {
        ImGui::SameLine();
        if (ImGui::Button("显示输出窗口")) enc_.output_window->show();
    }
    if (!enc_.error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kRed, "%s", enc_.error.c_str());
    }
    ImGui::EndChild();
}

void GuiApp::Impl::draw_decode() {
    const float content_h = std::max(240.0f, ImGui::GetContentRegionAvail().y - 62.0f);
    ImGui::BeginChild("##dec_left", ImVec2(370, content_h), ImGuiChildFlags_Borders);
    draw_decode_source();
    ImGui::Separator();
    draw_presets();
    ImGui::Separator();
    draw_key();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##dec_preview", ImVec2(0, content_h), ImGuiChildFlags_Borders);
    draw_decode_preview();
    ImGui::EndChild();

    draw_decode_actions();
}

void GuiApp::Impl::draw_decode_source() {
    ImGui::TextDisabled("输入源");
    ImGui::BeginDisabled(dec_.running);
    int source = static_cast<int>(dec_.source);
    ImGui::RadioButton("文件", &source, 0);
    ImGui::SameLine();
    ImGui::RadioButton("窗口", &source, 1);
    ImGui::SameLine();
    ImGui::RadioButton("显示器", &source, 2);
    dec_.source = static_cast<DecodeState::Source>(source);

    if (dec_.source == DecodeState::Source::File) {
        if (ImGui::Button("打开混淆视频...", ImVec2(-1, 0))) {
            const auto path = open_file_dialog(L"选择混淆视频", kVideoFilter);
            if (!path.empty()) dec_.file_path = path;
        }
        if (!dec_.file_path.empty()) {
            ImGui::TextWrapped("%s", wide_to_utf8(dec_.file_path).c_str());
        } else {
            ImGui::TextDisabled("未选择文件");
        }
        ImGui::Checkbox("播放器录屏（启用定位）", &dec_.detect_file);
        ImGui::TextDisabled("精确传输帧不勾选；播放器缩放/录屏时勾选。");
    } else if (dec_.source == DecodeState::Source::Window) {
        if (ImGui::Button("选择窗口...", ImVec2(-1, 0))) open_picker(true);
        if (dec_.hwnd && IsWindow(dec_.hwnd)) {
            ImGui::TextWrapped("%s", wide_to_utf8(dec_.hwnd_title).c_str());
        } else if (dec_.hwnd) {
            ImGui::TextColored(kRed, "窗口已关闭，请重新选择");
        } else {
            ImGui::TextDisabled("未选择窗口");
        }
    } else {
        std::string preview = "选择显示器";
        for (const auto& m : monitors_) {
            if (m.index == dec_.monitor_index) {
                preview = "显示器 " + std::to_string(m.index + 1) + " (" +
                          std::to_string(m.width) + "x" + std::to_string(m.height) + ")";
                break;
            }
        }
        if (ImGui::BeginCombo("##dec_monitor", preview.c_str())) {
            for (const auto& m : monitors_) {
                const std::string label =
                    "显示器 " + std::to_string(m.index + 1) + " (" +
                    std::to_string(m.width) + "x" + std::to_string(m.height) + ")";
                if (ImGui::Selectable(label.c_str(), dec_.monitor_index == m.index)) {
                    dec_.monitor_index = m.index;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Checkbox("独立预览窗口", &dec_.fullscreen);
    ImGui::TextDisabled("文件模式默认按精确传输帧解码；窗口/显示器会自动定位。");
    ImGui::EndDisabled();
}

void GuiApp::Impl::draw_decode_preview() {
    ImGui::Checkbox("显示捕获画面", &dec_.show_capture);
    ImGui::SameLine();
    ImGui::TextDisabled("默认显示还原画面");
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0;
    int height = 0;
    std::string label;
    if (dec_.show_capture && dec_.source_srv) {
        srv = dec_.source_srv.Get();
        width = dec_.source_w;
        height = dec_.source_h;
        label = "捕获画面";
    } else if (dec_.decoder) {
        srv = dec_.decoder->restored_srv();
        width = dec_.decoder->layout().ref_w();
        height = dec_.decoder->layout().ref_h();
        label = "还原画面 1400x784";
    } else {
        label = "等待输入源";
    }
    draw_image_fit(srv, width, height, label.c_str(), 78.0f);

    if (dec_.locked) {
        ImGui::TextColored(kGreen, "● 已锁定");
    } else if (dec_.running) {
        ImGui::TextColored(kYellow, "● 搜索定位点...");
    } else {
        ImGui::TextDisabled("● 未运行");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("四角 %d/4", dec_.markers);
    ImGui::SameLine();
    ImGui::TextColored(dec_.key_ok ? kGreen : kRed, "key %s", dec_.key_ok ? "✓" : "✗");
    ImGui::SameLine();
    ImGui::TextDisabled("margin %d · 检测 %.0f ms", dec_.margin, dec_.detect_ms);
    if (dec_.source_w > 0) {
        ImGui::TextDisabled("捕获 %dx%d · 解码 %.1f fps", dec_.source_w, dec_.source_h,
                            dec_.fps);
        if (dec_.source == DecodeState::Source::File && dec_.running) {
            ImGui::SameLine();
            ImGui::TextDisabled("源 %.1f fps · 丢帧 %llu", dec_.file.fps(),
                                static_cast<unsigned long long>(dec_.file.dropped()));
        }
    }
    if (!dec_.error.empty()) ImGui::TextColored(kRed, "%s", dec_.error.c_str());
}

void GuiApp::Impl::draw_decode_actions() {
    ImGui::BeginChild("##dec_actions", ImVec2(0, 56), ImGuiChildFlags_None);
    if (dec_.running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.16f, 1.0f));
        if (ImGui::Button("停止", ImVec2(150, 34))) stop_decode();
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
        if (ImGui::Button("开始解码", ImVec2(150, 34))) start_decode();
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", dec_.running ? "解码中" : "已停止");
    ImGui::SameLine();
    ImGui::TextDisabled("解码 %.1f fps", dec_.fps);
    if (dec_.fullscreen_window) {
        ImGui::SameLine();
        if (ImGui::Button("显示预览窗口")) dec_.fullscreen_window->show();
    }
    if (!dec_.error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kRed, "%s", dec_.error.c_str());
    }
    ImGui::EndChild();
}

void GuiApp::Impl::draw_status_bar() {
    ImGui::Separator();
    ImGui::TextDisabled("OBScure v0.2 · 本阶段：GUI/窗口捕获/输出通路 · 画质与码率后续");
    ImGui::SameLine(ImGui::GetWindowWidth() - 290);
    ImGui::TextDisabled("编码 %.1f fps · 解码 %.1f fps",
                        enc_.running ? enc_.fps : 0.0,
                        dec_.running ? dec_.fps : 0.0);
}

void GuiApp::Impl::open_picker(bool for_decode) {
    if (enc_.running || dec_.running) {
        set_error("请先停止当前运行，再切换捕获源。");
        return;
    }
    picker_for_decode_ = for_decode;
    picker_open_ = true;
    picker_search_[0] = '\0';
    picker_windows_ = enumerate_capture_windows();
    picker_selected_ = picker_windows_.empty() ? -1 : 0;
    stop_picker_preview();
    if (picker_selected_ >= 0) start_picker_preview(picker_selected_);
}

void GuiApp::Impl::start_picker_preview(int index) {
    stop_picker_preview();
    if (index < 0 || index >= static_cast<int>(picker_windows_.size())) return;
    const auto& info = picker_windows_[static_cast<size_t>(index)];
    if (info.minimized) {
        picker_error_ = "窗口已最小化，恢复后才能预览。";
        return;
    }
    picker_error_.clear();
    picker_preview_hwnd_ = info.hwnd;
    auto capture = std::make_unique<WgcCapture>();
    std::string error;
    if (!capture->start(device_.Get(), context_.Get(), info.hwnd, nullptr, &error)) {
        picker_error_ = "窗口预览失败: " + error;
        return;
    }
    picker_capture_ = std::move(capture);
}

void GuiApp::Impl::stop_picker_preview() {
    if (picker_capture_) {
        picker_capture_->stop();
        picker_capture_.reset();
    }
    picker_preview_hwnd_ = nullptr;
    picker_tex_.Reset();
    picker_srv_.Reset();
    picker_tex_w_ = 0;
    picker_tex_h_ = 0;
    picker_tex_format_ = DXGI_FORMAT_UNKNOWN;
}

void GuiApp::Impl::update_picker_preview() {
    if (!picker_capture_) return;
    if (picker_preview_hwnd_ && !IsWindow(picker_preview_hwnd_)) {
        picker_error_ = "窗口已关闭。";
        picker_capture_->stop();
        picker_capture_.reset();
        return;
    }
    if (!picker_capture_->has_frame()) return;
    std::lock_guard lock(picker_capture_->mutex());
    const int w = picker_capture_->width();
    const int h = picker_capture_->height();
    const DXGI_FORMAT format = picker_capture_->format();
    if (w <= 0 || h <= 0) return;
    if (!picker_tex_ || picker_tex_w_ != w || picker_tex_h_ != h ||
        picker_tex_format_ != format) {
        picker_tex_ = create_texture(device_.Get(), w, h, format,
                                     D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
        picker_srv_ = picker_tex_ ? create_srv(device_.Get(), picker_tex_.Get()) : nullptr;
        picker_tex_w_ = w;
        picker_tex_h_ = h;
        picker_tex_format_ = format;
    }
    if (picker_tex_) picker_capture_->copy_latest(picker_tex_.Get());
}

void GuiApp::Impl::draw_picker() {
    bool open = picker_open_;
    ImGui::SetNextWindowSize(ImVec2(1040, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin("选择要捕获的窗口", &open, ImGuiWindowFlags_NoCollapse);
    if (!open) {
        picker_open_ = false;
        stop_picker_preview();
        ImGui::End();
        return;
    }

    if (ImGui::Button("刷新")) {
        picker_windows_ = enumerate_capture_windows();
        picker_selected_ = picker_windows_.empty() ? -1 : 0;
        start_picker_preview(picker_selected_);
    }
    ImGui::SameLine();
    ImGui::InputTextWithHint("##search", "搜索窗口标题或进程...", picker_search_,
                             sizeof(picker_search_));
    ImGui::SameLine();
    ImGui::TextDisabled("%d 个窗口", static_cast<int>(picker_windows_.size()));

    const float list_width = 520.0f;
    const float child_height = std::max(200.0f, ImGui::GetContentRegionAvail().y - 46.0f);
    ImGui::BeginChild("##win_list", ImVec2(list_width, child_height),
                      ImGuiChildFlags_Borders);
    const std::string search = lower_ascii(picker_search_);
    for (int i = 0; i < static_cast<int>(picker_windows_.size()); ++i) {
        const auto& info = picker_windows_[static_cast<size_t>(i)];
        const std::string title = wide_to_utf8(info.title);
        const std::string process = wide_to_utf8(info.process);
        if (!search.empty()) {
            const std::string hay = lower_ascii(title + " " + process);
            if (hay.find(search) == std::string::npos) continue;
        }
        const std::string label = window_label(info);
        ImGui::PushID(static_cast<int>(reinterpret_cast<intptr_t>(info.hwnd)));
        if (ImGui::Selectable(label.c_str(), picker_selected_ == i)) {
            if (picker_selected_ != i) {
                picker_selected_ = i;
                start_picker_preview(i);
            }
        }
        ImGui::PopID();
    }
    if (picker_windows_.empty()) {
        ImGui::TextDisabled("没有找到可捕获的窗口。");
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##win_preview", ImVec2(0, child_height), ImGuiChildFlags_Borders);
    if (!picker_error_.empty()) {
        ImGui::TextColored(kRed, "%s", picker_error_.c_str());
    }
    if (picker_srv_) {
        draw_image_fit(picker_srv_.Get(), picker_tex_w_, picker_tex_h_, "窗口实时预览");
    } else {
        ImGui::TextDisabled("正在连接窗口预览...");
    }
    ImGui::EndChild();

    if (ImGui::Button("取消")) {
        picker_open_ = false;
        stop_picker_preview();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(picker_selected_ < 0 ||
                         picker_selected_ >= static_cast<int>(picker_windows_.size()));
    if (ImGui::Button("使用此窗口")) {
        const auto& info = picker_windows_[static_cast<size_t>(picker_selected_)];
        if (picker_for_decode_) {
            dec_.source = DecodeState::Source::Window;
            dec_.hwnd = info.hwnd;
            dec_.hwnd_title = info.title;
            dec_.source_w = info.width;
            dec_.source_h = info.height;
        } else {
            enc_.source = EncodeState::Source::Window;
            enc_.hwnd = info.hwnd;
            enc_.hwnd_title = info.title;
            enc_.source_w = info.width;
            enc_.source_h = info.height;
        }
        picker_open_ = false;
        stop_picker_preview();
    }
    ImGui::EndDisabled();

    ImGui::End();
}

void GuiApp::Impl::upload_bgr(const cv::Mat& bgr, ComPtr<ID3D11Texture2D>& texture,
                              ComPtr<ID3D11ShaderResourceView>& srv, int& width,
                              int& height) {
    if (bgr.empty()) return;
    cv::Mat bgra;
    cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
    if (!texture || width != bgra.cols || height != bgra.rows) {
        texture = create_texture(device_.Get(), bgra.cols, bgra.rows,
                                 DXGI_FORMAT_B8G8R8A8_UNORM,
                                 D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
        srv = texture ? create_srv(device_.Get(), texture.Get()) : nullptr;
        width = bgra.cols;
        height = bgra.rows;
    }
    if (texture && bgra.isContinuous()) {
        context_->UpdateSubresource(texture.Get(), 0, nullptr, bgra.data,
                                    static_cast<UINT>(bgra.cols * 4), 0);
    }
}

std::wstring GuiApp::Impl::make_record_path() {
    if (enc_.source == EncodeState::Source::File && !enc_.file_path.empty()) {
        const fs::path source(enc_.file_path);
        return (source.parent_path() / (source.stem().wstring() + L"_obscured.mp4"))
            .wstring();
    }
    const fs::path dir = fs::path(app_data_dir()) / "recordings";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    wchar_t name[64]{};
    wcsftime(name, sizeof(name) / sizeof(name[0]), L"transport_%Y%m%d_%H%M%S.mp4", &tm);
    return (dir / name).wstring();
}

bool GuiApp::Impl::start_encode() {
    if (enc_.running) return true;
    last_error_.clear();
    enc_.error.clear();
    try {
        if (key_buf_[0] == '\0') {
            set_error("密钥不能为空。");
            return false;
        }
        const Preset& preset = kPresets[preset_index_];
        Layout layout;
        layout.cell = preset.cell;
        layout.guard = preset.guard;
        layout.key = key_buf_;
        layout.validate();
        const KeyMaterial key = derive_key(layout);

        auto encoder = std::make_unique<EncodePipeline>();
        std::string error;
        if (!encoder->init(device_.Get(), context_.Get(), layout, key, preset.scale,
                           resolve_shader_path(L"transport.hlsl"), &error)) {
            set_error("编码器初始化失败: " + error);
            return false;
        }

        auto fail = [this](const std::string& message) {
            set_error(message);
            stop_encode();
            return false;
        };

        if (enc_.source == EncodeState::Source::File) {
            if (enc_.file_path.empty()) return fail("请先选择视频文件。");
            if (!enc_.file.open(enc_.file_path, &error)) {
                return fail("打开视频失败: " + error);
            }
            cv::Mat first;
            if (!enc_.file.tick(first, true)) return fail("读取视频帧失败。");
            enc_.last_file_frame = first;
            upload_bgr(first, enc_.source_tex, enc_.source_srv, enc_.tex_w, enc_.tex_h);
            enc_.tex_format = DXGI_FORMAT_B8G8R8A8_UNORM;
            enc_.source_w = first.cols;
            enc_.source_h = first.rows;
        } else {
            auto capture = std::make_unique<WgcCapture>();
            if (enc_.source == EncodeState::Source::Window) {
                if (!enc_.hwnd || !IsWindow(enc_.hwnd)) return fail("请选择有效的窗口。");
                if (!capture->start(device_.Get(), context_.Get(), enc_.hwnd, nullptr,
                                    &error)) {
                    return fail("窗口捕获失败: " + error);
                }
            } else {
                HMONITOR monitor = nullptr;
                std::wstring name;
                if (!monitor_by_index(enc_.monitor_index, &monitor, &name)) {
                    return fail("显示器不存在。");
                }
                if (!capture->start(device_.Get(), context_.Get(), nullptr, monitor,
                                    &error)) {
                    return fail("显示器捕获失败: " + error);
                }
            }
            for (int i = 0; i < 500 && !capture->has_frame(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!capture->has_frame()) return fail("没有收到捕获画面（窗口可能最小化）。");
            int cw = 0;
            int ch = 0;
            DXGI_FORMAT cf = DXGI_FORMAT_UNKNOWN;
            {
                std::lock_guard lock(capture->mutex());
                cw = capture->width();
                ch = capture->height();
                cf = capture->format();
                if (cw <= 0 || ch <= 0) return fail("捕获分辨率无效。");
                enc_.source_tex = create_texture(device_.Get(), cw, ch, cf,
                                                  D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
                enc_.source_srv =
                    enc_.source_tex ? create_srv(device_.Get(), enc_.source_tex.Get())
                                    : nullptr;
                if (!enc_.source_tex || !enc_.source_srv) {
                    return fail("创建源纹理失败。");
                }
                enc_.tex_w = cw;
                enc_.tex_h = ch;
                enc_.tex_format = cf;
                enc_.source_w = cw;
                enc_.source_h = ch;
                capture->copy_latest(enc_.source_tex.Get());
            }
            enc_.capture = std::move(capture);
        }

        enc_.encoder = std::move(encoder);
        enc_.layout = layout;
        enc_.key = key;

        if (enc_.out_window) {
            auto window = std::make_unique<OutputWindow>();
            if (!window->init(device_.Get(), context_.Get(), 1920, 1080,
                              L"OBScure Output 1920x1080",
                              resolve_shader_path(L"blit.hlsl"), &error)) {
                return fail("输出窗口创建失败: " + error);
            }
            window->show();
            window->set_bottom();
            enc_.output_window = std::move(window);
        }
        if (enc_.out_spout) {
            auto spout = std::make_unique<SpoutSink>();
            if (!spout->start(device_.Get(), enc_.spout_name, &error)) {
                return fail("Spout 启动失败: " + error);
            }
            enc_.spout = std::move(spout);
        }
        if (enc_.out_vcam) {
            std::string install_error;
            // Always rewrite the per-user registration so the GUI uses the
            // filter DLL shipped next to this executable.
            if (!vcam_install(&install_error)) {
                return fail("虚拟摄像头安装失败: " + install_error);
            }
            auto vcam = std::make_unique<VcamSink>();
            if (!vcam->start(device_.Get(), context_.Get(), 1920, 1080, &error)) {
                return fail("虚拟摄像头启动失败: " + error);
            }
            enc_.vcam = std::move(vcam);
        }
        if (enc_.out_record) {
            if (enc_.record_path.empty()) enc_.record_path = make_record_path();
            auto recorder = std::make_unique<FfmpegSink>();
            if (!recorder->start(device_.Get(), context_.Get(), 1920, 1080, 60.0,
                                 record_bitrate_, enc_.record_path, &error)) {
                return fail("录制启动失败: " + error);
            }
            enc_.recorder = std::move(recorder);
        }

        enc_.stat_frames = 0;
        enc_.fps = 0.0;
        enc_.stat_start = Clock::now();
        enc_.running = true;
        log_line("encode started: " + std::string(preset.id));
        return true;
    } catch (const std::exception& e) {
        set_error(std::string("启动编码失败: ") + e.what());
        stop_encode();
        return false;
    }
}

void GuiApp::Impl::stop_encode() {
    enc_.running = false;
    if (enc_.capture) {
        enc_.capture->stop();
        enc_.capture.reset();
    }
    if (enc_.output_window) {
        enc_.output_window->shutdown();
        enc_.output_window.reset();
    }
    if (enc_.spout) {
        enc_.spout->stop();
        enc_.spout.reset();
    }
    if (enc_.vcam) {
        enc_.vcam->stop();
        enc_.vcam.reset();
    }
    if (enc_.recorder) {
        enc_.recorder->stop();
        if (!enc_.recorder->error().empty()) enc_.error = enc_.recorder->error();
        enc_.recorder.reset();
    }
    enc_.file.close();
    enc_.encoder.reset();
    enc_.fps = 0.0;
    enc_.stat_frames = 0;
}

void GuiApp::Impl::update_encode() {
    if (!enc_.encoder) return;
    if (enc_.source == EncodeState::Source::File) {
        cv::Mat frame;
        if (enc_.file.tick(frame)) {
            enc_.last_file_frame = frame;
            upload_bgr(frame, enc_.source_tex, enc_.source_srv, enc_.tex_w, enc_.tex_h);
            enc_.tex_format = DXGI_FORMAT_B8G8R8A8_UNORM;
            enc_.source_w = frame.cols;
            enc_.source_h = frame.rows;
        }
    } else if (enc_.capture) {
        if (enc_.source == EncodeState::Source::Window &&
            (!enc_.hwnd || !IsWindow(enc_.hwnd))) {
            set_error("源窗口已关闭。");
            stop_encode();
            return;
        }
        std::lock_guard lock(enc_.capture->mutex());
        const int cw = enc_.capture->width();
        const int ch = enc_.capture->height();
        const DXGI_FORMAT cf = enc_.capture->format();
        if (cw <= 0 || ch <= 0) return;
        if (!enc_.source_tex || enc_.tex_w != cw || enc_.tex_h != ch ||
            enc_.tex_format != cf) {
            enc_.source_tex = create_texture(device_.Get(), cw, ch, cf,
                                             D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
            enc_.source_srv =
                enc_.source_tex ? create_srv(device_.Get(), enc_.source_tex.Get())
                                : nullptr;
            enc_.tex_w = cw;
            enc_.tex_h = ch;
            enc_.tex_format = cf;
            enc_.source_w = cw;
            enc_.source_h = ch;
        }
        if (enc_.source_tex) enc_.capture->copy_latest(enc_.source_tex.Get());
    }

    if (!enc_.source_srv) return;
    enc_.encoder->render(enc_.source_srv.Get());
    if (enc_.output_window) enc_.output_window->present(enc_.encoder->output_srv());
    if (enc_.spout) enc_.spout->send(enc_.encoder->output_texture());
    if (enc_.vcam) enc_.vcam->send(enc_.encoder->output_texture());
    if (enc_.recorder) enc_.recorder->write(enc_.encoder->output_texture());

    ++enc_.stat_frames;
    const auto now = Clock::now();
    if (enc_.source == EncodeState::Source::File && enc_.file.is_open()) {
        if (enc_.last_file_log.time_since_epoch().count() == 0 ||
            now - enc_.last_file_log >= std::chrono::seconds(10)) {
            enc_.last_file_log = now;
            log_line("file encode: pos=" +
                     std::to_string(static_cast<long long>(enc_.file.position_ms())) +
                     "ms fps=" + std::to_string(static_cast<int>(enc_.fps)) +
                     " dropped=" + std::to_string(enc_.file.dropped()));
        }
    }
    const double elapsed = std::chrono::duration<double>(now - enc_.stat_start).count();
    if (elapsed >= 1.0) {
        enc_.fps = static_cast<double>(enc_.stat_frames) / elapsed;
        enc_.stat_frames = 0;
        enc_.stat_start = now;
    }
}

bool GuiApp::Impl::start_decode() {
    if (dec_.running) return true;
    last_error_.clear();
    dec_.error.clear();
    try {
        if (key_buf_[0] == '\0') {
            set_error("密钥不能为空。");
            return false;
        }
        const Preset& preset = kPresets[preset_index_];
        Layout layout;
        layout.cell = preset.cell;
        layout.guard = preset.guard;
        layout.key = key_buf_;
        layout.validate();
        const KeyMaterial key = derive_key(layout);

        auto decoder = std::make_unique<DecodePipeline>();
        std::string error;
        if (!decoder->init(device_.Get(), context_.Get(), layout, key,
                           resolve_shader_path(L"decode.hlsl"), &error)) {
            set_error("解码器初始化失败: " + error);
            return false;
        }

        auto fail = [this](const std::string& message) {
            set_error(message);
            stop_decode();
            return false;
        };

        dec_.detect_tick = 0;
        dec_.detector.reset();
        if (dec_.source == DecodeState::Source::File) {
            if (dec_.file_path.empty()) return fail("请先选择混淆视频。");
            if (!dec_.file.open(dec_.file_path, &error)) {
                return fail("打开混淆视频失败: " + error);
            }
            cv::Mat first;
            if (!dec_.file.tick(first, true)) return fail("读取混淆视频帧失败。");
            dec_.last_file_frame = first;
            upload_bgr(first, dec_.source_tex, dec_.source_srv, dec_.tex_w, dec_.tex_h);
            dec_.tex_format = DXGI_FORMAT_B8G8R8A8_UNORM;
            dec_.source_w = first.cols;
            dec_.source_h = first.rows;
            dec_.H_initial = identity_scaled(first.cols, first.rows);
            dec_.H = dec_.H_initial.clone();
            dec_.margin = adaptive_margin(dec_.H);
            if (dec_.detect_file) {
                dec_.detector = std::make_unique<DetectionWorker>(layout, key);
                dec_.detector->start();
                dec_.detector->submit(first);
                dec_.locked = false;
            } else {
                dec_.locked = true;
            }
        } else {
            auto capture = std::make_unique<WgcCapture>();
            if (dec_.source == DecodeState::Source::Window) {
                if (!dec_.hwnd || !IsWindow(dec_.hwnd)) return fail("请选择有效的窗口。");
                if (!capture->start(device_.Get(), context_.Get(), dec_.hwnd, nullptr,
                                    &error)) {
                    return fail("窗口捕获失败: " + error);
                }
            } else {
                HMONITOR monitor = nullptr;
                std::wstring name;
                if (!monitor_by_index(dec_.monitor_index, &monitor, &name)) {
                    return fail("显示器不存在。");
                }
                if (!capture->start(device_.Get(), context_.Get(), nullptr, monitor,
                                    &error)) {
                    return fail("显示器捕获失败: " + error);
                }
            }
            for (int i = 0; i < 500 && !capture->has_frame(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!capture->has_frame()) return fail("没有收到捕获画面（窗口可能最小化）。");
            int cw = 0;
            int ch = 0;
            DXGI_FORMAT cf = DXGI_FORMAT_UNKNOWN;
            {
                std::lock_guard lock(capture->mutex());
                cw = capture->width();
                ch = capture->height();
                cf = capture->format();
                if (cw <= 0 || ch <= 0) return fail("捕获分辨率无效。");
                dec_.source_tex = create_texture(device_.Get(), cw, ch, cf,
                                                  D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
                dec_.source_srv =
                    dec_.source_tex ? create_srv(device_.Get(), dec_.source_tex.Get())
                                    : nullptr;
                dec_.staging = create_staging_texture(device_.Get(), cw, ch, cf);
                if (!dec_.source_tex || !dec_.source_srv || !dec_.staging) {
                    return fail("创建捕获纹理失败。");
                }
                dec_.tex_w = cw;
                dec_.tex_h = ch;
                dec_.tex_format = cf;
                dec_.source_w = cw;
                dec_.source_h = ch;
                capture->copy_latest(dec_.source_tex.Get());
            }
            dec_.capture = std::move(capture);
            dec_.H_initial = identity_scaled(dec_.source_w, dec_.source_h);
            dec_.H = dec_.H_initial.clone();
            dec_.margin = adaptive_margin(dec_.H);
            dec_.detector = std::make_unique<DetectionWorker>(layout, key);
            dec_.detector->start();
            dec_.locked = false;
        }

        dec_.decoder = std::move(decoder);
        dec_.layout = layout;
        dec_.key = key;

        if (dec_.fullscreen) {
            auto window = std::make_unique<OutputWindow>();
            if (!window->init(device_.Get(), context_.Get(), layout.ref_w(),
                              layout.ref_h(), L"OBScure Restored",
                              resolve_shader_path(L"blit.hlsl"), &error)) {
                return fail("独立预览窗口创建失败: " + error);
            }
            window->show();
            window->set_bottom();
            dec_.fullscreen_window = std::move(window);
        }

        dec_.stat_frames = 0;
        dec_.fps = 0.0;
        dec_.stat_start = Clock::now();
        dec_.running = true;
        log_line("decode started");
        return true;
    } catch (const std::exception& e) {
        set_error(std::string("启动解码失败: ") + e.what());
        stop_decode();
        return false;
    }
}

void GuiApp::Impl::stop_decode() {
    dec_.running = false;
    if (dec_.detector) {
        dec_.detector->stop();
        dec_.detector.reset();
    }
    if (dec_.capture) {
        dec_.capture->stop();
        dec_.capture.reset();
    }
    if (dec_.fullscreen_window) {
        dec_.fullscreen_window->shutdown();
        dec_.fullscreen_window.reset();
    }
    dec_.file.close();
    dec_.decoder.reset();
    dec_.fps = 0.0;
    dec_.stat_frames = 0;
}

bool GuiApp::Impl::read_source_bgr(cv::Mat& bgr) {
    if (dec_.source == DecodeState::Source::File) {
        bgr = dec_.last_file_frame;
        return !bgr.empty();
    }
    if (!dec_.staging || !dec_.source_tex) return false;
    context_->CopyResource(dec_.staging.Get(), dec_.source_tex.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(dec_.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    cv::Mat bgra(dec_.tex_h, dec_.tex_w, CV_8UC4, mapped.pData, mapped.RowPitch);
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    context_->Unmap(dec_.staging.Get(), 0);
    return true;
}

void GuiApp::Impl::apply_detection(const DetectionOutcome& outcome) {
    const auto now = Clock::now();
    if (dec_.last_detect_log.time_since_epoch().count() == 0 ||
        now - dec_.last_detect_log >= std::chrono::seconds(5)) {
        dec_.last_detect_log = now;
        log_line("decode detect: capture=" + std::to_string(dec_.source_w) + "x" +
                 std::to_string(dec_.source_h) +
                 " markers=" + std::to_string(outcome.markers) +
                 " ok=" + (outcome.ok ? "1" : "0") +
                 " ms=" + std::to_string(static_cast<int>(outcome.ms)));
    }
    if (!outcome.ok) {
        dec_.locked = false;
        dec_.markers = outcome.markers;
        dec_.detect_ms = outcome.ms;
        return;
    }
    const bool snap = content_delta(dec_.H_initial, outcome.H, dec_.layout) < 2.0;
    dec_.H = snap ? dec_.H_initial.clone() : outcome.H;
    dec_.margin = snap ? adaptive_margin(dec_.H) : outcome.margin;
    dec_.key_ok = outcome.key_ok;
    dec_.markers = outcome.markers;
    dec_.detect_ms = outcome.ms;
    dec_.locked = true;
}

void GuiApp::Impl::update_decode() {
    if (!dec_.decoder) return;
    if (dec_.source == DecodeState::Source::File) {
        cv::Mat frame;
        if (dec_.file.tick(frame)) {
            dec_.last_file_frame = frame;
            upload_bgr(frame, dec_.source_tex, dec_.source_srv, dec_.tex_w, dec_.tex_h);
            dec_.tex_format = DXGI_FORMAT_B8G8R8A8_UNORM;
            dec_.source_w = frame.cols;
            dec_.source_h = frame.rows;
        }
    } else if (dec_.capture) {
        if (dec_.source == DecodeState::Source::Window &&
            (!dec_.hwnd || !IsWindow(dec_.hwnd))) {
            set_error("源窗口已关闭。");
            stop_decode();
            return;
        }
        std::lock_guard lock(dec_.capture->mutex());
        const int cw = dec_.capture->width();
        const int ch = dec_.capture->height();
        const DXGI_FORMAT cf = dec_.capture->format();
        if (cw <= 0 || ch <= 0) return;
        if (!dec_.source_tex || dec_.tex_w != cw || dec_.tex_h != ch ||
            dec_.tex_format != cf) {
            dec_.source_tex = create_texture(device_.Get(), cw, ch, cf,
                                             D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
            dec_.source_srv =
                dec_.source_tex ? create_srv(device_.Get(), dec_.source_tex.Get())
                                : nullptr;
            dec_.staging = create_staging_texture(device_.Get(), cw, ch, cf);
            dec_.tex_w = cw;
            dec_.tex_h = ch;
            dec_.tex_format = cf;
            dec_.source_w = cw;
            dec_.source_h = ch;
        }
        if (dec_.source_tex) dec_.capture->copy_latest(dec_.source_tex.Get());
    }

    if (dec_.detector) {
        bool submit = false;
        cv::Mat bgr;
        if (dec_.source == DecodeState::Source::File) {
            if (++dec_.detect_tick >= 30 && !dec_.last_file_frame.empty()) {
                bgr = dec_.last_file_frame;
                submit = true;
                dec_.detect_tick = 0;
            }
        } else {
            if (++dec_.detect_tick >= 60) {
                if (read_source_bgr(bgr)) submit = true;
                dec_.detect_tick = 0;
            }
        }
        if (submit) {
            dec_.detector->submit(bgr);
            dec_.last_detect_frame = bgr.clone();
        }
        DetectionOutcome outcome;
        while (dec_.detector->take(&outcome)) {
            apply_detection(outcome);
            if (!outcome.ok && !dec_.last_detect_frame.empty()) {
                const auto now = Clock::now();
                if (dec_.last_fail_dump.time_since_epoch().count() == 0 ||
                    now - dec_.last_fail_dump > std::chrono::seconds(5)) {
                    dec_.last_fail_dump = now;
                    std::error_code ec;
                    std::filesystem::create_directories("out", ec);
                    char name[128]{};
                    sprintf_s(name, "out\\decode_fail_%dx%d.png",
                              dec_.last_detect_frame.cols, dec_.last_detect_frame.rows);
                    cv::imwrite(name, dec_.last_detect_frame);
                    log_line("decode fail dump: " + std::string(name) +
                             " markers=" + std::to_string(outcome.markers));
                }
            }
        }
    }

    if (!dec_.source_srv || dec_.H.empty()) return;
    dec_.decoder->set_margin(dec_.margin);
    dec_.decoder->render(dec_.source_srv.Get(), dec_.source_w, dec_.source_h, dec_.H);
    if (dec_.fullscreen_window) {
        dec_.fullscreen_window->present(dec_.decoder->restored_srv());
    }

    ++dec_.stat_frames;
    const auto now = Clock::now();
    if (dec_.source == DecodeState::Source::File && dec_.file.is_open()) {
        if (dec_.last_file_log.time_since_epoch().count() == 0 ||
            now - dec_.last_file_log >= std::chrono::seconds(10)) {
            dec_.last_file_log = now;
            log_line("file decode: pos=" +
                     std::to_string(static_cast<long long>(dec_.file.position_ms())) +
                     "ms fps=" + std::to_string(static_cast<int>(dec_.fps)) +
                     " dropped=" + std::to_string(dec_.file.dropped()));
        }
    }
    const double elapsed = std::chrono::duration<double>(now - dec_.stat_start).count();
    if (elapsed >= 1.0) {
        dec_.fps = static_cast<double>(dec_.stat_frames) / elapsed;
        dec_.stat_frames = 0;
        dec_.stat_start = now;
    }
}

GuiApp::GuiApp() : impl_(std::make_unique<Impl>()) {}
GuiApp::~GuiApp() = default;

int GuiApp::run() {
    return impl_->run();
}

}  // namespace obs
