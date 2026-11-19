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
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "core/detect.h"
#include "core/key.h"
#include "core/layout.h"
#include "core/metadata.h"
#include "core/presets.h"
#include "gui/gui_settings.h"
#include "io/ffmpeg_sink.h"
#include "live/d3d_helpers.h"
#include "live/decode_pipeline.h"
#include "live/detection_worker.h"
#include "live/encode_pipeline.h"
#include "live/file_source.h"
#include "live/output_window.h"
#include "live/preview_window.h"
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

// WM_GETMINMAXINFO can arrive before WM_NCCREATE, when the per-window user
// data is not set yet. This flag lets the console strip start narrow.
bool g_console_min_size = false;

const wchar_t kVideoFilter[] =
    L"视频文件\0*.mp4;*.mkv;*.mov;*.avi;*.ts\0所有文件\0*.*\0";

struct DpiScope {
    DPI_AWARENESS_CONTEXT old = nullptr;
    DpiScope() { old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); }
    ~DpiScope() {
        if (old) SetThreadDpiAwarenessContext(old);
    }
};

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
    return key_id_from_text(key);
}

std::string window_label(const WindowInfo& info) {
    std::ostringstream os;
    os << wide_to_utf8(info.title) << "  ·  " << wide_to_utf8(info.process) << "  ·  "
       << info.width << "x" << info.height;
    if (info.minimized) os << "  [最小化]";
    return os.str();
}

RECT clamp_window_rect(RECT rect, int min_w, int min_h) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        rect = {0, 0, min_w, min_h};
    }
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return rect;
    const RECT work = info.rcWork;
    const int work_w = work.right - work.left;
    const int work_h = work.bottom - work.top;
    const int width = std::clamp(static_cast<int>(rect.right - rect.left), min_w, work_w);
    const int height = std::clamp(static_cast<int>(rect.bottom - rect.top), min_h, work_h);
    const int x = std::clamp(static_cast<int>(rect.left), static_cast<int>(work.left),
                             static_cast<int>(work.right) - width);
    const int y = std::clamp(static_cast<int>(rect.top), static_cast<int>(work.top),
                             static_cast<int>(work.bottom) - height);
    return {x, y, x + width, y + height};
}

std::string format_time_ms(double ms) {
    if (ms < 0) ms = 0;
    const long long total = static_cast<long long>(ms / 1000.0);
    const long long h = total / 3600;
    const long long m = (total % 3600) / 60;
    const long long s = total % 60;
    char buffer[32]{};
    if (h > 0) {
        sprintf_s(buffer, "%lld:%02lld:%02lld", h, m, s);
    } else {
        sprintf_s(buffer, "%02lld:%02lld", m, s);
    }
    return buffer;
}

struct ExportFormat {
    const char* label;
    const char* extension;
    const char* codec;
};

constexpr ExportFormat kExportFormats[] = {
    {"MP4 / H.264", "mp4", "h264"},
    {"MKV / H.264", "mkv", "h264"},
    {"MOV / H.264", "mov", "h264"},
    {"MP4 / H.265", "mp4", "h265"},
    {"MKV / H.265", "mkv", "h265"},
    {"MOV / H.265", "mov", "h265"},
    {"MP4 / AV1", "mp4", "av1"},
    {"MKV / AV1", "mkv", "av1"},
    {"MOV / AV1", "mov", "av1"},
};
constexpr int kExportFormatCount = static_cast<int>(std::size(kExportFormats));

std::wstring replace_extension(std::wstring path, const std::string& extension) {
    std::filesystem::path p(path);
    p.replace_extension(L"." + utf8_to_wide(extension));
    return p.wstring();
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
        bool player_dragging = false;
        float player_pos = 0.0f;
        Clock::time_point player_last_seek{};
        double probe_duration_ms = 0.0;
        double probe_fps = 60.0;
        int probe_frames = 0;
        bool probe_valid = false;
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
        bool use_color = true;
        bool use_dihedral = true;
        std::optional<Metadata> video_meta;
        bool metadata_applied = false;
        bool metadata_read_pending = false;
        Clock::time_point metadata_read_deadline{};
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
        bool player_dragging = false;
        float player_pos = 0.0f;
        Clock::time_point player_last_seek{};
        double probe_duration_ms = 0.0;
        double probe_fps = 60.0;
        int probe_frames = 0;
        bool probe_valid = false;
    };

    struct ExportState {
        bool running = false;
        bool cancel_requested = false;
        bool for_decode = false;
        std::wstring source_path;
        std::wstring output_path;
        std::string format = "mp4";
        std::string codec = "h264";
        int bitrate = 8;
        int total_frames = 0;
        int done_frames = 0;
        double fps = 60.0;
        int width = 0;
        int height = 0;
        bool pending_first_frame = false;
        bool detection_mode = false;
        int detect_failures = 0;
        cv::Mat first_frame;
        cv::Mat H;
        Layout layout;
        KeyMaterial key;
        bool use_color = true;
        bool use_dihedral = true;
        TransportOptions transport;
        std::unique_ptr<FileSource> file;
        std::unique_ptr<EncodePipeline> encoder;
        std::unique_ptr<DecodePipeline> decoder;
        std::unique_ptr<FfmpegSink> sink;
        ComPtr<ID3D11Texture2D> source_tex;
        ComPtr<ID3D11ShaderResourceView> source_srv;
        int src_w = 0;
        int src_h = 0;
        std::string message;
        std::string error;
        Clock::time_point started{};
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
int margin_override_ = -1;  // -1 auto, 0/1/2 manual
    bool public_mode_ = false;
    bool enc_use_color_ = true;
    bool show_key_ = false;
    bool auto_match_settings_ = true;
    Clock::time_point key_copied_until_{};
    GuiSettings settings_;
    bool detached_preview_ = false;
    bool start_detached_ = false;
    std::unique_ptr<PreviewWindow> preview_window_;
    ExportState export_;
    std::wstring export_path_encode_;
    std::wstring export_path_decode_;
    int export_format_index_ = 0;
    int export_bitrate_ = 8;
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
    bool auto_detach_ = false;
    bool auto_no_detach_ = false;
    bool auto_export_ = false;
    std::wstring auto_file_;
    std::wstring auto_window_;
    std::wstring auto_out_;
    std::wstring auto_format_ = L"mp4";
    std::wstring auto_codec_ = L"h264";
    int auto_export_bitrate_ = 0;

    int run();
    bool init(HINSTANCE instance);
    void shutdown();
    void open_log();
    void log_line(const std::string& text);
    void set_error(const std::string& text);
    void save_settings();

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
    void draw_video_info();
    void draw_player_bar(bool decode_mode);
    void apply_player_seek(bool decode_mode, const cv::Mat& frame);
    void handle_player_hotkeys();
    void draw_preview_toolbar();
    void set_detached_preview(bool detached);
    void restore_detached_preview();
    bool ensure_preview_window();
    void store_main_rect(bool console);
    void restore_merged_window_rect();
    void apply_console_rect();
    void place_preview_to_right();
    void update_detached_preview();
    void update_preview_player();
    bool start_export(bool for_decode);
    void update_export();
    void finish_export(bool cancelled);
    void draw_export_ui(bool for_decode);
    void probe_file(bool decode_mode, const std::wstring& path);
    std::wstring default_export_path(bool for_decode, const std::wstring& source);
    double export_source_duration_ms(bool for_decode) const;
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
    void reset_decode_detection();
    void apply_detection(const DetectionOutcome& outcome);
    bool create_decode_pipeline(const Layout& layout, const KeyMaterial& key,
                                bool use_color, bool use_dihedral, std::string* error);
    void recreate_decode_detector(bool enable);
    void sync_decode_preview_window();
    bool apply_video_metadata(const Metadata& metadata, bool force);
    bool apply_manual_decode_settings();
    void request_video_settings();
    std::optional<Metadata> read_file_metadata();
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
            if (export_.running) finish_export(true);
        }
        if (picker_open_) update_picker_preview();
        if (export_.running) update_export();
        update_preview_player();
        update_detached_preview();

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
    load_gui_settings(&settings_);
    if (settings_.console_w < 380 || settings_.console_w > 700) {
        settings_.console_w = 480;
    }
    if (settings_.console_h < 620) settings_.console_h = 820;
    parse_command_line();
    start_detached_ =
        auto_no_detach_ ? false : (auto_detach_ || settings_.detached_preview);
    detached_preview_ = start_detached_;
    g_console_min_size = detached_preview_;
    export_bitrate_ = std::clamp(settings_.export_bitrate, 1, 100);
    export_path_encode_ = settings_.export_path_encode;
    export_path_decode_ = settings_.export_path_decode;
    for (int i = 0; i < kExportFormatCount; ++i) {
        if (settings_.export_format == kExportFormats[i].extension &&
            settings_.export_codec == kExportFormats[i].codec) {
            export_format_index_ = i;
            break;
        }
    }
    monitors_ = enumerate_monitors();
    ui_key_cache_ = key_buf_;
    ui_key_id_ = key_id_for_text(ui_key_cache_);
    if (!init_window(instance)) return false;
    if (!init_d3d()) return false;
    if (!init_imgui()) return false;
    if (start_detached_) restore_detached_preview();
    log_line("GUI started");
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
        } else if (arg == L"--key" && i + 1 < argc) {
            const std::string text = wide_to_utf8(argv[++i]);
            strncpy_s(key_buf_, text.c_str(), _TRUNCATE);
        } else if (arg == L"--public") {
            public_mode_ = true;
        } else if (arg == L"--no-color") {
            enc_use_color_ = false;
            dec_.use_color = false;
        } else if (arg == L"--detach") {
            auto_detach_ = true;
        } else if (arg == L"--no-detach") {
            auto_no_detach_ = true;
        } else if (arg == L"--export") {
            auto_export_ = true;
            auto_start_ = true;
        } else if (arg == L"--format" && i + 1 < argc) {
            auto_format_ = argv[++i];
        } else if (arg == L"--codec" && i + 1 < argc) {
            auto_codec_ = argv[++i];
        } else if (arg == L"--export-bitrate" && i + 1 < argc) {
            try {
                auto_export_bitrate_ = std::stoi(argv[++i]);
            } catch (...) {
                auto_export_bitrate_ = 0;
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

    if (auto_export_) {
        if (auto_file_.empty()) {
            set_error("--export 需要 --file 指定源文件。");
            return;
        }
        mode_ = auto_decode_ ? Mode::Decode : Mode::Encode;
        if (auto_decode_) {
            dec_.source = DecodeState::Source::File;
            dec_.file_path = auto_file_;
            dec_.detect_file = auto_detect_;
            probe_file(true, auto_file_);
        } else {
            enc_.source = EncodeState::Source::File;
            enc_.file_path = auto_file_;
            probe_file(false, auto_file_);
        }
        const std::string format = lower_ascii(wide_to_utf8(auto_format_));
        const std::string codec = lower_ascii(wide_to_utf8(auto_codec_));
        for (int i = 0; i < kExportFormatCount; ++i) {
            if (format == kExportFormats[i].extension &&
                codec == kExportFormats[i].codec) {
                export_format_index_ = i;
                break;
            }
        }
        if (auto_export_bitrate_ > 0) {
            export_bitrate_ = std::clamp(auto_export_bitrate_, 1, 100);
        }
        if (!auto_out_.empty()) {
            if (auto_decode_) {
                export_path_decode_ = auto_out_;
            } else {
                export_path_encode_ = auto_out_;
            }
        }
        start_export(auto_decode_);
        return;
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
            probe_file(true, auto_file_);
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
            probe_file(false, auto_file_);
        }
        if (!auto_out_.empty()) {
            enc_.record_path = auto_out_;
        } else if (auto_record_) {
            enc_.record_path = make_record_path();
        }
        start_encode();
    }
}

