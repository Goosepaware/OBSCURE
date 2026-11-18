// OBScure live decoder: capture a transport window/file, locate the four
// markers, invert the tile transform on the GPU and show the restored image.
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "core/detect.h"
#include "core/key.h"
#include "core/layout.h"
#include "core/transform.h"
#include "live/d3d_helpers.h"
#include "live/wgc_capture.h"

namespace fs = std::filesystem;
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

struct Preset {
    int cell;
    int guard;
};

const std::map<std::string, Preset>& presets() {
    static const std::map<std::string, Preset> p = {
        {"c32_g2_s100", {32, 2}},
        {"c32_g2_s120", {32, 2}},
        {"c32_g2_s140", {32, 2}},
        {"c16_g1_s100", {16, 1}},
        {"c16_g2_s100", {16, 2}},
        {"c64_g2_s100", {64, 2}},
        {"c64_g2_s120", {64, 2}},
    };
    return p;
}

std::map<std::string, std::string> parse_args(int argc, char** argv) {
    std::map<std::string, std::string> out;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) != 0) continue;
        a = a.substr(2);
        if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
            out[a] = argv[++i];
        } else {
            out[a] = "1";
        }
    }
    return out;
}

std::string get(const std::map<std::string, std::string>& a, const std::string& k,
                const std::string& def) {
    auto it = a.find(k);
    return it == a.end() ? def : it->second;
}

int get_int(const std::map<std::string, std::string>& a, const std::string& k, int def) {
    auto it = a.find(k);
    if (it == a.end()) return def;
    return std::stoi(it->second);
}

