// OBScure live encoder: WGC capture -> GPU tile obfuscation -> Spout2 -> OBS.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <winrt/base.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "SpoutDX.h"
#include "core/border.h"
#include "core/key.h"
#include "core/layout.h"
#include "core/metadata.h"
#include "core/presets.h"
#include "core/transform.h"
#include "live/d3d_helpers.h"
#include "live/wgc_capture.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI ctrl_handler(DWORD) {
    g_running.store(false);
    return TRUE;
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

struct alignas(16) ShaderParams {
    uint32_t canvasW;
    uint32_t canvasH;
    uint32_t contentX;
    uint32_t contentY;
    uint32_t contentW;
    uint32_t contentH;
    uint32_t cell;
    uint32_t guard;
    uint32_t cols;
    uint32_t rows;
    uint32_t inner;
    uint32_t refW;
    uint32_t refH;
    float extraScale;
    uint32_t pad0;
    uint32_t pad1;
};
static_assert(sizeof(ShaderParams) == 64, "shader params must be 64 bytes");

std::vector<uint32_t> build_lut(const obs::Layout& layout, const obs::KeyMaterial& key,
                                bool use_color) {
    std::vector<uint32_t> lut(static_cast<size_t>(layout.blocks()));
    for (int d = 0; d < layout.blocks(); ++d) {
        const uint32_t b = static_cast<uint32_t>(key.perm[static_cast<size_t>(d)]);
        const uint32_t color =
            use_color ? (key.color[static_cast<size_t>(d)] & 7u) : 0u;
        const uint32_t dih = key.dihedral[static_cast<size_t>(d)] & 7u;
        lut[static_cast<size_t>(d)] = b | (color << 16) | (dih << 19);
    }
    return lut;
}

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