void GuiApp::Impl::store_main_rect(bool console) {
    if (!hwnd_) return;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd_, &placement)) return;
    const RECT& rect = placement.rcNormalPosition;
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    if (console) {
        settings_.console_x = rect.left;
        settings_.console_y = rect.top;
        settings_.console_w = rect.right - rect.left;
        settings_.console_h = rect.bottom - rect.top;
    } else {
        settings_.window_x = rect.left;
        settings_.window_y = rect.top;
        settings_.window_w = rect.right - rect.left;
        settings_.window_h = rect.bottom - rect.top;
        settings_.maximized = placement.showCmd == SW_SHOWMAXIMIZED;
    }
}

void GuiApp::Impl::save_settings() {
    store_main_rect(detached_preview_);
    if (preview_window_) {
        RECT rect{};
        preview_window_->get_window_rect(&rect);
        if (rect.right > rect.left && rect.bottom > rect.top) {
            settings_.preview_x = rect.left;
            settings_.preview_y = rect.top;
            settings_.preview_w = rect.right - rect.left;
            settings_.preview_h = rect.bottom - rect.top;
        }
        settings_.preview_topmost = preview_window_->topmost();
    }
    settings_.detached_preview = detached_preview_;
    save_gui_settings(settings_);
}

void GuiApp::Impl::shutdown() {
    finish_export(true);
    save_settings();
    stop_encode();
    stop_decode();
    stop_picker_preview();
    if (preview_window_) {
        preview_window_->shutdown();
        preview_window_.reset();
    }
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
    if (public_mode_) return public_key_id();
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
            const bool console = (self && self->detached_preview_) || g_console_min_size;
            info->ptMinTrackSize.x = console ? 380 : 1000;
            info->ptMinTrackSize.y = console ? 620 : 680;
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

    const bool console_mode = detached_preview_;
    const int min_width = console_mode ? 380 : 1000;
    const int min_height = console_mode ? 620 : 680;
    RECT window_rect{};
    bool have_saved_rect =
        console_mode
            ? (settings_.console_x != INT_MIN && settings_.console_y != INT_MIN &&
               settings_.console_w >= min_width && settings_.console_h >= min_height)
            : (settings_.window_x != INT_MIN && settings_.window_y != INT_MIN &&
               settings_.window_w >= min_width && settings_.window_h >= min_height);
    if (have_saved_rect) {
        if (console_mode) {
            window_rect = {settings_.console_x, settings_.console_y,
                           settings_.console_x + settings_.console_w,
                           settings_.console_y + settings_.console_h};
        } else {
            window_rect = {settings_.window_x, settings_.window_y,
                           settings_.window_x + settings_.window_w,
                           settings_.window_y + settings_.window_h};
        }
        window_rect = clamp_window_rect(window_rect, min_width, min_height);
    } else {
        window_rect = {0, 0, console_mode ? 480 : 1280, 820};
        RECT adjusted = window_rect;
        AdjustWindowRect(&adjusted, WS_OVERLAPPEDWINDOW, FALSE);
        window_rect = {0, 0, adjusted.right - adjusted.left,
                       adjusted.bottom - adjusted.top};
    }

    hwnd_ = CreateWindowExW(0, L"OBScureGuiWindow", L"OBScure",
                            WS_OVERLAPPEDWINDOW,
                            have_saved_rect ? window_rect.left : CW_USEDEFAULT,
                            have_saved_rect ? window_rect.top : CW_USEDEFAULT,
                            window_rect.right - window_rect.left,
                            window_rect.bottom - window_rect.top, nullptr, nullptr,
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
    RECT window_rect_log{};
    GetWindowRect(hwnd_, &window_rect_log);
    log_line("window client " + std::to_string(win_w_) + "x" + std::to_string(win_h_) +
             " rect " + std::to_string(window_rect_log.right - window_rect_log.left) + "x" +
             std::to_string(window_rect_log.bottom - window_rect_log.top) + " dpi " +
             std::to_string(GetDpiForWindow(hwnd_)) + " awareness " +
             std::to_string(static_cast<int>(awareness)) + " thread_awareness " +
             std::to_string(GetAwarenessFromDpiAwarenessContext(
                 GetThreadDpiAwarenessContext())));
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    if (!detached_preview_ && settings_.maximized) ShowWindow(hwnd_, SW_MAXIMIZE);
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

    draw_preview_toolbar();

    if (export_.running) {
        ImGui::TextColored(kYellow, "导出中：%s %d/%d 帧",
                           export_.for_decode ? "解码" : "编码",
                           export_.done_frames, export_.total_frames);
    }

    if (detached_preview_ && preview_window_ && !io.WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_F8, false)) {
        const bool topmost = !preview_window_->topmost();
        preview_window_->set_topmost(topmost);
        settings_.preview_topmost = topmost;
        save_settings();
    }
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        set_detached_preview(!detached_preview_);
    }
    if (export_.running && !io.WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        export_.cancel_requested = true;
    }

    if (mode_ == Mode::Decode && dec_.running && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
            request_video_settings();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
            apply_manual_decode_settings();
        }
    }

    handle_player_hotkeys();

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

    const ImVec4 accent = kAccent;
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::TextUnformatted("OBSCure");
    ImGui::PopStyleColor();

    const bool running = enc_.running || dec_.running;
    if (detached_preview_) {
        ImGui::SameLine(100.0f);
        tab("编码", Mode::Encode);
        ImGui::SameLine();
        tab("解码", Mode::Decode);
        ImGui::TextDisabled("keyID %08X", key_id());
        ImGui::SameLine();
        ImGui::TextColored(running ? kGreen : kYellow, running ? "● 运行中"
                                                              : "● 就绪");
        return;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("实时画面混淆");
    ImGui::SameLine(220.0f);
    tab("编码", Mode::Encode);
    ImGui::SameLine();
    tab("解码", Mode::Decode);
    ImGui::SameLine();
    ImGui::TextDisabled("keyID %08X", key_id());
    ImGui::SameLine();
    ImGui::TextColored(running ? kGreen : kYellow, running ? "● 运行中" : "● 就绪");
}

void GuiApp::Impl::draw_encode() {
    const float action_h = detached_preview_ ? 112.0f : 62.0f;
    const float content_h = std::max(240.0f, ImGui::GetContentRegionAvail().y - action_h);
    const float left_w = detached_preview_
                             ? std::min(560.0f, ImGui::GetContentRegionAvail().x)
                             : 370.0f;
    ImGui::BeginChild("##enc_left", ImVec2(left_w, content_h),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    draw_encode_source();
    ImGui::Separator();
    draw_encode_outputs();
    ImGui::Separator();
    draw_presets();
    ImGui::Separator();
    ImGui::BeginDisabled(enc_.running);
    ImGui::Checkbox("颜色混淆", &enc_use_color_);
    ImGui::EndDisabled();
    ImGui::TextWrapped("关闭后保留原色；\n位置/翻转混淆仍启用。");
    ImGui::Separator();
    draw_key();
    draw_export_ui(false);
    ImGui::EndChild();

    if (!detached_preview_) {
        ImGui::SameLine();
        ImGui::BeginChild("##enc_preview", ImVec2(0, content_h),
                          ImGuiChildFlags_Borders);
        draw_encode_preview();
        ImGui::EndChild();
    }

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
                probe_file(false, path);
            }
        }
        if (!enc_.file_path.empty()) {
            ImGui::TextWrapped("%s", wide_to_utf8(enc_.file_path).c_str());
        } else {
            ImGui::TextDisabled("未选择视频文件");
        }
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("窗口捕获需要窗口可见。\n独占全屏游戏建议用无边框模式。");
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
    ImGui::Checkbox("虚拟摄像头（OBScure）", &enc_.out_vcam);
    ImGui::TextWrapped("DirectShow 虚拟摄像头");
    ImGui::TextWrapped("优先 NV12，其次 YUY2 / RGB。");
    ImGui::TextWrapped("OBS 添加“视频采集设备”即可。");
    ImGui::Checkbox("实时录制（直播模拟）", &enc_.out_record);
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
        ImGui::TextWrapped("直播参考：1080p60 约 6–8 Mbps，720p60 约 4–6 Mbps。");
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
    ImGui::BeginDisabled(enc_.running);
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
    ImGui::TextWrapped("本阶段固定算法，画质/码率后续再调。");
}