double get_double(const std::map<std::string, std::string>& a, const std::string& k,
                  double def) {
    auto it = a.find(k);
    if (it == a.end()) return def;
    return std::stod(it->second);
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

std::string open_video_dialog() {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Video\0*.mp4;*.mkv;*.mov;*.avi;*.ts\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open obfuscated video";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return {};
    return wide_to_utf8(file);
}

struct alignas(16) DecodeParams {
    uint32_t canvasW;
    uint32_t canvasH;
    uint32_t contentX;
    uint32_t contentY;
    uint32_t cell;
    uint32_t guard;
    uint32_t cols;
    uint32_t rows;
    uint32_t inner;
    uint32_t refW;
    uint32_t refH;
    uint32_t captureW;
    uint32_t captureH;
    float margin;
    uint32_t useColor;
    uint32_t useDih;
    float H0[4];
    float H1[4];
    float H2[4];
};
static_assert(sizeof(DecodeParams) == 112, "decode params size");

struct MapEntry {
    float x;
    float y;
};

std::vector<uint32_t> build_src_from_dst(const obs::Layout& layout) {
    const int inner = layout.inner();
    std::vector<uint32_t> table(static_cast<size_t>(8 * inner * inner));
    cv::Mat probe(inner, inner, CV_16U);
    for (int code = 0; code < 8; ++code) {
        for (int y = 0; y < inner; ++y) {
            for (int x = 0; x < inner; ++x) {
                probe.at<ushort>(y, x) = static_cast<ushort>(y * inner + x);
            }
        }
        cv::Mat t;
        obs::apply_dihedral(probe, t, code);
        for (int y = 0; y < inner; ++y) {
            for (int x = 0; x < inner; ++x) {
                table[static_cast<size_t>(code * inner * inner + y * inner + x)] =
                    t.at<ushort>(y, x);
            }
        }
    }
    return table;
}

std::vector<uint32_t> build_dec_lut(const obs::Layout& layout,
                                    const obs::KeyMaterial& key) {
    std::vector<uint32_t> lut(static_cast<size_t>(layout.blocks()), 0);
    for (int d = 0; d < layout.blocks(); ++d) {
        const int b = key.perm[static_cast<size_t>(d)];
        const uint32_t color = key.color[static_cast<size_t>(d)] & 7u;
        const uint32_t dih = key.dihedral[static_cast<size_t>(d)] & 7u;
        lut[static_cast<size_t>(b)] =
            static_cast<uint32_t>(d) | (color << 16) | (dih << 19);
    }
    return lut;
}

std::vector<MapEntry> build_decode_map(const obs::Layout& layout, int margin) {
    const int inner = layout.inner();
    const auto src_from_dst = build_src_from_dst(layout);
    std::vector<uint32_t> dst_from_src(static_cast<size_t>(8 * inner * inner), 0);
    for (int code = 0; code < 8; ++code) {
        for (int dest = 0; dest < inner * inner; ++dest) {
            const uint32_t src = src_from_dst[static_cast<size_t>(code * inner * inner + dest)];
            dst_from_src[static_cast<size_t>(code * inner * inner + src)] =
                static_cast<uint32_t>(dest);
        }
    }

    std::vector<MapEntry> map(static_cast<size_t>(8 * inner * inner));
    const float scale = static_cast<float>(inner - 2 * margin) / static_cast<float>(inner);
    for (int code = 0; code < 8; ++code) {
        for (int v = 0; v < inner; ++v) {
            for (int u = 0; u < inner; ++u) {
                const float su =
                    static_cast<float>(margin) + (u + 0.5f) * scale - 0.5f;
                const float sv =
                    static_cast<float>(margin) + (v + 0.5f) * scale - 0.5f;
                const int sx = std::clamp(static_cast<int>(std::lround(su)), 0, inner - 1);
                const int sy = std::clamp(static_cast<int>(std::lround(sv)), 0, inner - 1);
                const uint32_t dest =
                    dst_from_src[static_cast<size_t>(code * inner * inner + sy * inner + sx)];
                MapEntry e;
                e.x = static_cast<float>(dest % inner) + 0.5f;
                e.y = static_cast<float>(dest / inner) + 0.5f;
                map[static_cast<size_t>(code * inner * inner + v * inner + u)] = e;
            }
        }
    }
    return map;
}

void set_identity_scaled(cv::Mat* H, int width, int height) {
    *H = cv::Mat::eye(3, 3, CV_64F);
    H->at<double>(0, 0) = static_cast<double>(width) / 1920.0;
    H->at<double>(1, 1) = static_cast<double>(height) / 1080.0;
}

bool run_detection(const cv::Mat& bgr, const obs::Layout& layout,
                   const obs::KeyMaterial& key, cv::Mat* H_full, int* margin,
                   bool* key_ok, double* detect_ms, int* markers) {
    if (bgr.empty()) return false;
    cv::Mat det = bgr;
    double sx = 1.0;
    double sy = 1.0;
    if (bgr.cols > 1920) {
        const int dw = 1920;
        const int dh = static_cast<int>(std::lround(bgr.rows * (1920.0 / bgr.cols)));
        cv::resize(bgr, det, cv::Size(dw, dh), 0, 0, cv::INTER_AREA);
        sx = static_cast<double>(bgr.cols) / dw;
        sy = static_cast<double>(bgr.rows) / dh;
    }
    const auto t0 = Clock::now();
    const auto res = obs::detect_homography(det);
    if (res.H.empty()) return false;
    cv::Mat scale = (cv::Mat_<double>(3, 3) << sx, 0, 0, 0, sy, 0, 0, 0, 1);
    *H_full = scale * res.H;
    *margin = obs::adaptive_margin(*H_full);
    *markers = res.marker_count;

    const auto md = obs::decode_metadata(det, res.H, layout);
    *key_ok = md && md->key_id == key.key_id;
    *detect_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return true;
}

struct DetectionOutcome {
    bool ok = false;
    cv::Mat H;
    int margin = 0;
    bool key_ok = false;
    int markers = 0;
    double ms = 0.0;
};

class DetectionWorker {
public:
    DetectionWorker(obs::Layout layout, obs::KeyMaterial key)
        : layout_(std::move(layout)), key_(std::move(key)) {}

    ~DetectionWorker() { stop(); }

    void start() {
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void submit(const cv::Mat& bgr) {
        if (bgr.empty()) return;
        {
            std::lock_guard lock(mutex_);
            frame_ = bgr.clone();
            has_frame_ = true;
        }
        cv_.notify_one();
    }

    bool take(DetectionOutcome* out) {
        std::lock_guard lock(mutex_);
        if (!has_result_) return false;
        *out = std::move(result_);
        has_result_ = false;
        return true;
    }

private:
    void run() {
        for (;;) {
            cv::Mat frame;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || has_frame_; });
                if (stop_ && !has_frame_) return;
                frame = std::move(frame_);
                has_frame_ = false;
            }

            DetectionOutcome out;
            out.ok = run_detection(frame, layout_, key_, &out.H, &out.margin,
                                   &out.key_ok, &out.ms, &out.markers);
            {
                std::lock_guard lock(mutex_);
                result_ = std::move(out);
                has_result_ = true;
            }
        }
    }

    obs::Layout layout_;
    obs::KeyMaterial key_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool has_frame_ = false;
    cv::Mat frame_;
    bool has_result_ = false;
    DetectionOutcome result_;
};