std::string resolve_shader_path(const std::string& requested) {
    if (fs::exists(requested)) return fs::absolute(requested).string();
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path alt =
        fs::path(exe).parent_path().parent_path() / "shaders" / "transport.hlsl";
    if (fs::exists(alt)) return alt.string();
    return requested;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    const auto args = parse_args(argc, argv);
    if (args.find("help") != args.end()) {
        std::cout
            << "obscure-live [options]\n"
            << "  --window TITLE     capture a window (substring match)\n"
            << "  --monitor N        capture monitor N (default 0)\n"
            << "  --file PATH        use a video file as the source (loop)\n"
            << "  --seconds N        run for N seconds (0 = until Ctrl+C, default 20)\n"
            << "  --config NAME      c32_g2_s120 (default), c32_g2_s100, c32_g2_s140,\n"
            << "                     c16_g2_s100, c16_g1_s100, c64_g2_s100, c64_g2_s120\n"
            << "  --spout NAME       Spout sender name (default OBScure)\n"
            << "  --key TEXT         shared key (default obscure-demo-key)\n"
            << "  --public           use the built-in public key\n"
            << "  --no-color         disable color scrambling\n"
            << "  --shader PATH      transport.hlsl path\n"
            << "  --fps N            output frame rate (default 60)\n";
        return 0;
    }

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        const std::string config = get(args, "config", "c32_g2_s120");
        const int preset_index = obs::preset_index_by_id(config);
        if (preset_index < 0) {
            std::cerr << "unknown config: " << config << "\n";
            return 2;
        }
        const obs::Preset& preset = obs::kPresets[preset_index];
        const bool public_key = args.find("public") != args.end();
        const bool use_color = args.find("no-color") == args.end();

        obs::Layout layout;
        layout.cell = preset.cell;
        layout.guard = preset.guard;
        layout.key = public_key ? std::string(obs::kPublicKey)
                                : get(args, "key", "obscure-demo-key");
        layout.validate();
        const obs::KeyMaterial key = obs::derive_key(layout);
        obs::TransportOptions options;
        options.use_color = use_color;
        options.use_dihedral = true;
        options.public_mode = public_key;
        options.preset_id = preset_index + 1;
        options.scale_x10 = obs::preset_scale_x10(preset);

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        std::string error;
        if (!obs::create_device(&device, &context, &error)) {
            std::cerr << error << "\n";
            return 1;
        }

        const std::string shader_path =
            resolve_shader_path(get(args, "shader", "shaders/transport.hlsl"));
        const auto vs_blob = obs::compile_shader(utf8_to_wide(shader_path), "VSMain",
                                                 "vs_5_0", &error);
        const auto ps_blob = obs::compile_shader(utf8_to_wide(shader_path), "PSMain",
                                                 "ps_5_0", &error);
        if (!vs_blob || !ps_blob) {
            std::cerr << "shader compile failed: " << error << "\n";
            return 1;
        }
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                              vs_blob->GetBufferSize(), nullptr,
                                              vs.GetAddressOf())) ||
            FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                             ps_blob->GetBufferSize(), nullptr,
                                             ps.GetAddressOf()))) {
            std::cerr << "shader creation failed\n";
            return 1;
        }

        // Border texture (markers, metadata, calibration) generated once.
        const cv::Mat border = obs::build_border(layout, key, options);
        cv::Mat border_bgra;
        cv::cvtColor(border, border_bgra, cv::COLOR_BGR2BGRA);
        auto border_tex = obs::create_texture(
            device.Get(), layout.canvas_w, layout.canvas_h, DXGI_FORMAT_B8G8R8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE, border_bgra.data,
            static_cast<UINT>(layout.canvas_w * 4));
        auto border_srv = border_tex ? obs::create_srv(device.Get(), border_tex.Get())
                                     : nullptr;
        if (!border_tex || !border_srv) {
            std::cerr << "border texture creation failed\n";
            return 1;
        }

        const auto lut = build_lut(layout, key, use_color);
        const auto table = build_src_from_dst(layout);
        auto lut_buf = obs::create_structured_buffer(
            device.Get(), lut.size() * sizeof(uint32_t), sizeof(uint32_t), lut.data());
        auto table_buf = obs::create_structured_buffer(
            device.Get(), table.size() * sizeof(uint32_t), sizeof(uint32_t), table.data());
        auto lut_srv = lut_buf ? obs::create_buffer_srv(
                                     device.Get(), lut_buf.Get(),
                                     static_cast<UINT>(lut.size()))
                               : nullptr;
        auto table_srv = table_buf ? obs::create_buffer_srv(
                                         device.Get(), table_buf.Get(),
                                         static_cast<UINT>(table.size()))
                                   : nullptr;
        if (!lut_srv || !table_srv) {
            std::cerr << "LUT buffer creation failed\n";
            return 1;
        }

        ShaderParams params{};
        params.canvasW = static_cast<uint32_t>(layout.canvas_w);
        params.canvasH = static_cast<uint32_t>(layout.canvas_h);
        params.contentX = static_cast<uint32_t>(layout.content_x);
        params.contentY = static_cast<uint32_t>(layout.content_y);
        params.contentW = static_cast<uint32_t>(layout.content_w);
        params.contentH = static_cast<uint32_t>(layout.content_h);
        params.cell = static_cast<uint32_t>(layout.cell);
        params.guard = static_cast<uint32_t>(layout.guard);
        params.cols = static_cast<uint32_t>(layout.cols());
        params.rows = static_cast<uint32_t>(layout.rows());
        params.inner = static_cast<uint32_t>(layout.inner());
        params.refW = static_cast<uint32_t>(layout.ref_w());
        params.refH = static_cast<uint32_t>(layout.ref_h());
        params.extraScale = static_cast<float>(preset.scale);
        auto cb = obs::create_constant_buffer(device.Get(), sizeof(params), &params);
        if (!cb) {
            std::cerr << "constant buffer creation failed\n";
            return 1;
        }

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

        auto output_tex = obs::create_texture(
            device.Get(), layout.canvas_w, layout.canvas_h, DXGI_FORMAT_B8G8R8A8_UNORM,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
        if (!output_tex) {
            std::cerr << "output texture creation failed\n";
            return 1;
        }
        ComPtr<ID3D11RenderTargetView> output_rtv;
        if (FAILED(device->CreateRenderTargetView(output_tex.Get(), nullptr,
                                                  output_rtv.GetAddressOf()))) {
            std::cerr << "output RTV creation failed\n";
            return 1;
        }

        const uint32_t black = 0xFF000000u;
        auto fallback_tex = obs::create_texture(
            device.Get(), 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
            &black, sizeof(black));
        auto fallback_srv =
            fallback_tex ? obs::create_srv(device.Get(), fallback_tex.Get()) : nullptr;

        const std::string file_source = get(args, "file", "");
        const bool file_mode = !file_source.empty();
        cv::VideoCapture file_cap;
        obs::WgcCapture capture;
        ComPtr<ID3D11Texture2D> source_tex;
        ComPtr<ID3D11ShaderResourceView> source_srv;
        int source_w = 0;
        int source_h = 0;
        DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
        bool warned_format = false;

        if (file_mode) {
            file_cap.open(file_source, cv::CAP_MSMF);
            if (!file_cap.isOpened()) file_cap.open(file_source, cv::CAP_ANY);
            if (!file_cap.isOpened()) {
                std::cerr << "cannot open video: " << file_source << "\n";
                return 1;
            }
            source_w = static_cast<int>(file_cap.get(cv::CAP_PROP_FRAME_WIDTH));
            source_h = static_cast<int>(file_cap.get(cv::CAP_PROP_FRAME_HEIGHT));
            if (source_w <= 0 || source_h <= 0) {
                std::cerr << "bad video size\n";
                return 1;
            }
            source_format = DXGI_FORMAT_B8G8R8A8_UNORM;
            source_tex = obs::create_texture(device.Get(), source_w, source_h,
                                             source_format, D3D11_BIND_SHADER_RESOURCE,
                                             nullptr, 0);
            source_srv = source_tex ? obs::create_srv(device.Get(), source_tex.Get())
                                    : nullptr;
            if (!source_tex || !source_srv) {
                std::cerr << "file source texture creation failed\n";
                return 1;
            }
            std::cout << "[capture] file: " << file_source << "  " << source_w << "x"
                      << source_h << "\n";
        } else {
            HWND target_window = nullptr;
            HMONITOR target_monitor = nullptr;
            const std::string window_title = get(args, "window", "");
            if (!window_title.empty()) {
                const std::wstring needle = utf8_to_wide(window_title);
                std::wstring found_title;
                if (!obs::find_window_by_title(needle, &target_window, &found_title)) {
                    std::cerr << "window not found: " << window_title
                              << "\nUse --monitor 0 or a more specific title.\n";
                    return 1;
                }
                std::wcout << L"[capture] window: " << found_title << L"\n";
            } else {
                const int monitor_index = get_int(args, "monitor", 0);
                std::wstring monitor_name;
                if (!obs::monitor_by_index(monitor_index, &target_monitor, &monitor_name)) {
                    std::cerr << "monitor not found: " << monitor_index << "\n";
                    return 1;
                }
                std::wcout << L"[capture] monitor: " << monitor_name << L"\n";
            }

            if (!capture.start(device.Get(), context.Get(), target_window, target_monitor,
                               &error)) {
                std::cerr << "capture start failed: " << error << "\n";
                return 1;
            }
            const auto wait_start = Clock::now();
            while (!capture.has_frame() &&
                   Clock::now() - wait_start < std::chrono::seconds(5)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (!capture.has_frame()) {
                std::cerr << "no capture frame received\n";
                capture.stop();
                return 1;
            }
        }

        // Spout sender.
        const std::string spout_name = get(args, "spout", "OBScure");
        spoutDX sender;
        if (!sender.OpenDirectX11(device.Get())) {
            std::cerr << "Spout OpenDirectX11 failed\n";
            capture.stop();
            return 1;
        }
        sender.SetSenderFormat(DXGI_FORMAT_B8G8R8A8_UNORM);
        sender.SetSenderName(spout_name.c_str());

        const double seconds = get_double(args, "seconds", 20.0);
        const double fps = std::max(1.0, get_double(args, "fps", 60.0));
        const auto frame_duration = std::chrono::duration<double>(1.0 / fps);
        auto loop_start = Clock::now();
        uint64_t frame_index = 0;
        uint64_t last_frame_index = 0;
        uint64_t last_capture = 0;
        auto last_stats = loop_start;

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(layout.canvas_w);
        viewport.Height = static_cast<float>(layout.canvas_h);
        viewport.MaxDepth = 1.0f;

        std::cout << "[live] config=" << config << " grid=" << layout.cols() << "x"
                  << layout.rows() << " ref=" << layout.ref_w() << "x" << layout.ref_h()
                  << " key=" << std::hex << std::uppercase << key.key_id << std::dec
                  << " spout=" << spout_name << " fps=" << fps << "\n";

        while (g_running.load()) {
            const auto now = Clock::now();
            if (seconds > 0.0 &&
                std::chrono::duration<double>(now - loop_start).count() >= seconds) {
                break;
            }

            if (file_mode) {
                cv::Mat frame;
                if (!file_cap.read(frame) || frame.empty()) {
                    file_cap.set(cv::CAP_PROP_POS_FRAMES, 0);
                    file_cap.read(frame);
                }
                if (!frame.empty()) {
                    if (frame.cols != source_w || frame.rows != source_h) {
                        source_w = frame.cols;
                        source_h = frame.rows;
                        source_tex = obs::create_texture(
                            device.Get(), source_w, source_h, source_format,
                            D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
                        source_srv = source_tex
                                         ? obs::create_srv(device.Get(), source_tex.Get())
                                         : nullptr;
                    }
                    if (source_tex) {
                        cv::Mat bgra;
                        cv::cvtColor(frame, bgra, cv::COLOR_BGR2BGRA);
                        if (!bgra.isContinuous()) bgra = bgra.clone();
                        context->UpdateSubresource(source_tex.Get(), 0, nullptr,
                                                   bgra.data, source_w * 4, 0);
                    }
                }
            } else {
                std::lock_guard lock(capture.mutex());
                const int cw = capture.width();
                const int ch = capture.height();
                const DXGI_FORMAT cf = capture.format();
                if (cw > 0 && ch > 0) {
                    if (!source_tex || source_w != cw || source_h != ch ||
                        source_format != cf) {
                        source_tex = obs::create_texture(
                            device.Get(), cw, ch, cf, D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
                        source_srv = source_tex
                                         ? obs::create_srv(device.Get(), source_tex.Get())
                                         : nullptr;
                        source_w = cw;
                        source_h = ch;
                        source_format = cf;
                        if (cf != DXGI_FORMAT_B8G8R8A8_UNORM && !warned_format) {
                            std::cout << "[warn] capture format is " << static_cast<int>(cf)
                                      << " (expected B8G8R8A8_UNORM)\n";
                            warned_format = true;
                        }
                    }
                    if (source_tex) {
                        capture.copy_latest(source_tex.Get());
                    }
                }
            }

            ID3D11ShaderResourceView* srvs[4] = {
                lut_srv.Get(), table_srv.Get(),
                source_srv ? source_srv.Get() : fallback_srv.Get(), border_srv.Get()};
            ID3D11Buffer* cbs[1] = {cb.Get()};
            ID3D11SamplerState* samplers[1] = {sampler.Get()};

            context->OMSetRenderTargets(1, output_rtv.GetAddressOf(), nullptr);
            context->RSSetViewports(1, &viewport);
            context->RSSetState(rasterizer.Get());
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->PSSetShader(ps.Get(), nullptr, 0);
            context->PSSetConstantBuffers(0, 1, cbs);
            context->PSSetShaderResources(0, 4, srvs);
            context->PSSetSamplers(0, 1, samplers);
            context->Draw(3, 0);
            ID3D11ShaderResourceView* null_srvs[4] = {nullptr, nullptr, nullptr, nullptr};
            context->PSSetShaderResources(0, 4, null_srvs);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            context->Flush();

            sender.SendTexture(output_tex.Get());

            ++frame_index;
            const auto stat_now = Clock::now();
            if (std::chrono::duration<double>(stat_now - last_stats).count() >= 1.0) {
                const double sec = std::chrono::duration<double>(stat_now - last_stats).count();
                const uint64_t cap = capture.frame_count();
                std::cout << "[live] frames=" << frame_index
                          << " render_fps=" << (frame_index - last_frame_index) / sec
                          << " capture_fps=" << (cap - last_capture) / sec
                          << " spout=" << (sender.IsInitialized() ? 1 : 0)
                          << " spout_fps=" << sender.GetFps() << "\n";
                last_frame_index = frame_index;
                last_capture = cap;
                last_stats = stat_now;
            }

            const auto next = loop_start + std::chrono::duration_cast<Clock::duration>(
                                               frame_duration * static_cast<double>(frame_index));
            const auto after = Clock::now();
            if (next > after) {
                std::this_thread::sleep_until(next);
            } else if (after - next > std::chrono::milliseconds(200)) {
                loop_start = after;
            }
        }

        capture.stop();
        sender.ReleaseSender();
        std::cout << "[live] stopped after " << frame_index << " frames, captured "
                  << capture.frame_count() << " source frames\n";
        winrt::uninit_apartment();
        return 0;
    } catch (const winrt::hresult_error& e) {
        std::cerr << "winrt error: " << winrt::to_string(e.message()) << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