void GuiApp::Impl::draw_key() {
    ImGui::TextDisabled("共享密钥");
    ImGui::BeginDisabled(enc_.running);
    ImGui::Checkbox("无密钥模式（公开视频）", &public_mode_);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(enc_.running || public_mode_);
    ImGui::SetNextItemWidth(168);
    ImGui::InputText("##key", key_buf_, sizeof(key_buf_),
                     show_key_ ? 0 : ImGuiInputTextFlags_Password);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(show_key_ ? "隐藏" : "显示", ImVec2(48, 0))) {
        show_key_ = !show_key_;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(public_mode_);
    if (ImGui::Button("复制", ImVec2(48, 0))) {
        ImGui::SetClipboardText(key_buf_);
        key_copied_until_ = Clock::now() + std::chrono::seconds(2);
    }
    ImGui::EndDisabled();
    if (Clock::now() < key_copied_until_) {
        ImGui::SameLine();
        ImGui::TextColored(kGreen, "已复制");
    }
    ImGui::TextDisabled("keyID %08X", key_id());
    if (public_mode_) {
        ImGui::TextWrapped("公开模式：内置公开密钥，\n任何人安装 OBSCure 后都能解码。");
    } else {
        ImGui::TextWrapped("两端必须一致；解码中修改后点“应用设置”。");
    }
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
    if (export_.running && !export_.for_decode && export_.encoder) {
        srv = export_.encoder->output_srv();
        width = 1920;
        height = 1080;
        label = "导出预览 1920x1080";
    } else if (enc_.show_transport && enc_.encoder) {
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
    const bool has_player =
        enc_.source == EncodeState::Source::File && enc_.file.is_open();
    draw_image_fit(srv, width, height, label.c_str(), has_player ? 150.0f : 52.0f);
    if (has_player) draw_player_bar(false);

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
    if (detached_preview_) {
        ImGui::BeginChild("##enc_actions", ImVec2(0, 104), ImGuiChildFlags_None);
        if (enc_.running) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.55f, 0.16f, 0.16f, 1.0f));
            if (ImGui::Button("停止", ImVec2(-1, 34))) stop_encode();
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
            if (ImGui::Button("开始混淆", ImVec2(-1, 34))) start_encode();
            ImGui::PopStyleColor();
        }
        ImGui::TextWrapped("%s · 渲染 %.1f fps",
                           enc_.running ? "编码中" : "已停止", enc_.fps);
        if (enc_.output_window) {
            if (ImGui::Button("显示输出窗口", ImVec2(-1, 28))) {
                enc_.output_window->show();
            }
        }
        ImGui::EndChild();
        return;
    }

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
    ImGui::EndChild();
}

void GuiApp::Impl::draw_decode() {
    const float action_h = detached_preview_ ? 116.0f : 62.0f;
    const float content_h = std::max(240.0f, ImGui::GetContentRegionAvail().y - action_h);
    const float left_w = detached_preview_
                             ? std::min(560.0f, ImGui::GetContentRegionAvail().x)
                             : 370.0f;
    ImGui::BeginChild("##dec_left", ImVec2(left_w, content_h),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    draw_decode_source();
    ImGui::Separator();
    draw_presets();
    ImGui::Separator();
    ImGui::Checkbox("颜色还原", &dec_.use_color);
    ImGui::Checkbox("自动匹配视频设置", &auto_match_settings_);
    draw_video_info();
    ImGui::TextWrapped("修改档位/密钥/颜色后，\n点“应用设置”立即生效。");
    ImGui::Separator();
    ImGui::TextDisabled("边缘内缩");
    const char* margin_items[] = {"自动", "0 px", "1 px", "2 px"};
    int margin_index = margin_override_ + 1;
    if (ImGui::Combo("##dec_margin", &margin_index, margin_items, 4)) {
        margin_override_ = margin_index - 1;
        if (margin_override_ >= 0) dec_.margin = margin_override_;
    }
    ImGui::TextWrapped("自动：大窗口内缩 1px。\nc64 1:1 也会内缩，减少 cell 白边。");
    ImGui::Separator();
    draw_key();
    draw_export_ui(true);
    ImGui::EndChild();

    if (!detached_preview_) {
        ImGui::SameLine();
        ImGui::BeginChild("##dec_preview", ImVec2(0, content_h),
                          ImGuiChildFlags_Borders);
        draw_decode_preview();
        ImGui::EndChild();
    }

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
            if (!path.empty()) {
                dec_.file_path = path;
                probe_file(true, path);
            }
        }
        if (!dec_.file_path.empty()) {
            ImGui::TextWrapped("%s", wide_to_utf8(dec_.file_path).c_str());
        } else {
            ImGui::TextDisabled("未选择文件");
        }
        ImGui::Checkbox("播放器录屏（启用定位）", &dec_.detect_file);
        ImGui::TextWrapped("精确传输帧不勾选；播放器缩放/录屏时勾选。");
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
    ImGui::TextWrapped("文件模式默认直接解码。\n播放器录屏请勾选“启用定位”。");
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
    if (export_.running && export_.for_decode && export_.decoder) {
        srv = export_.decoder->restored_srv();
        width = export_.width;
        height = export_.height;
        char label_buf[64]{};
        sprintf_s(label_buf, "导出预览 %dx%d", width, height);
        label = label_buf;
    } else if (dec_.show_capture && dec_.source_srv) {
        srv = dec_.source_srv.Get();
        width = dec_.source_w;
        height = dec_.source_h;
        label = "捕获画面";
    } else if (dec_.decoder) {
        srv = dec_.decoder->restored_srv();
        width = dec_.decoder->layout().ref_w();
        height = dec_.decoder->layout().ref_h();
        char label_buf[64]{};
        sprintf_s(label_buf, "还原画面 %dx%d", width, height);
        label = label_buf;
    } else {
        label = "等待输入源";
    }
    const bool has_player =
        dec_.source == DecodeState::Source::File && dec_.file.is_open();
    draw_image_fit(srv, width, height, label.c_str(), has_player ? 150.0f : 78.0f);
    if (has_player) draw_player_bar(true);

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
    ImGui::TextDisabled("margin %d · 检测 %.0f ms", dec_.margin, dec_.detect_ms);
    if (dec_.source_w > 0) {
        ImGui::TextDisabled("捕获 %dx%d · 解码 %.1f fps", dec_.source_w, dec_.source_h,
                            dec_.fps);
        if (dec_.source == DecodeState::Source::File && dec_.running) {
            ImGui::TextDisabled("源 %.1f fps · 丢帧 %llu", dec_.file.fps(),
                                static_cast<unsigned long long>(dec_.file.dropped()));
        }
    }
    if (!dec_.error.empty()) {
        ImGui::TextWrapped("%s", dec_.error.c_str());
    }
}

void GuiApp::Impl::draw_preview_toolbar() {
    if (!detached_preview_) {
        if (ImGui::Button("分离播放画面", ImVec2(140, 28))) {
            set_detached_preview(true);
        }
        ImGui::SameLine();
        ImGui::TextWrapped("把播放画面放到独立窗口，控制台与画面可分别缩放。");
        return;
    }

    if (ImGui::Button("合并窗口", ImVec2(100, 28))) {
        set_detached_preview(false);
    }
    ImGui::SameLine();
    bool topmost = preview_window_ && preview_window_->topmost();
    if (ImGui::Checkbox("始终置顶 (F8)", &topmost)) {
        if (preview_window_) preview_window_->set_topmost(topmost);
        settings_.preview_topmost = topmost;
        save_settings();
    }
    if (preview_window_ && !preview_window_->visible()) {
        ImGui::SameLine();
        if (ImGui::Button("显示", ImVec2(60, 28))) {
            preview_window_->show();
        }
    }
    ImGui::TextWrapped("播放画面已分离");
    if (mode_ == Mode::Encode) {
        if (enc_.running) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.55f, 0.16f, 0.16f, 1.0f));
            if (ImGui::Button("停止混淆", ImVec2(110, 30))) stop_encode();
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
            if (ImGui::Button("开始混淆", ImVec2(110, 30))) start_encode();
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f fps", enc_.fps);
    } else {
        if (dec_.running) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.55f, 0.16f, 0.16f, 1.0f));
            if (ImGui::Button("停止解码", ImVec2(110, 30))) stop_decode();
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
            if (ImGui::Button("开始解码", ImVec2(110, 30))) start_decode();
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f fps", dec_.fps);
    }
}

bool GuiApp::Impl::ensure_preview_window() {
    if (preview_window_) return true;
    RECT rect{};
    const bool have_rect = settings_.preview_w >= 320 && settings_.preview_h >= 180 &&
                           settings_.preview_x != INT_MIN &&
                           settings_.preview_y != INT_MIN;
    if (have_rect) {
        rect = {settings_.preview_x, settings_.preview_y,
                settings_.preview_x + settings_.preview_w,
                settings_.preview_y + settings_.preview_h};
    } else {
        RECT main_rect{};
        GetWindowRect(hwnd_, &main_rect);
        rect = {main_rect.right + 16, main_rect.top,
                main_rect.right + 16 + 960, main_rect.top + 540};
    }
    rect = clamp_window_rect(rect, 320, 180);

    auto window = std::make_unique<PreviewWindow>();
    std::string error;
    if (!window->init(device_.Get(), context_.Get(), rect, L"OBScure 播放画面",
                      resolve_shader_path(L"blit.hlsl"), &error)) {
        set_error("创建播放窗口失败: " + error);
        return false;
    }
    preview_window_ = std::move(window);
    preview_window_->set_topmost(settings_.preview_topmost);
    return true;
}