bool create_staging(ID3D11Device* device, int width, int height, DXGI_FORMAT format,
                    ComPtr<ID3D11Texture2D>* out) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, out->GetAddressOf()));
}

bool read_bgra(ID3D11DeviceContext* context, ID3D11Texture2D* source,
               ID3D11Texture2D* staging, int width, int height, cv::Mat* bgr) {
    context->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    cv::Mat bgra(height, width, CV_8UC4, mapped.pData, mapped.RowPitch);
    cv::cvtColor(bgra, *bgr, cv::COLOR_BGRA2BGR);
    context->Unmap(staging, 0);
    return true;
}

bool save_texture_png(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                      ID3D11Texture2D* staging, int width, int height,
                      const std::string& path) {
    context->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    cv::Mat bgra(height, width, CV_8UC4, mapped.pData, mapped.RowPitch);
    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    context->Unmap(staging, 0);
    return cv::imwrite(path, bgr);
}

}  // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto args = parse_args(argc, argv);
    if (args.find("help") != args.end()) {
        std::cout
            << "obscure-decode [options]\n"
            << "  (no arguments: open a video file picker)\n"
            << "  --window TITLE     capture a player window\n"
            << "  --hwnd HEX         capture a window by handle (avoids title encoding)\n"
            << "  --monitor N        capture monitor N\n"
            << "  --file PATH        decode a transport video (loop)\n"
            << "  --seconds N        run for N seconds (0 = until close, default 0)\n"
            << "  --config NAME      c32_g2_s120 (default), c32_g2_s100, c32_g2_s140,\n"
            << "                     c16_g2_s100, c16_g1_s100, c64_g2_s100, c64_g2_s120\n"
            << "  --key TEXT         shared key (default obscure-demo-key)\n"
            << "  --shader PATH      decode.hlsl path\n"
            << "  --margin auto|0|1|2 decoder inset (default auto)\n"
            << "  --detect           force marker detection in file mode\n"
            << "  --no-detect        use an identity/scaled homography\n"
            << "  --dump-every N     write restored PNGs every N frames\n"
            << "  --dump-dir PATH    dump directory\n";
        return 0;
    }

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        const std::string config = get(args, "config", "c32_g2_s120");
        const auto pit = presets().find(config);
        if (pit == presets().end()) {
            std::cerr << "unknown config\n";
            return 2;
        }
        obs::Layout layout;
        layout.cell = pit->second.cell;
        layout.guard = pit->second.guard;
        layout.key = get(args, "key", "obscure-demo-key");
        layout.validate();
        const obs::KeyMaterial key = obs::derive_key(layout);

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        std::string error;
        if (!obs::create_device(&device, &context, &error)) {
            std::cerr << error << "\n";
            return 1;
        }

        const std::string shader_path = get(args, "shader", "shaders/decode.hlsl");
        const auto vs_blob =
            obs::compile_shader(utf8_to_wide(shader_path), "VSMain", "vs_5_0", &error);
        const auto ps_blob =
            obs::compile_shader(utf8_to_wide(shader_path), "PSDecode", "ps_5_0", &error);
        const auto blit_blob =
            obs::compile_shader(utf8_to_wide(shader_path), "PSBlit", "ps_5_0", &error);
        if (!vs_blob || !ps_blob || !blit_blob) {
            std::cerr << "shader compile failed: " << error << "\n";
            return 1;
        }
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11PixelShader> ps_blit;
        device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                   nullptr, vs.GetAddressOf());
        device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                  nullptr, ps.GetAddressOf());
        device->CreatePixelShader(blit_blob->GetBufferPointer(),
                                  blit_blob->GetBufferSize(), nullptr,
                                  ps_blit.GetAddressOf());

        const int ref_w = layout.ref_w();
        const int ref_h = layout.ref_h();

        // Decode window and swap chain.
        const wchar_t kClass[] = L"OBScureDecoderWindow";
        WNDCLASSW wc{};
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        RegisterClassW(&wc);
        RECT rc{0, 0, ref_w, ref_h};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                         FALSE);
        HWND hwnd = CreateWindowExW(
            0, kClass, L"OBScure Decoder",
            WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, CW_USEDEFAULT,
            CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
            wc.hInstance, nullptr);
        if (!hwnd) {
            std::cerr << "window creation failed\n";
            return 1;
        }
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        ComPtr<IDXGIDevice> dxgi_device;
        device->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()));
        ComPtr<IDXGIAdapter> adapter;
        dxgi_device->GetAdapter(adapter.GetAddressOf());
        ComPtr<IDXGIFactory2> factory;
        adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
        DXGI_SWAP_CHAIN_DESC1 sc_desc{};
        sc_desc.Width = static_cast<UINT>(ref_w);
        sc_desc.Height = static_cast<UINT>(ref_h);
        sc_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sc_desc.SampleDesc.Count = 1;
        sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc_desc.BufferCount = 2;
        sc_desc.Scaling = DXGI_SCALING_STRETCH;
        sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sc_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> swapchain;
        if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sc_desc, nullptr,
                                                   nullptr, swapchain.GetAddressOf()))) {
            std::cerr << "swap chain creation failed\n";
            return 1;
        }
        ComPtr<ID3D11Texture2D> backbuffer;
        swapchain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()));
        ComPtr<ID3D11RenderTargetView> back_rtv;
        device->CreateRenderTargetView(backbuffer.Get(), nullptr, back_rtv.GetAddressOf());

        auto restored_tex = obs::create_texture(
            device.Get(), ref_w, ref_h, DXGI_FORMAT_B8G8R8A8_UNORM,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
        ComPtr<ID3D11RenderTargetView> restored_rtv;
        ComPtr<ID3D11ShaderResourceView> restored_srv;
        if (!restored_tex ||
            FAILED(device->CreateRenderTargetView(restored_tex.Get(), nullptr,
                                                  restored_rtv.GetAddressOf()))) {
            std::cerr << "restored render target creation failed\n";
            return 1;
        }
        restored_srv = obs::create_srv(device.Get(), restored_tex.Get());
        if (!restored_srv) {
            std::cerr << "restored SRV creation failed\n";
            return 1;
        }

        ComPtr<ID3D11Texture2D> restored_staging;
        create_staging(device.Get(), ref_w, ref_h, DXGI_FORMAT_B8G8R8A8_UNORM,
                       &restored_staging);

        DecodeParams params{};
        params.canvasW = static_cast<uint32_t>(layout.canvas_w);
        params.canvasH = static_cast<uint32_t>(layout.canvas_h);
        params.contentX = static_cast<uint32_t>(layout.content_x);
        params.contentY = static_cast<uint32_t>(layout.content_y);
        params.cell = static_cast<uint32_t>(layout.cell);
        params.guard = static_cast<uint32_t>(layout.guard);
        params.cols = static_cast<uint32_t>(layout.cols());
        params.rows = static_cast<uint32_t>(layout.rows());
        params.inner = static_cast<uint32_t>(layout.inner());
        params.refW = static_cast<uint32_t>(ref_w);
        params.refH = static_cast<uint32_t>(ref_h);
        params.margin = 0.0f;
        params.useColor = 1;
        params.useDih = 1;

        D3D11_BUFFER_DESC cb_desc{};
        cb_desc.ByteWidth = sizeof(DecodeParams);
        cb_desc.Usage = D3D11_USAGE_DYNAMIC;
        cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> cb;
        device->CreateBuffer(&cb_desc, nullptr, cb.GetAddressOf());

        const auto dec_lut = build_dec_lut(layout, key);
        auto lut_buf = obs::create_structured_buffer(
            device.Get(), dec_lut.size() * sizeof(uint32_t), sizeof(uint32_t),
            dec_lut.data());
        auto lut_srv = obs::create_buffer_srv(device.Get(), lut_buf.Get(),
                                              static_cast<UINT>(dec_lut.size()));

        int current_margin = -1;
        ComPtr<ID3D11Buffer> map_buf;
        ComPtr<ID3D11ShaderResourceView> map_srv;
        auto rebuild_map = [&](int margin) {
            const auto map = build_decode_map(layout, margin);
            map_buf = obs::create_structured_buffer(
                device.Get(), map.size() * sizeof(MapEntry), sizeof(MapEntry),
                map.data());
            map_srv = obs::create_buffer_srv(device.Get(), map_buf.Get(),
                                             static_cast<UINT>(map.size()));
            current_margin = margin;
            if (!map_buf || !map_srv) {
                std::cerr << "decode map creation failed\n";
            }
        };

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        device->CreateSamplerState(&sampler_desc, sampler.GetAddressOf());

        D3D11_RASTERIZER_DESC rasterizer_desc{};
        rasterizer_desc.FillMode = D3D11_FILL_SOLID;
        rasterizer_desc.CullMode = D3D11_CULL_NONE;
        rasterizer_desc.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rasterizer;
        device->CreateRasterizerState(&rasterizer_desc, rasterizer.GetAddressOf());

        // Source: file or WGC.
        std::string file_source = get(args, "file", "");
        const bool has_target = !file_source.empty() || args.find("window") != args.end() ||
                                args.find("hwnd") != args.end() ||
                                args.find("monitor") != args.end();
        if (!has_target) {
            file_source = open_video_dialog();
            if (file_source.empty()) return 0;
        }
        const bool file_mode = !file_source.empty();
        cv::VideoCapture file_cap;
        obs::WgcCapture capture;
        ComPtr<ID3D11Texture2D> source_tex;
        ComPtr<ID3D11ShaderResourceView> source_srv;
        ComPtr<ID3D11Texture2D> source_staging;
        int source_w = 0;
        int source_h = 0;
        DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;

        if (file_mode) {
            file_cap.open(file_source, cv::CAP_MSMF);
            if (!file_cap.isOpened()) file_cap.open(file_source, cv::CAP_ANY);
            if (!file_cap.isOpened()) {
                std::cerr << "cannot open video: " << file_source << "\n";
                return 1;
            }
            source_w = static_cast<int>(file_cap.get(cv::CAP_PROP_FRAME_WIDTH));
            source_h = static_cast<int>(file_cap.get(cv::CAP_PROP_FRAME_HEIGHT));
            source_format = DXGI_FORMAT_B8G8R8A8_UNORM;
            source_tex = obs::create_texture(device.Get(), source_w, source_h,
                                             source_format, D3D11_BIND_SHADER_RESOURCE,
                                             nullptr, 0);
            source_srv = obs::create_srv(device.Get(), source_tex.Get());
            create_staging(device.Get(), source_w, source_h, source_format,
                           &source_staging);
            std::cout << "[decode] file " << source_w << "x" << source_h << "\n";
        } else {
            HWND target_window = nullptr;
            HMONITOR target_monitor = nullptr;
            const std::string hwnd_arg = get(args, "hwnd", "");
            const std::string window_title = get(args, "window", "");
            if (!hwnd_arg.empty()) {
                const auto value = std::stoull(hwnd_arg, nullptr, 0);
                target_window = reinterpret_cast<HWND>(static_cast<uintptr_t>(value));
                if (!IsWindow(target_window)) {
                    std::cerr << "invalid hwnd\n";
                    return 1;
                }
                std::wcout << L"[decode] hwnd: " << target_window << L"\n";
            } else if (!window_title.empty()) {
                std::wstring found;
                if (!obs::find_window_by_title(utf8_to_wide(window_title), &target_window,
                                               &found)) {
                    std::cerr << "window not found\n";
                    return 1;
                }
                std::wcout << L"[decode] window: " << found << L"\n";
            } else {
                const int idx = get_int(args, "monitor", 0);
                std::wstring name;
                if (!obs::monitor_by_index(idx, &target_monitor, &name)) {
                    std::cerr << "monitor not found\n";
                    return 1;
                }
                std::wcout << L"[decode] monitor: " << name << L"\n";
            }
            if (!capture.start(device.Get(), context.Get(), target_window,
                               target_monitor, &error)) {
                std::cerr << "capture failed: " << error << "\n";
                return 1;
            }
            const auto wait_start = Clock::now();
            while (!capture.has_frame() &&
                   Clock::now() - wait_start < std::chrono::seconds(5)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (!capture.has_frame()) {
                std::cerr << "no capture frame\n";
                return 1;
            }
            std::lock_guard lock(capture.mutex());
            source_w = capture.width();
            source_h = capture.height();
            source_format = capture.format();
            std::cout << "[decode] capture " << source_w << "x" << source_h
                      << " format " << static_cast<int>(source_format) << "\n";
            source_tex = obs::create_texture(device.Get(), source_w, source_h,
                                             source_format, D3D11_BIND_SHADER_RESOURCE,
                                             nullptr, 0);
            source_srv = obs::create_srv(device.Get(), source_tex.Get());
            create_staging(device.Get(), source_w, source_h, source_format,
                           &source_staging);
        }

        if (!source_tex || !source_srv || !source_staging) {
            std::cerr << "source texture setup failed\n";
            return 1;
        }


        cv::Mat H;
        set_identity_scaled(&H, source_w, source_h);
        cv::Mat H_initial = H.clone();
        int margin = obs::adaptive_margin(H);
        const std::string margin_arg = get(args, "margin", "auto");
        const bool auto_margin = margin_arg == "auto";
        if (!auto_margin) margin = std::stoi(margin_arg);
        rebuild_map(margin);

        const bool force_detect = args.find("detect") != args.end();
        bool no_detect = args.find("no-detect") != args.end();
        // A transport file is already the canonical frame; skip detection for a
        // clean 1:1 decode. Use --detect for recordings of a scaled/cropped player.
        if (file_mode && !force_detect) no_detect = true;
        bool locked = no_detect;
        bool key_ok = false;
        int marker_count = 0;
        double detect_ms = 0.0;

        DetectionWorker detector(layout, key);
        if (!no_detect) detector.start();

        const double seconds = get_double(args, "seconds", 0.0);
        const int dump_every = get_int(args, "dump-every", 0);
        const std::string dump_dir = get(args, "dump-dir", "");
        if (!dump_dir.empty()) fs::create_directories(dump_dir);
        const auto start_time = Clock::now();
        const auto frame_duration = std::chrono::duration<double>(1.0 / 60.0);
        auto loop_start = start_time;
        uint64_t frame_index = 0;
        auto last_stats = start_time;
        uint64_t last_frame = 0;

        std::cout << "[decode] config=" << config << " ref=" << ref_w << "x" << ref_h
                  << " key=" << std::hex << std::uppercase << key.key_id << std::dec
                  << " margin=" << margin << "\n";

        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(ref_w);
        vp.Height = static_cast<float>(ref_h);
        vp.MaxDepth = 1.0f;

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
            if (seconds > 0.0 &&
                std::chrono::duration<double>(Clock::now() - start_time).count() >=
                    seconds) {
                break;
            }

            cv::Mat file_bgr;
            if (file_mode) {
                if (!file_cap.read(file_bgr) || file_bgr.empty()) {
                    file_cap.set(cv::CAP_PROP_POS_FRAMES, 0);
                    if (!file_cap.read(file_bgr) || file_bgr.empty()) break;
                }
                if (file_bgr.cols != source_w || file_bgr.rows != source_h) {
                    source_w = file_bgr.cols;
                    source_h = file_bgr.rows;
                    source_tex = obs::create_texture(
                        device.Get(), source_w, source_h, source_format,
                        D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
                    source_srv = obs::create_srv(device.Get(), source_tex.Get());
                    source_staging = nullptr;
                    create_staging(device.Get(), source_w, source_h, source_format,
                                   &source_staging);
                    set_identity_scaled(&H, source_w, source_h);
                    H_initial = H.clone();
                }
                cv::Mat bgra;
                cv::cvtColor(file_bgr, bgra, cv::COLOR_BGR2BGRA);
                if (!bgra.isContinuous()) bgra = bgra.clone();
                context->UpdateSubresource(source_tex.Get(), 0, nullptr, bgra.data,
                                           source_w * 4, 0);
            } else {
                std::lock_guard lock(capture.mutex());
                if (source_tex) capture.copy_latest(source_tex.Get());
                if (source_tex && dump_every > 0 && frame_index == 0 &&
                    !dump_dir.empty()) {
                    save_texture_png(context.Get(), source_tex.Get(),
                                     source_staging.Get(), source_w, source_h,
                                     (fs::path(dump_dir) / "source_tex_0.png").string());
                }
            }

            if (!no_detect && frame_index % 60 == 0) {
                cv::Mat bgr;
                if (file_mode) {
                    bgr = file_bgr;
                } else if (source_staging) {
                    read_bgra(context.Get(), source_tex.Get(), source_staging.Get(),
                              source_w, source_h, &bgr);
                }
                if (!bgr.empty()) detector.submit(bgr);
            }
            DetectionOutcome outcome;
            if (detector.take(&outcome)) {
                if (outcome.ok) {
                    bool snap = false;
                    if (!H_initial.empty() && !outcome.H.empty()) {
                        const double xs[4] = {static_cast<double>(layout.content_x),
                                              static_cast<double>(layout.content_x +
                                                                  layout.content_w),
                                              static_cast<double>(layout.content_x +
                                                                  layout.content_w),
                                              static_cast<double>(layout.content_x)};
                        const double ys[4] = {static_cast<double>(layout.content_y),
                                              static_cast<double>(layout.content_y),
                                              static_cast<double>(layout.content_y +
                                                                  layout.content_h),
                                              static_cast<double>(layout.content_y +
                                                                  layout.content_h)};
                        double max_delta = 0.0;
                        for (int i = 0; i < 4; ++i) {
                            auto map_pt = [](const cv::Mat& M, double x, double y) {
                                const double w = M.at<double>(2, 0) * x +
                                                 M.at<double>(2, 1) * y +
                                                 M.at<double>(2, 2);
                                return cv::Point2d(
                                    (M.at<double>(0, 0) * x + M.at<double>(0, 1) * y +
                                     M.at<double>(0, 2)) / w,
                                    (M.at<double>(1, 0) * x + M.at<double>(1, 1) * y +
                                     M.at<double>(1, 2)) / w);
                            };
                            const auto a = map_pt(H_initial, xs[i], ys[i]);
                            const auto b = map_pt(outcome.H, xs[i], ys[i]);
                            max_delta = std::max(max_delta, std::hypot(a.x - b.x, a.y - b.y));
                        }
                        snap = max_delta < 2.0;
                    }
                    H = snap ? H_initial.clone() : outcome.H;
                    if (auto_margin) {
                        margin = snap ? obs::adaptive_margin(H) : outcome.margin;
                    }
                    key_ok = outcome.key_ok;
                    marker_count = outcome.markers;
                    detect_ms = outcome.ms;
                    locked = true;
                } else {
                    locked = false;
                }
                if (margin != current_margin) rebuild_map(margin);
            }

            if (H.empty()) {
                set_identity_scaled(&H, source_w, source_h);
            }
            params.captureW = static_cast<uint32_t>(source_w);
            params.captureH = static_cast<uint32_t>(source_h);
            params.margin = static_cast<float>(margin);
            params.H0[0] = static_cast<float>(H.at<double>(0, 0));
            params.H0[1] = static_cast<float>(H.at<double>(0, 1));
            params.H0[2] = static_cast<float>(H.at<double>(0, 2));
            params.H0[3] = 0.0f;
            params.H1[0] = static_cast<float>(H.at<double>(1, 0));
            params.H1[1] = static_cast<float>(H.at<double>(1, 1));
            params.H1[2] = static_cast<float>(H.at<double>(1, 2));
            params.H1[3] = 0.0f;
            params.H2[0] = static_cast<float>(H.at<double>(2, 0));
            params.H2[1] = static_cast<float>(H.at<double>(2, 1));
            params.H2[2] = static_cast<float>(H.at<double>(2, 2));
            params.H2[3] = 0.0f;

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, &params, sizeof(params));
                context->Unmap(cb.Get(), 0);
            }

            ID3D11ShaderResourceView* dec_srvs[3] = {lut_srv.Get(), map_srv.Get(),
                                                     source_srv.Get()};
            ID3D11Buffer* cbs[1] = {cb.Get()};
            ID3D11SamplerState* samplers[1] = {sampler.Get()};

            context->OMSetRenderTargets(1, restored_rtv.GetAddressOf(), nullptr);
            context->RSSetViewports(1, &vp);
            context->RSSetState(rasterizer.Get());
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->PSSetShader(ps.Get(), nullptr, 0);
            context->PSSetConstantBuffers(0, 1, cbs);
            context->PSSetShaderResources(0, 3, dec_srvs);
            context->PSSetSamplers(0, 1, samplers);
            context->Draw(3, 0);

            ID3D11ShaderResourceView* nulls[3] = {nullptr, nullptr, nullptr};
            context->PSSetShaderResources(0, 3, nulls);

            // Blit restored texture to the window back buffer.
            context->OMSetRenderTargets(1, back_rtv.GetAddressOf(), nullptr);
            ID3D11ShaderResourceView* blit_srv[1] = {restored_srv.Get()};
            context->PSSetShader(ps_blit.Get(), nullptr, 0);
            context->PSSetShaderResources(3, 1, blit_srv);
            context->Draw(3, 0);
            ID3D11ShaderResourceView* null_blit[1] = {nullptr};
            context->PSSetShaderResources(3, 1, null_blit);
            context->OMSetRenderTargets(0, nullptr, nullptr);

            if (dump_every > 0 && frame_index % dump_every == 0 && !dump_dir.empty()) {
                const std::string path =
                    (fs::path(dump_dir) / ("restored_" + std::to_string(frame_index) +
                                           ".png"))
                        .string();
                save_texture_png(context.Get(), restored_tex.Get(),
                                 restored_staging.Get(), ref_w, ref_h, path);
            }

            swapchain->Present(1, 0);
            ++frame_index;

            const auto now = Clock::now();
            if (std::chrono::duration<double>(now - last_stats).count() >= 1.0) {
                const double sec = std::chrono::duration<double>(now - last_stats).count();
                std::cout << "[decode] fps=" << (frame_index - last_frame) / sec
                          << " lock=" << (locked ? 1 : 0) << " markers=" << marker_count
                          << " margin=" << margin << " key=" << (key_ok ? 1 : 0)
                          << " detect_ms=" << detect_ms << "\n";
                last_frame = frame_index;
                last_stats = now;
            }

            const auto next = loop_start + std::chrono::duration_cast<Clock::duration>(
                                               frame_duration *
                                               static_cast<double>(frame_index));
            const auto after = Clock::now();
            if (next > after) {
                std::this_thread::sleep_until(next);
            } else if (after - next > std::chrono::milliseconds(200)) {
                loop_start = after;
            }
        }

        detector.stop();
        if (!file_mode) capture.stop();
        winrt::uninit_apartment();
        std::cout << "[decode] stopped after " << frame_index << " frames\n";
        return 0;
    } catch (const winrt::hresult_error& e) {
        std::cerr << "winrt error: " << winrt::to_string(e.message()) << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