void GuiApp::Impl::restore_merged_window_rect() {
    g_console_min_size = false;
    RECT rect{};
    const bool have_rect = settings_.window_w >= 1000 && settings_.window_h >= 680 &&
                           settings_.window_x != INT_MIN &&
                           settings_.window_y != INT_MIN;
    if (have_rect) {
        rect = {settings_.window_x, settings_.window_y,
                settings_.window_x + settings_.window_w,
                settings_.window_y + settings_.window_h};
    } else {
        rect = {0, 0, 1280, 820};
        RECT adjusted = rect;
        AdjustWindowRect(&adjusted, WS_OVERLAPPEDWINDOW, FALSE);
        rect = {0, 0, adjusted.right - adjusted.left,
                adjusted.bottom - adjusted.top};
    }
    rect = clamp_window_rect(rect, 1000, 680);
    ShowWindow(hwnd_, SW_RESTORE);
    SetWindowPos(hwnd_, nullptr, rect.left, rect.top, rect.right - rect.left,
                 rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    if (settings_.maximized) ShowWindow(hwnd_, SW_MAXIMIZE);
}

void GuiApp::Impl::apply_console_rect() {
    g_console_min_size = true;
    RECT console{};
    const bool have_console = settings_.console_w >= 380 &&
                              settings_.console_h >= 620 &&
                              settings_.console_x != INT_MIN &&
                              settings_.console_y != INT_MIN;
    if (have_console) {
        console = {settings_.console_x, settings_.console_y,
                   settings_.console_x + settings_.console_w,
                   settings_.console_y + settings_.console_h};
    } else {
        RECT main_rect{};
        GetWindowRect(hwnd_, &main_rect);
        const long height = std::max(500L, main_rect.bottom - main_rect.top);
        console = {main_rect.left, main_rect.top, main_rect.left + 480,
                   main_rect.top + height};
    }
    console = clamp_window_rect(console, 380, 620);
    ShowWindow(hwnd_, SW_RESTORE);
    SetWindowPos(hwnd_, nullptr, console.left, console.top,
                 console.right - console.left, console.bottom - console.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void GuiApp::Impl::place_preview_to_right() {
    if (!preview_window_) return;
    RECT main_rect{};
    GetWindowRect(hwnd_, &main_rect);
    RECT prev{};
    preview_window_->get_window_rect(&prev);
    int prev_w = prev.right - prev.left;
    int prev_h = prev.bottom - prev.top;
    if (prev_w < 320 || prev_h < 180) {
        prev_w = 960;
        prev_h = 540;
    }
    if (prev.left < main_rect.right + 8) {
        preview_window_->set_window_rect(
            {main_rect.right + 16, main_rect.top,
             main_rect.right + 16 + prev_w, main_rect.top + prev_h});
    }
}

void GuiApp::Impl::restore_detached_preview() {
    if (!detached_preview_) return;
    apply_console_rect();
    if (!ensure_preview_window()) {
        detached_preview_ = false;
        restore_merged_window_rect();
        return;
    }
    place_preview_to_right();
    preview_window_->set_topmost(settings_.preview_topmost);
    preview_window_->show();
}

void GuiApp::Impl::set_detached_preview(bool detached) {
    if (detached == detached_preview_) return;
    if (!detached) {
        store_main_rect(true);
        if (preview_window_) preview_window_->hide();
        detached_preview_ = false;
        settings_.detached_preview = false;
        restore_merged_window_rect();
        save_settings();
        return;
    }

    // Remember the wide, merged window before shrinking to the console strip.
    store_main_rect(false);
    detached_preview_ = true;
    apply_console_rect();
    if (!ensure_preview_window()) {
        detached_preview_ = false;
        restore_merged_window_rect();
        return;
    }
    place_preview_to_right();
    preview_window_->set_topmost(settings_.preview_topmost);
    preview_window_->show();
    settings_.detached_preview = true;
    save_settings();
}

void GuiApp::Impl::update_preview_player() {
    if (!detached_preview_ || !preview_window_) return;

    FileSource* file = nullptr;
    bool decode_mode = false;
    if (mode_ == Mode::Encode && enc_.source == EncodeState::Source::File &&
        enc_.file.is_open()) {
        file = &enc_.file;
    } else if (mode_ == Mode::Decode && dec_.source == DecodeState::Source::File &&
               dec_.file.is_open()) {
        file = &dec_.file;
        decode_mode = true;
    }

    if (!file) {
        preview_window_->set_player_state(false, false, false, true, 0, 0, 0);
        return;
    }

    preview_window_->set_player_state(true, true, file->paused(), file->loop(),
                                      file->position_ms(), file->duration_ms(),
                                      file->fps());
    PreviewPlayerCommand command;
    while (preview_window_->take_player_command(&command)) {
        switch (command.type) {
            case PreviewPlayerCommand::Type::TogglePause:
                file->set_paused(!file->paused());
                break;
            case PreviewPlayerCommand::Type::ToggleLoop:
                file->set_loop(!file->loop());
                settings_.player_loop = file->loop();
                save_settings();
                break;
            case PreviewPlayerCommand::Type::Seek: {
                cv::Mat frame;
                if (file->seek_ms(command.value, frame)) {
                    apply_player_seek(decode_mode, frame);
                }
                break;
            }
            default:
                break;
        }
    }
}

void GuiApp::Impl::update_detached_preview() {
    if (!detached_preview_ || !preview_window_) return;

    if (export_.running) {
        ID3D11ShaderResourceView* srv = nullptr;
        int width = 0;
        int height = 0;
        if (export_.encoder) {
            srv = export_.encoder->output_srv();
            width = 1920;
            height = 1080;
        } else if (export_.decoder) {
            srv = export_.decoder->restored_srv();
            width = export_.width;
            height = export_.height;
        }
        preview_window_->set_title(L"OBScure 播放画面 - 导出");
        preview_window_->present(srv, width, height);
        return;
    }

    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0;
    int height = 0;
    if (mode_ == Mode::Encode) {
        preview_window_->set_title(L"OBScure 播放画面 - 编码");
        if (enc_.show_transport && enc_.encoder) {
            srv = enc_.encoder->output_srv();
            width = 1920;
            height = 1080;
        } else if (enc_.source_srv) {
            srv = enc_.source_srv.Get();
            width = enc_.source_w;
            height = enc_.source_h;
        }
    } else {
        preview_window_->set_title(L"OBScure 播放画面 - 解码");
        if (dec_.show_capture && dec_.source_srv) {
            srv = dec_.source_srv.Get();
            width = dec_.source_w;
            height = dec_.source_h;
        } else if (dec_.decoder) {
            srv = dec_.decoder->restored_srv();
            width = dec_.decoder->layout().ref_w();
            height = dec_.decoder->layout().ref_h();
        }
    }
    preview_window_->present(srv, width, height);
}

void GuiApp::Impl::probe_file(bool decode_mode, const std::wstring& path) {
    if (!export_.running) {
        export_.message.clear();
        export_.error.clear();
    }
    double duration_ms = 0.0;
    double fps = 60.0;
    int frames = 0;
    bool valid = false;
    if (!path.empty()) {
        FileSource probe;
        std::string error;
        if (probe.open(path, &error)) {
            duration_ms = probe.duration_ms();
            fps = probe.fps();
            frames = probe.total_frames();
            valid = true;
            probe.close();
        }
    }
    if (decode_mode) {
        dec_.probe_duration_ms = duration_ms;
        dec_.probe_fps = fps;
        dec_.probe_frames = frames;
        dec_.probe_valid = valid;
    } else {
        enc_.probe_duration_ms = duration_ms;
        enc_.probe_fps = fps;
        enc_.probe_frames = frames;
        enc_.probe_valid = valid;
    }
    std::wstring& output = decode_mode ? export_path_decode_ : export_path_encode_;
    std::wstring& known_source =
        decode_mode ? settings_.export_source_decode : settings_.export_source_encode;
    if (output.empty() || path != known_source) {
        output = default_export_path(decode_mode, path);
        known_source = path;
        if (decode_mode) {
            settings_.export_path_decode = output;
        } else {
            settings_.export_path_encode = output;
        }
        save_settings();
    }
}

std::wstring GuiApp::Impl::default_export_path(bool for_decode,
                                              const std::wstring& source_path) {
    namespace fs = std::filesystem;
    if (source_path.empty()) return {};
    fs::path source(source_path);
    fs::path dir = settings_.export_dir.empty() ? source.parent_path()
                                                : fs::path(settings_.export_dir);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) dir = source.parent_path();
    std::wstring stem = source.stem().wstring();
    stem += for_decode ? L"_restored" : L"_obscured";
    const std::string extension =
        kExportFormats[std::clamp(export_format_index_, 0, kExportFormatCount - 1)]
            .extension;
    return (dir / (stem + L"." + utf8_to_wide(extension))).wstring();
}

double GuiApp::Impl::export_source_duration_ms(bool for_decode) const {
    if (for_decode) {
        if (dec_.file.is_open()) return dec_.file.duration_ms();
        return dec_.probe_duration_ms;
    }
    if (enc_.file.is_open()) return enc_.file.duration_ms();
    return enc_.probe_duration_ms;
}

bool GuiApp::Impl::start_export(bool for_decode) {
    if (export_.running) return false;
    if (enc_.running || dec_.running) {
        set_error("请先停止实时编码/解码，再开始导出。");
        return false;
    }

    const std::wstring& source_path =
        for_decode ? dec_.file_path : enc_.file_path;
    std::wstring& output_path =
        for_decode ? export_path_decode_ : export_path_encode_;
    if (source_path.empty()) {
        set_error("请先选择源文件。");
        return false;
    }
    if (output_path.empty()) {
        output_path = default_export_path(for_decode, source_path);
    }
    const int format_index =
        std::clamp(export_format_index_, 0, kExportFormatCount - 1);
    output_path = replace_extension(output_path,
                                    kExportFormats[format_index].extension);

    ExportState state;
    state.for_decode = for_decode;
    state.source_path = source_path;
    state.output_path = output_path;
    state.format = kExportFormats[format_index].extension;
    state.codec = kExportFormats[format_index].codec;
    state.bitrate = std::clamp(export_bitrate_, 1, 100);

    std::string error;
    state.file = std::make_unique<FileSource>();
    if (!state.file->open(source_path, &error)) {
        set_error("打开导出源失败: " + error);
        return false;
    }
    state.file->set_loop(false);
    state.fps = std::clamp(state.file->fps(), 1.0, 240.0);
    state.total_frames = state.file->total_frames();
    if (state.total_frames <= 0 && state.file->duration_ms() > 0.0) {
        state.total_frames = static_cast<int>(
            std::lround(state.file->duration_ms() / 1000.0 * state.fps));
    }

    if (!for_decode) {
        const Preset& preset = kPresets[preset_index_];
        Layout layout;
        layout.cell = preset.cell;
        layout.guard = preset.guard;
        if (!public_mode_ && key_buf_[0] == '\0') {
            set_error("密钥不能为空。");
            return false;
        }
        layout.key = public_mode_ ? std::string(kPublicKey) : std::string(key_buf_);
        layout.validate();
        const KeyMaterial key = derive_key(layout);
        state.layout = layout;
        state.key = key;
        state.use_color = enc_use_color_;
        state.use_dihedral = true;
        state.transport.use_color = enc_use_color_;
        state.transport.use_dihedral = true;
        state.transport.public_mode = public_mode_;
        state.transport.preset_id = preset_index_ + 1;
        state.transport.scale_x10 = preset_scale_x10(preset);
        state.encoder = std::make_unique<EncodePipeline>();
        if (!state.encoder->init(device_.Get(), context_.Get(), layout, key,
                                 preset.scale, state.transport,
                                 resolve_shader_path(L"transport.hlsl"), &error)) {
            set_error("导出编码器初始化失败: " + error);
            return false;
        }
        state.width = 1920;
        state.height = 1080;
    } else {
        cv::Mat first;
        if (!state.file->read_sequential(first) || first.empty()) {
            set_error("读取导出源第一帧失败。");
            return false;
        }
        state.first_frame = first;
        state.pending_first_frame = true;

        Layout layout;
        layout.cell = kPresets[preset_index_].cell;
        layout.guard = kPresets[preset_index_].guard;
        layout.key = public_mode_ ? std::string(kPublicKey) : std::string(key_buf_);
        layout.validate();
        KeyMaterial key = derive_key(layout);
        bool use_color = dec_.use_color;
        bool use_dihedral = dec_.use_dihedral;
        std::optional<Metadata> applied_meta;

        const cv::Mat identity = identity_scaled(first.cols, first.rows);
        auto metadata = decode_metadata(first, identity, layout);
        auto apply_metadata = [&](const Metadata& md, bool& ok, std::string& why) {
            auto target = layout_from_metadata(md);
            if (!target) {
                why = "视频元数据几何参数非法。";
                ok = false;
                return;
            }
            layout = *target;
            applied_meta = md;
            if (md.public_mode()) {
                public_mode_ = true;
                layout.key = kPublicKey;
            } else {
                public_mode_ = false;
                layout.key = key_buf_;
            }
            key = derive_key(layout);
            use_color = md.use_color();
            use_dihedral = md.use_dihedral();
            if (const auto* p = preset_by_id(md.preset_id)) {
                const int idx = preset_index_by_id(p->id);
                if (idx >= 0) preset_index_ = idx;
            } else {
                const int idx = preset_index_by_geometry(md.cell, md.guard, md.scale_x10);
                if (idx >= 0) preset_index_ = idx;
            }
            if (!md.public_mode() && md.key_id != key.key_id) {
                char buffer[160]{};
                sprintf_s(buffer, "密钥不匹配：视频 keyID %08X，当前 keyID %08X",
                          md.key_id, key.key_id);
                why = buffer;
                ok = false;
            }
        };

        bool metadata_ok = metadata.has_value();
        if (metadata_ok) {
            bool ok = true;
            std::string why;
            apply_metadata(*metadata, ok, why);
            if (!ok) {
                set_error(why);
                return false;
            }
        }

        cv::Mat H;
        bool detection_mode = dec_.detect_file;
        if (detection_mode) {
            DetectionOutcome outcome;
            DetectionWorker::detect(first, layout, key, &outcome);
            if (!outcome.ok) {
                set_error("无法定位四角标记，不能导出。");
                return false;
            }
            H = outcome.H;
            if (!metadata_ok && outcome.metadata) {
                bool ok = true;
                std::string why;
                apply_metadata(*outcome.metadata, ok, why);
                if (!ok) {
                    set_error(why);
                    return false;
                }
                metadata_ok = true;
            }
        } else {
            H = identity;
            if (!metadata_ok) {
                DetectionOutcome outcome;
                DetectionWorker::detect(first, layout, key, &outcome);
                if (outcome.ok) {
                    detection_mode = true;
                    H = outcome.H;
                    if (outcome.metadata) {
                        bool ok = true;
                        std::string why;
                        apply_metadata(*outcome.metadata, ok, why);
                        if (!ok) {
                            set_error(why);
                            return false;
                        }
                    }
                }
            }
        }
        if (H.empty()) {
            set_error("无法确定导出画面的定位。");
            return false;
        }

        state.layout = layout;
        state.key = key;
        state.use_color = use_color;
        state.use_dihedral = use_dihedral;
        dec_.use_color = use_color;
        dec_.use_dihedral = use_dihedral;
        if (applied_meta) dec_.video_meta = *applied_meta;
        state.H = H;
        state.detection_mode = detection_mode;
        state.decoder = std::make_unique<DecodePipeline>();
        if (!state.decoder->init(device_.Get(), context_.Get(), layout, key, use_color,
                                 use_dihedral, resolve_shader_path(L"decode.hlsl"),
                                 &error)) {
            set_error("导出解码器初始化失败: " + error);
            return false;
        }
        state.width = layout.ref_w();
        state.height = layout.ref_h();
    }

    state.sink = std::make_unique<FfmpegSink>();
    const VideoCodec codec = state.codec == "h265"   ? VideoCodec::H265
                             : state.codec == "av1" ? VideoCodec::Av1
                                                    : VideoCodec::H264;
    if (!state.sink->start(device_.Get(), context_.Get(), state.width, state.height,
                           state.fps, state.bitrate, state.output_path, &error,
                           codec)) {
        set_error("启动导出编码失败: " + error);
        return false;
    }

    state.started = Clock::now();
    export_ = std::move(state);
    export_.running = true;

    settings_.export_dir = std::filesystem::path(output_path).parent_path().wstring();
    settings_.export_format = export_.format;
    settings_.export_codec = export_.codec;
    settings_.export_bitrate = export_bitrate_;
    if (for_decode) {
        settings_.export_path_decode = output_path;
        settings_.export_source_decode = source_path;
    } else {
        settings_.export_path_encode = output_path;
        settings_.export_source_encode = source_path;
    }
    save_settings();
    log_line(std::string("export started: ") + (for_decode ? "decode" : "encode") +
             " format=" + export_.format + " codec=" + export_.codec +
             " bitrate=" + std::to_string(export_.bitrate));
    return true;
}

void GuiApp::Impl::update_export() {
    if (!export_.running) return;
    if (export_.sink && !export_.sink->error().empty()) {
        export_.error = export_.sink->error();
        finish_export(false);
        return;
    }
    if (export_.cancel_requested) {
        finish_export(true);
        return;
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(8);
    int processed = 0;
    while (processed < 2 && Clock::now() < deadline && export_.running) {
        cv::Mat frame;
        if (export_.pending_first_frame) {
            frame = export_.first_frame;
            export_.pending_first_frame = false;
        } else if (!export_.file->read_sequential(frame) || frame.empty()) {
            finish_export(false);
            return;
        }

        if (export_.encoder) {
            upload_bgr(frame, export_.source_tex, export_.source_srv, export_.src_w,
                       export_.src_h);
            export_.encoder->render(export_.source_srv.Get());
            export_.sink->write(export_.encoder->output_texture());
        } else if (export_.decoder) {
            if (export_.detection_mode && !export_.H.empty() &&
                export_.done_frames > 0 && export_.done_frames % 120 == 0) {
                DetectionOutcome outcome;
                DetectionWorker::detect(frame, export_.layout, export_.key, &outcome);
                if (outcome.ok) {
                    export_.H = outcome.H;
                    export_.detect_failures = 0;
                } else {
                    ++export_.detect_failures;
                }
            }
            if (export_.H.empty()) {
                export_.error = "导出时无法定位四角标记。";
                finish_export(false);
                return;
            }
            const cv::Mat H = export_.detection_mode
                                  ? export_.H
                                  : identity_scaled(frame.cols, frame.rows);
            upload_bgr(frame, export_.source_tex, export_.source_srv, export_.src_w,
                       export_.src_h);
            export_.decoder->render(export_.source_srv.Get(), frame.cols, frame.rows,
                                    H);
            export_.sink->write(export_.decoder->restored_texture());
        } else {
            export_.error = "导出管线未初始化。";
            finish_export(false);
            return;
        }

        ++export_.done_frames;
        ++processed;
        if (export_.total_frames > 0 &&
            export_.done_frames >= export_.total_frames) {
            finish_export(false);
            return;
        }
        if (export_.sink && !export_.sink->error().empty()) {
            export_.error = export_.sink->error();
            finish_export(false);
            return;
        }
    }
}

void GuiApp::Impl::finish_export(bool cancelled) {
    if (!export_.running && !export_.sink) return;
    if (export_.sink) {
        export_.sink->stop();
        if (!export_.sink->error().empty()) export_.error = export_.sink->error();
        export_.sink.reset();
    }
    export_.encoder.reset();
    export_.decoder.reset();
    if (export_.file) {
        export_.file->close();
        export_.file.reset();
    }
    export_.running = false;
    export_.pending_first_frame = false;
    const std::string mode = export_.for_decode ? "解码" : "编码";
    if (cancelled) {
        export_.message = mode + "导出已取消";
    } else if (!export_.error.empty()) {
        export_.message = mode + "导出失败";
    } else {
        export_.message = mode + "导出完成";
    }
    save_settings();
    log_line(std::string("export finished: ") + export_.message +
             " frames=" + std::to_string(export_.done_frames));
}

void GuiApp::Impl::draw_export_ui(bool for_decode) {
    if (for_decode) {
        if (dec_.source != DecodeState::Source::File || dec_.file_path.empty()) return;
    } else {
        if (enc_.source != EncodeState::Source::File || enc_.file_path.empty()) return;
    }

    ImGui::Separator();
    ImGui::TextDisabled("导出视频（整段离线）");
    if (export_.running) {
        const float progress =
            export_.total_frames > 0
                ? std::clamp(static_cast<float>(export_.done_frames) /
                                 export_.total_frames,
                             0.0f, 1.0f)
                : 0.0f;
        char text[96]{};
        if (export_.total_frames > 0) {
            sprintf_s(text, "%d / %d 帧", export_.done_frames, export_.total_frames);
        } else {
            sprintf_s(text, "%d 帧", export_.done_frames);
        }
        ImGui::ProgressBar(progress, ImVec2(-1, 0), text);
        ImGui::TextDisabled("%s导出中 · %.1f 秒", export_.for_decode ? "解码" : "编码",
                            std::chrono::duration<double>(Clock::now() -
                                                          export_.started)
                                .count());
        if (ImGui::Button("取消导出", ImVec2(110, 30))) {
            export_.cancel_requested = true;
        }
        if (!export_.error.empty()) {
            ImGui::TextWrapped("%s", export_.error.c_str());
        }
        return;
    }

    const char* labels[kExportFormatCount]{};
    for (int i = 0; i < kExportFormatCount; ++i) {
        labels[i] = kExportFormats[i].label;
    }
    int format_index = std::clamp(export_format_index_, 0, kExportFormatCount - 1);
    ImGui::SetNextItemWidth(180);
    if (ImGui::Combo("格式##export", &format_index, labels, kExportFormatCount)) {
        export_format_index_ = format_index;
        std::wstring& path = for_decode ? export_path_decode_ : export_path_encode_;
        if (!path.empty()) {
            path = replace_extension(path, kExportFormats[format_index].extension);
        }
        settings_.export_format = kExportFormats[format_index].extension;
        settings_.export_codec = kExportFormats[format_index].codec;
        save_settings();
    }
    ImGui::SetNextItemWidth(100);
    if (ImGui::InputInt("码率 (Mbps)##export", &export_bitrate_)) {
        export_bitrate_ = std::clamp(export_bitrate_, 1, 100);
        settings_.export_bitrate = export_bitrate_;
    }

    const double duration_ms = export_source_duration_ms(for_decode);
    if (duration_ms > 0.0) {
        const double size_mb =
            export_bitrate_ * (duration_ms / 1000.0) / 8.0 * 1.01;
        ImGui::TextWrapped("预计文件大小：约 %.1f MB（%s @ %d Mbps）", size_mb,
                           format_time_ms(duration_ms).c_str(), export_bitrate_);
    } else {
        ImGui::TextWrapped("无法读取时长，预计大小不可用。");
    }

    std::wstring& path = for_decode ? export_path_decode_ : export_path_encode_;
    if (path.empty()) {
        path = default_export_path(for_decode, for_decode ? dec_.file_path : enc_.file_path);
    }
    ImGui::TextWrapped("%s", wide_to_utf8(path).c_str());
    if (ImGui::Button("选择导出路径...", ImVec2(-1, 0))) {
        const std::wstring default_name =
            std::filesystem::path(path).filename().wstring();
        const auto selected =
            save_file_dialog(L"导出视频", kVideoFilter, default_name);
        if (!selected.empty()) {
            path = replace_extension(selected,
                                     kExportFormats[export_format_index_].extension);
            settings_.export_dir =
                std::filesystem::path(path).parent_path().wstring();
            if (for_decode) {
                settings_.export_path_decode = path;
                settings_.export_source_decode = dec_.file_path;
            } else {
                settings_.export_path_encode = path;
                settings_.export_source_encode = enc_.file_path;
            }
            save_settings();
        }
    }

    const bool busy = enc_.running || dec_.running || export_.running;
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(for_decode ? "开始解码导出" : "开始混淆导出", ImVec2(-1, 32))) {
        start_export(for_decode);
    }
    ImGui::EndDisabled();
    if (enc_.running || dec_.running) {
        ImGui::TextWrapped("先停止实时运行，再开始导出。");
    }
    if (!export_.message.empty()) {
        ImGui::TextWrapped("%s", export_.message.c_str());
    }
    if (!export_.error.empty()) {
        ImGui::TextWrapped("%s", export_.error.c_str());
    }
}


void GuiApp::Impl::handle_player_hotkeys() {
    if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) return;
    FileSource* file = nullptr;
    bool decode_mode = false;
    if (mode_ == Mode::Encode && enc_.source == EncodeState::Source::File &&
        enc_.file.is_open()) {
        file = &enc_.file;
    } else if (mode_ == Mode::Decode && dec_.source == DecodeState::Source::File &&
               dec_.file.is_open()) {
        file = &dec_.file;
        decode_mode = true;
    }
    if (!file) return;

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        file->set_paused(!file->paused());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
        ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
        const double delta = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ? -5000.0
                                                                           : 5000.0;
        cv::Mat frame;
        if (file->seek_ms(file->position_ms() + delta, frame)) {
            apply_player_seek(decode_mode, frame);
        }
    }
}

void GuiApp::Impl::draw_player_bar(bool decode_mode) {
    FileSource& file = decode_mode ? dec_.file : enc_.file;
    bool& dragging = decode_mode ? dec_.player_dragging : enc_.player_dragging;
    float& player_pos = decode_mode ? dec_.player_pos : enc_.player_pos;
    Clock::time_point& last_seek =
        decode_mode ? dec_.player_last_seek : enc_.player_last_seek;

    if (!file.is_open()) return;

    const bool paused = file.paused();
    if (ImGui::Button(paused ? "播放" : "暂停", ImVec2(64, 0))) {
        file.set_paused(!paused);
    }
    ImGui::SameLine();
    const std::string position = format_time_ms(file.position_ms());
    const std::string duration = file.duration_ms() > 0
                                     ? format_time_ms(file.duration_ms())
                                     : "--:--";
    ImGui::Text("%s / %s", position.c_str(), duration.c_str());
    ImGui::SameLine();
    bool loop = file.loop();
    if (ImGui::Checkbox("循环", &loop)) {
        file.set_loop(loop);
        settings_.player_loop = loop;
    }

    const float duration_s =
        file.duration_ms() > 0 ? static_cast<float>(file.duration_ms() / 1000.0) : 0.0f;
    if (duration_s <= 0.0f) {
        ImGui::ProgressBar(0.0f, ImVec2(-1, 0), "时长未知");
        return;
    }

    if (!dragging) player_pos = static_cast<float>(file.position_ms() / 1000.0);
    ImGui::SetNextItemWidth(-1);
    const bool changed =
        ImGui::SliderFloat("##player_seek", &player_pos, 0.0f, duration_s, "%.1f s");
    const bool active = ImGui::IsItemActive();
    if (active) dragging = true;
    const auto now = Clock::now();
    const bool due = last_seek.time_since_epoch().count() == 0 ||
                     now - last_seek >= std::chrono::milliseconds(120);

    auto seek_now = [&] {
        cv::Mat frame;
        if (file.seek_ms(static_cast<double>(player_pos) * 1000.0, frame)) {
            apply_player_seek(decode_mode, frame);
        }
        last_seek = now;
    };
    if (changed && due) seek_now();
    if (!active && dragging) {
        dragging = false;
        seek_now();
    }
}

void GuiApp::Impl::apply_player_seek(bool decode_mode, const cv::Mat& frame) {
    if (frame.empty()) return;
    if (decode_mode) {
        const bool size_changed =
            dec_.source_w != frame.cols || dec_.source_h != frame.rows;
        dec_.last_file_frame = frame;
        upload_bgr(frame, dec_.source_tex, dec_.source_srv, dec_.tex_w, dec_.tex_h);
        dec_.tex_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        dec_.source_w = frame.cols;
        dec_.source_h = frame.rows;
        if (size_changed) {
            dec_.H_initial = identity_scaled(frame.cols, frame.rows);
            dec_.H = dec_.H_initial.clone();
            if (dec_.detect_file) reset_decode_detection();
        }
    } else {
        enc_.last_file_frame = frame;
        upload_bgr(frame, enc_.source_tex, enc_.source_srv, enc_.tex_w, enc_.tex_h);
        enc_.tex_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        enc_.source_w = frame.cols;
        enc_.source_h = frame.rows;
    }
}

void GuiApp::Impl::draw_video_info() {
    if (!dec_.video_meta) {
        if (dec_.running) ImGui::TextDisabled("视频信息：等待识别...");
        return;
    }
    const Metadata& m = *dec_.video_meta;
    std::ostringstream info;
    info << "视频信息：";
    if (m.public_mode()) {
        info << "公开视频（无需密钥）";
    } else {
        char id_buf[40]{};
        sprintf_s(id_buf, "需要密钥 keyID %08X", m.key_id);
        info << id_buf;
    }
    info << "\n档位：";
    if (const auto* p = preset_by_id(m.preset_id)) {
        info << p->id;
    } else {
        info << "自定义 cell " << m.cell << " · guard " << m.guard;
        if (m.scale_x10 > 0) {
            char scale_buf[24]{};
            sprintf_s(scale_buf, " · %.1fx", m.scale_x10 / 10.0);
            info << scale_buf;
        }
    }
    info << (m.use_color() ? " · 颜色混淆 开" : " · 颜色混淆 关");
    ImGui::TextWrapped("%s", info.str().c_str());
    if (!m.public_mode() && m.key_id != dec_.key.key_id) {
        ImGui::TextColored(kRed, "密钥不匹配：视频 keyID %08X，当前 keyID %08X",
                           m.key_id, dec_.key.key_id);
    }
}

void GuiApp::Impl::draw_decode_actions() {
    if (detached_preview_) {
        ImGui::BeginChild("##dec_actions", ImVec2(0, 108), ImGuiChildFlags_None);
        if (dec_.running) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.55f, 0.16f, 0.16f, 1.0f));
            if (ImGui::Button("停止", ImVec2(130, 34))) stop_decode();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("重新识别", ImVec2(140, 34))) reset_decode_detection();
            if (ImGui::Button("读取设置", ImVec2(130, 34))) request_video_settings();
            ImGui::SameLine();
            if (ImGui::Button("应用设置", ImVec2(110, 34))) {
                apply_manual_decode_settings();
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
            if (ImGui::Button("开始解码", ImVec2(-1, 34))) start_decode();
            ImGui::PopStyleColor();
        }
        ImGui::TextWrapped("%s · 解码 %.1f fps",
                           dec_.running ? "解码中" : "已停止", dec_.fps);
        ImGui::EndChild();
        return;
    }

    ImGui::BeginChild("##dec_actions", ImVec2(0, 56), ImGuiChildFlags_None);
    if (dec_.running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.16f, 1.0f));
        if (ImGui::Button("停止", ImVec2(150, 34))) stop_decode();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("重新识别定位点", ImVec2(180, 34))) reset_decode_detection();
        ImGui::SameLine();
        if (ImGui::Button("读取视频设置", ImVec2(150, 34))) request_video_settings();
        ImGui::SameLine();
        if (ImGui::Button("应用设置", ImVec2(110, 34))) apply_manual_decode_settings();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccentDim);
        if (ImGui::Button("开始解码", ImVec2(150, 34))) start_decode();
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", dec_.running ? "解码中" : "已停止");
    ImGui::SameLine();
    ImGui::TextDisabled("解码 %.1f fps", dec_.fps);
    if (dec_.running) {
        ImGui::SameLine();
        ImGui::TextDisabled("F5 读取设置");
    }
    if (dec_.fullscreen_window) {
        ImGui::SameLine();
        if (ImGui::Button("显示预览窗口")) dec_.fullscreen_window->show();
    }
    ImGui::EndChild();
}

void GuiApp::Impl::draw_status_bar() {
    ImGui::Separator();
    if (detached_preview_) {
        ImGui::TextDisabled("编码 %.1f fps · 解码 %.1f fps",
                            enc_.running ? enc_.fps : 0.0,
                            dec_.running ? dec_.fps : 0.0);
        return;
    }
    ImGui::TextDisabled("OBScure v0.3 · 分离窗口 / 文件播放 / 离线导出");
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label.c_str());
        ImGui::PopID();
    }
    if (picker_windows_.empty()) {
        ImGui::TextDisabled("没有找到可捕获的窗口。");
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##win_preview", ImVec2(0, child_height), ImGuiChildFlags_Borders);
    if (!picker_error_.empty()) {
        ImGui::TextWrapped("%s", picker_error_.c_str());
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
    if (export_.running) {
        set_error("导出中，请先等待或取消导出。");
        return false;
    }
    last_error_.clear();
    enc_.error.clear();
    try {
        if (!public_mode_ && key_buf_[0] == '\0') {
            set_error("密钥不能为空。");
            return false;
        }
        const Preset& preset = kPresets[preset_index_];
        Layout layout;
        layout.cell = preset.cell;
        layout.guard = preset.guard;
        layout.key = public_mode_ ? std::string(kPublicKey) : std::string(key_buf_);
        layout.validate();
        const KeyMaterial key = derive_key(layout);
        TransportOptions options;
        options.use_color = enc_use_color_;
        options.use_dihedral = true;
        options.public_mode = public_mode_;
        options.preset_id = preset_index_ + 1;
        options.scale_x10 = preset_scale_x10(preset);

        auto encoder = std::make_unique<EncodePipeline>();
        std::string error;
        if (!encoder->init(device_.Get(), context_.Get(), layout, key, preset.scale,
                           options, resolve_shader_path(L"transport.hlsl"), &error)) {
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
            enc_.file.set_loop(settings_.player_loop);
            enc_.probe_duration_ms = enc_.file.duration_ms();
            enc_.probe_fps = enc_.file.fps();
            enc_.probe_frames = enc_.file.total_frames();
            enc_.probe_valid = true;
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
    if (export_.running) {
        set_error("导出中，请先等待或取消导出。");
        return false;
    }
    last_error_.clear();
    dec_.error.clear();
    dec_.video_meta.reset();
    dec_.metadata_applied = false;
    dec_.metadata_read_pending = false;
    dec_.metadata_read_deadline = {};
    try {
        if (!public_mode_ && key_buf_[0] == '\0') {
            // An empty decode key means "public video"; private transports still
            // need the matching key entered before starting.
            public_mode_ = true;
        }
        const Preset& preset = kPresets[preset_index_];
        Layout layout;
        layout.cell = preset.cell;
        layout.guard = preset.guard;
        layout.key = public_mode_ ? std::string(kPublicKey) : std::string(key_buf_);
        layout.validate();
        KeyMaterial key = derive_key(layout);
        bool use_color = dec_.use_color;
        bool use_dihedral = true;

        auto fail = [this](const std::string& message) {
            set_error(message);
            stop_decode();
            return false;
        };

        std::string error;
        dec_.detect_tick = 0;
        if (dec_.detector) {
            dec_.detector->stop();
            dec_.detector.reset();
        }
        dec_.last_detect_frame.release();

        if (dec_.source == DecodeState::Source::File) {
            if (dec_.file_path.empty()) return fail("请先选择混淆视频。");
            if (!dec_.file.open(dec_.file_path, &error)) {
                return fail("打开混淆视频失败: " + error);
            }
            dec_.file.set_loop(settings_.player_loop);
            dec_.probe_duration_ms = dec_.file.duration_ms();
            dec_.probe_fps = dec_.file.fps();
            dec_.probe_frames = dec_.file.total_frames();
            dec_.probe_valid = true;
            cv::Mat first;
            if (!dec_.file.tick(first, true)) return fail("读取混淆视频帧失败。");
            dec_.last_file_frame = first;
            upload_bgr(first, dec_.source_tex, dec_.source_srv, dec_.tex_w, dec_.tex_h);
            dec_.tex_format = DXGI_FORMAT_B8G8R8A8_UNORM;
            dec_.source_w = first.cols;
            dec_.source_h = first.rows;
            dec_.H_initial = identity_scaled(first.cols, first.rows);
            dec_.H = dec_.H_initial.clone();
            dec_.margin = margin_override_ >= 0 ? margin_override_
                                                : adaptive_margin(dec_.H, layout.cell);

            // Exact transport files are full-frame and upright, so metadata can
            // be read directly through the scaled identity homography. This
            // avoids a visible settings jump on the first frame.
            if (!dec_.detect_file) {
                if (auto metadata = read_file_metadata()) {
                    dec_.video_meta = metadata;
                    auto target_layout = layout_from_metadata(*metadata);
                    if (auto_match_settings_ && target_layout) {
                        layout = *target_layout;
                        if (metadata->public_mode()) {
                            public_mode_ = true;
                            layout.key = kPublicKey;
                        } else {
                            public_mode_ = false;
                            layout.key = key_buf_;
                        }
                        key = derive_key(layout);
                        use_color = metadata->use_color();
                        use_dihedral = metadata->use_dihedral();
                        if (const auto* p = preset_by_id(metadata->preset_id)) {
                            const int idx = preset_index_by_id(p->id);
                            if (idx >= 0) preset_index_ = idx;
                        } else {
                            const int idx = preset_index_by_geometry(
                                metadata->cell, metadata->guard, metadata->scale_x10);
                            if (idx >= 0) preset_index_ = idx;
                        }
                        dec_.metadata_applied = true;
                    }
                    dec_.key_ok = metadata->public_mode()
                                      ? key.key_id == public_key_id()
                                      : metadata->key_id == key.key_id;
                    if (!dec_.key_ok) {
                        char buf[128]{};
                        sprintf_s(buf,
                                  "密钥不匹配：视频 keyID %08X，当前 keyID %08X",
                                  metadata->key_id, key.key_id);
                        dec_.error = buf;
                        log_line(std::string("decode metadata key mismatch: ") + buf);
                    }
                }
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
            dec_.margin = margin_override_ >= 0 ? margin_override_
                                                : adaptive_margin(dec_.H, layout.cell);
        }

        if (margin_override_ < 0) {
            dec_.margin = adaptive_margin(dec_.H, layout.cell);
        }
        if (!create_decode_pipeline(layout, key, use_color, use_dihedral, &error)) {
            return fail("解码器初始化失败: " + error);
        }
        const bool enable_detection =
            dec_.source != DecodeState::Source::File || dec_.detect_file;
        recreate_decode_detector(enable_detection);
        dec_.locked = !enable_detection;

        sync_decode_preview_window();

        dec_.stat_frames = 0;
        dec_.fps = 0.0;
        dec_.stat_start = Clock::now();
        dec_.running = true;
        log_line("decode started: preset=" + std::string(kPresets[preset_index_].id) +
                 " color=" + (use_color ? "on" : "off") +
                 " key=" + (public_mode_ ? "public" : "private"));
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
    dec_.video_meta.reset();
    dec_.metadata_applied = false;
    dec_.metadata_read_pending = false;
    dec_.metadata_read_deadline = {};
    dec_.key_ok = false;
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

bool GuiApp::Impl::create_decode_pipeline(const Layout& layout, const KeyMaterial& key,
                                          bool use_color, bool use_dihedral,
                                          std::string* error) {
    auto decoder = std::make_unique<DecodePipeline>();
    if (!decoder->init(device_.Get(), context_.Get(), layout, key, use_color,
                       use_dihedral, resolve_shader_path(L"decode.hlsl"), error)) {
        return false;
    }
    const int margin = margin_override_ >= 0 ? margin_override_ : dec_.margin;
    decoder->set_margin(std::max(0, margin));
    dec_.decoder = std::move(decoder);
    dec_.layout = layout;
    dec_.key = key;
    dec_.use_color = use_color;
    dec_.use_dihedral = use_dihedral;
    return true;
}

void GuiApp::Impl::recreate_decode_detector(bool enable) {
    if (dec_.detector) {
        dec_.detector->stop();
        dec_.detector.reset();
    }
    dec_.detect_tick = 0;
    if (!enable) return;
    dec_.detector = std::make_unique<DetectionWorker>(dec_.layout, dec_.key);
    dec_.detector->start();
    cv::Mat seed;
    if (dec_.source == DecodeState::Source::File && !dec_.last_file_frame.empty()) {
        seed = dec_.last_file_frame;
    } else if (!dec_.last_detect_frame.empty()) {
        seed = dec_.last_detect_frame;
    }
    if (!seed.empty()) dec_.detector->submit(seed);
}

void GuiApp::Impl::sync_decode_preview_window() {
    if (!dec_.fullscreen) {
        if (dec_.fullscreen_window) {
            dec_.fullscreen_window->shutdown();
            dec_.fullscreen_window.reset();
        }
        return;
    }
    if (!dec_.decoder) return;
    const int width = dec_.layout.ref_w();
    const int height = dec_.layout.ref_h();
    if (dec_.fullscreen_window && dec_.fullscreen_window->width() == width &&
        dec_.fullscreen_window->height() == height) {
        return;
    }
    auto window = std::make_unique<OutputWindow>();
    std::string error;
    if (!window->init(device_.Get(), context_.Get(), width, height, L"OBScure Restored",
                      resolve_shader_path(L"blit.hlsl"), &error)) {
        dec_.error = "独立预览窗口创建失败: " + error;
        return;
    }
    window->show();
    window->set_bottom();
    dec_.fullscreen_window = std::move(window);
}

std::optional<Metadata> GuiApp::Impl::read_file_metadata() {
    if (dec_.last_file_frame.empty()) return std::nullopt;
    const cv::Mat H =
        identity_scaled(dec_.last_file_frame.cols, dec_.last_file_frame.rows);
    return decode_metadata(dec_.last_file_frame, H, dec_.layout);
}

bool GuiApp::Impl::apply_video_metadata(const Metadata& metadata, bool force) {
    if (!force && !auto_match_settings_) return false;
    auto target_layout = layout_from_metadata(metadata);
    if (!target_layout) {
        dec_.metadata_applied = true;
        dec_.metadata_read_pending = false;
        dec_.error = "视频元数据几何参数非法，已忽略。";
        log_line("decode metadata rejected: invalid geometry");
        return false;
    }

    const bool want_public = metadata.public_mode();
    if (!want_public) {
        const uint32_t current_id =
            public_mode_ ? public_key_id() : key_id_from_text(key_buf_);
        if (metadata.key_id != current_id) {
            public_mode_ = false;
            dec_.video_meta = metadata;
            dec_.metadata_applied = true;
            dec_.metadata_read_pending = false;
            dec_.key_ok = false;
            dec_.use_color = metadata.use_color();
            dec_.use_dihedral = metadata.use_dihedral();
            if (const auto* p = preset_by_id(metadata.preset_id)) {
                const int idx = preset_index_by_id(p->id);
                if (idx >= 0) preset_index_ = idx;
            } else {
                const int idx = preset_index_by_geometry(metadata.cell, metadata.guard,
                                                         metadata.scale_x10);
                if (idx >= 0) preset_index_ = idx;
            }
            char buf[160]{};
            sprintf_s(buf, "密钥不匹配：视频 keyID %08X，当前 keyID %08X",
                      metadata.key_id, current_id);
            dec_.error = buf;
            log_line(std::string("decode metadata key mismatch: ") + buf);
            return false;
        }
    }

    Layout layout = *target_layout;
    if (margin_override_ < 0) {
        dec_.margin = adaptive_margin(dec_.H, layout.cell);
    }
    layout.key = want_public ? std::string(kPublicKey) : std::string(key_buf_);
    const KeyMaterial key = derive_key(layout);
    std::string error;
    if (!create_decode_pipeline(layout, key, metadata.use_color(),
                                metadata.use_dihedral(), &error)) {
        dec_.metadata_applied = true;
        dec_.metadata_read_pending = false;
        dec_.error = "应用视频设置失败: " + error;
        return false;
    }

    public_mode_ = want_public;
    if (const auto* p = preset_by_id(metadata.preset_id)) {
        const int idx = preset_index_by_id(p->id);
        if (idx >= 0) preset_index_ = idx;
    } else {
        const int idx = preset_index_by_geometry(metadata.cell, metadata.guard,
                                                 metadata.scale_x10);
        if (idx >= 0) preset_index_ = idx;
    }

    recreate_decode_detector(dec_.source != DecodeState::Source::File ||
                             dec_.detect_file);
    sync_decode_preview_window();
    dec_.video_meta = metadata;
    dec_.metadata_applied = true;
    dec_.metadata_read_pending = false;
    dec_.key_ok = true;
    dec_.error.clear();
    log_line("decode metadata applied: preset=" +
             std::string(preset_by_id(metadata.preset_id)
                             ? preset_by_id(metadata.preset_id)->id
                             : "custom") +
             " color=" + (metadata.use_color() ? "on" : "off") +
             " key=" + (want_public ? "public" : "private"));
    return true;
}

bool GuiApp::Impl::apply_manual_decode_settings() {
    if (!dec_.running) return false;
    if (!public_mode_ && key_buf_[0] == '\0') {
        set_error("密钥不能为空。");
        return false;
    }
    const Preset& preset = kPresets[preset_index_];
    Layout layout;
    layout.cell = preset.cell;
    layout.guard = preset.guard;
    layout.key = public_mode_ ? std::string(kPublicKey) : std::string(key_buf_);
    layout.validate();
    if (margin_override_ < 0) {
        dec_.margin = adaptive_margin(dec_.H, layout.cell);
    }
    const KeyMaterial key = derive_key(layout);
    std::string error;
    if (!create_decode_pipeline(layout, key, dec_.use_color, dec_.use_dihedral, &error)) {
        set_error("应用设置失败: " + error);
        return false;
    }
    recreate_decode_detector(dec_.source != DecodeState::Source::File ||
                             dec_.detect_file);
    sync_decode_preview_window();
    dec_.metadata_applied = true;  // manual changes win over later detection
    dec_.metadata_read_pending = false;
    if (dec_.video_meta) {
        dec_.key_ok = dec_.video_meta->public_mode()
                          ? public_mode_
                          : dec_.video_meta->key_id == key.key_id;
    } else {
        dec_.key_ok = true;
    }
    dec_.error.clear();
    log_line("decode settings applied: preset=" + std::string(preset.id) +
             " color=" + (dec_.use_color ? "on" : "off") +
             " key=" + (public_mode_ ? "public" : "private"));
    return true;
}

void GuiApp::Impl::request_video_settings() {
    if (!dec_.running) return;
    dec_.metadata_applied = false;
    dec_.metadata_read_pending = true;
    dec_.metadata_read_deadline = Clock::now() + std::chrono::seconds(3);
    dec_.error.clear();

    if (dec_.source == DecodeState::Source::File && !dec_.detect_file) {
        dec_.metadata_read_pending = false;
        if (auto metadata = read_file_metadata()) {
            apply_video_metadata(*metadata, true);
        } else {
            dec_.error =
                "未读到视频信息；如为播放器录屏，请勾选“播放器录屏（启用定位）”。";
        }
        return;
    }

    if (!dec_.detector) recreate_decode_detector(true);
    dec_.detect_tick = dec_.source == DecodeState::Source::File ? 29 : 59;
    log_line("decode metadata read requested");
}

void GuiApp::Impl::reset_decode_detection() {
    if (!dec_.running) return;
    dec_.H_initial = identity_scaled(dec_.source_w, dec_.source_h);
    dec_.H = dec_.H_initial.clone();
    dec_.margin = margin_override_ >= 0 ? margin_override_
                                        : adaptive_margin(dec_.H, dec_.layout.cell);
    dec_.locked = false;
    dec_.markers = 0;
    dec_.key_ok = dec_.video_meta
                      ? (dec_.video_meta->public_mode()
                             ? dec_.key.key_id == public_key_id()
                             : dec_.video_meta->key_id == dec_.key.key_id)
                      : false;
    dec_.detect_ms = 0.0;
    dec_.detect_tick = dec_.source == DecodeState::Source::File ? 29 : 59;
    dec_.last_detect_frame.release();
    dec_.last_detect_log = {};
    recreate_decode_detector(dec_.source != DecodeState::Source::File ||
                             dec_.detect_file);
    log_line("decode detection reset");
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
    const int auto_margin = snap ? adaptive_margin(dec_.H, dec_.layout.cell)
                                 : outcome.margin;
    dec_.margin = margin_override_ >= 0 ? margin_override_ : auto_margin;
    dec_.markers = outcome.markers;
    dec_.detect_ms = outcome.ms;
    dec_.locked = true;

    if (outcome.metadata) {
        dec_.video_meta = outcome.metadata;
        dec_.key_ok = outcome.metadata->public_mode()
                          ? dec_.key.key_id == public_key_id()
                          : outcome.metadata->key_id == dec_.key.key_id;
        dec_.metadata_read_pending = false;
        if (!dec_.metadata_applied) {
            apply_video_metadata(*outcome.metadata, false);
        }
    } else {
        dec_.key_ok = outcome.key_ok;
        if (dec_.metadata_read_pending) {
            dec_.metadata_read_pending = false;
            dec_.error = "未读到视频信息（定位已锁定，但元数据条码不可读）。";
        }
    }
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
            reset_decode_detection();
        }
        if (dec_.source_tex) dec_.capture->copy_latest(dec_.source_tex.Get());
    }

    if (dec_.metadata_read_pending &&
        dec_.metadata_read_deadline.time_since_epoch().count() != 0 &&
        Clock::now() > dec_.metadata_read_deadline) {
        dec_.metadata_read_pending = false;
        dec_.error = "读取视频设置超时：未检测到可读的元数据。";
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
