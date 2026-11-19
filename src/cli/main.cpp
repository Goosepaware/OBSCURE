#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "core/border.h"
#include "core/detect.h"
#include "core/key.h"
#include "core/layout.h"
#include "core/metadata.h"
#include "core/presets.h"
#include "core/transform.h"
#include "io/ffmpeg_writer.h"

namespace fs = std::filesystem;

namespace {

void usage() {
    std::cout
        << "OBScure CLI (phase 1)\n"
        << "  obscure-cli selftest\n"
        << "  obscure-cli encode --input in.mp4 --output out.mp4 [options]\n"
        << "  obscure-cli decode --input out.mp4 --output restored.mp4 [options]\n\n"
        << "Options:\n"
        << "  --config NAME        c32_g2_s120 (default), c32_g2_s100, c32_g2_s140,\n"
        << "                       c16_g2_s100, c16_g1_s100, c64_g2_s100, c64_g2_s120,\n"
        << "                       h8_g0_s100, h16_g0_s100, h32_g0_s100\n"
        << "  --algo grid|hilbert  scrambling algorithm (hilbert = spatial curve)\n"
        << "  --bitrate N          encode CBR Mbps (default 8)\n"
        << "  --key TEXT           shared key (default obscure-demo-key)\n"
        << "  --public             use the built-in public key (no secret needed)\n"
        << "  --no-color           disable color scrambling (keep original colors)\n"
        << "  --frames N           process at most N frames (0 = all)\n"
        << "  --start SEC          seek before reading\n"
        << "  --ffmpeg PATH        ffmpeg executable\n"
        << "  --margin auto|0|1|2  decoder guard-band inset (default auto)\n"
        << "  --no-detect          decode assuming the exact canonical frame\n"
        << "  --dump-every N       write transport/restored PNGs every N frames\n"
        << "  --dump-dir PATH      dump directory\n";
}

std::map<std::string, std::string> parse_args(int argc, char** argv) {
    std::map<std::string, std::string> out;
    for (int i = 2; i < argc; ++i) {
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

std::string default_ffmpeg() {
    const fs::path p = "D:/program/OBScure/ffmpeg.exe";
    if (fs::exists(p)) return p.string();
    return "ffmpeg";
}

void ensure_parent(const std::string& path) {
    if (path.empty()) return;
    const fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }
}

int run_selftest() {
    try {
        obs::Layout layout;
        layout.cell = 32;
        layout.guard = 2;
        layout.key = "obscure-selftest";
        layout.validate();
        const obs::KeyMaterial key = obs::derive_key(layout);
        obs::TransportOptions options;
        options.use_color = true;
        options.use_dihedral = true;
        options.preset_id = 2;
        options.scale_x10 = 12;
        const cv::Mat border = obs::build_border(layout, key, options);

        cv::Mat source(layout.canvas_h, layout.canvas_w, CV_8UC3);
        cv::randu(source, cv::Scalar(0, 0, 0), cv::Scalar(255, 255, 255));
        cv::putText(source, "OBSCure selftest 1234567890", cv::Point(400, 500),
                    cv::FONT_HERSHEY_SIMPLEX, 2.0, cv::Scalar(0, 0, 0), 4,
                    cv::LINE_AA);
        cv::rectangle(source, cv::Rect(200, 700, 500, 200), cv::Scalar(0, 200, 255),
                      cv::FILLED);

        const cv::Mat content = obs::obfuscate_content(source, layout, key, 1.0, true, true);
        const cv::Mat transport = obs::compose_transport(content, border, layout);
        const cv::Mat reference = obs::make_reference(source, layout, 1.0);
        const cv::Mat restored = obs::deobfuscate_content(
            obs::crop_content(transport, layout), layout, key, true, true, 0);

        const double max_diff = cv::norm(restored, reference, cv::NORM_INF);
        const auto det = obs::detect_homography(transport);
        const auto meta = obs::decode_metadata(transport, det.H, layout);

        bool dihedral_ok = true;
        bool color_ok = true;
        cv::Mat patch(28, 28, CV_8UC3);
        cv::randu(patch, cv::Scalar(0, 0, 0), cv::Scalar(255, 255, 255));
        for (int code = 0; code < 8; ++code) {
            cv::Mat t;
            obs::apply_dihedral(patch, t, code);
            cv::Mat back;
            obs::apply_dihedral(t, back, obs::inverse_dihedral_code(code));
            const double d = cv::norm(back, patch, cv::NORM_INF);
            if (d != 0.0) {
                dihedral_ok = false;
                std::cout << "  dihedral fail code=" << code << " diff=" << d << "\n";
            }

            cv::Mat c = patch.clone();
            obs::apply_color(c, code);
            obs::apply_color(c, code);
            const double cd = cv::norm(c, patch, cv::NORM_INF);
            if (cd != 0.0) {
                color_ok = false;
                std::cout << "  color fail code=" << code << " diff=" << cd << "\n";
            }
        }

        const bool markers_ok = det.marker_count == 4 && !det.H.empty();
        const bool meta_ok = meta.has_value() && meta->key_id == key.key_id &&
                             !meta->public_mode() && meta->use_color() &&
                             meta->preset_id == 2 && meta->scale_x10 == 12;
        const bool exact = max_diff == 0.0;

        // Public + color-disabled round trip and metadata flags.
        obs::Layout public_layout = layout;
        public_layout.key = obs::kPublicKey;
        const obs::KeyMaterial public_key = obs::derive_key(public_layout);
        obs::TransportOptions public_options;
        public_options.use_color = false;
        public_options.use_dihedral = true;
        public_options.public_mode = true;
        public_options.preset_id = 6;
        public_options.scale_x10 = 10;
        const cv::Mat public_border =
            obs::build_border(public_layout, public_key, public_options);
        const cv::Mat public_content = obs::obfuscate_content(
            source, public_layout, public_key, 1.0, false, true);
        const cv::Mat public_transport =
            obs::compose_transport(public_content, public_border, public_layout);
        const cv::Mat public_reference =
            obs::make_reference(source, public_layout, 1.0);
        const cv::Mat public_restored = obs::deobfuscate_content(
            obs::crop_content(public_transport, public_layout), public_layout,
            public_key, false, true, 0);
        const double public_diff =
            cv::norm(public_restored, public_reference, cv::NORM_INF);
        const auto public_det = obs::detect_homography(public_transport);
        const auto public_meta =
            obs::decode_metadata(public_transport, public_det.H, public_layout);
        const bool public_ok =
            public_diff == 0.0 && public_meta.has_value() &&
            public_meta->public_mode() && !public_meta->use_color() &&
            public_meta->preset_id == 6 && public_meta->scale_x10 == 10 &&
            public_meta->key_id == public_key.key_id;

        // Spatial-curve algorithm: full-frame, guard-free, upright tiles. The
        // curve must be a true permutation and the CPU round trip must be exact.
        obs::Layout hilbert_layout = layout;
        hilbert_layout.cell = 16;
        hilbert_layout.guard = 0;
        hilbert_layout.algorithm = static_cast<int>(obs::ScrambleAlgorithm::Hilbert);
        hilbert_layout.validate();
        const obs::KeyMaterial hilbert_key = obs::derive_key(hilbert_layout);
        std::vector<int> seen(static_cast<size_t>(hilbert_layout.blocks()), 0);
        bool perm_ok = true;
        for (int32_t v : hilbert_key.perm) {
            if (v < 0 || v >= hilbert_layout.blocks() ||
                seen[static_cast<size_t>(v)] != 0) {
                perm_ok = false;
                break;
            }
            seen[static_cast<size_t>(v)] = 1;
        }
        const std::vector<int32_t> curve =
            obs::hilbert_curve_order(hilbert_layout.cols(), hilbert_layout.rows());
        std::fill(seen.begin(), seen.end(), 0);
        bool curve_ok = static_cast<int>(curve.size()) == hilbert_layout.blocks();
        for (int32_t v : curve) {
            if (!curve_ok || v < 0 || v >= hilbert_layout.blocks() ||
                seen[static_cast<size_t>(v)] != 0) {
                curve_ok = false;
                break;
            }
            seen[static_cast<size_t>(v)] = 1;
        }

        obs::TransportOptions hilbert_options;
        hilbert_options.use_color = false;
        hilbert_options.use_dihedral = false;
        hilbert_options.preset_id = obs::preset_index_by_id("h16_g0_s100") + 1;
        hilbert_options.scale_x10 = 10;
        hilbert_options.algorithm = static_cast<int>(obs::ScrambleAlgorithm::Hilbert);
        const cv::Mat hilbert_border =
            obs::build_border(hilbert_layout, hilbert_key, hilbert_options);
        const cv::Mat hilbert_content = obs::obfuscate_content(
            source, hilbert_layout, hilbert_key, 1.0, false, false);
        const cv::Mat hilbert_transport =
            obs::compose_transport(hilbert_content, hilbert_border, hilbert_layout);
        const cv::Mat hilbert_reference =
            obs::make_reference(source, hilbert_layout, 1.0);
        const cv::Mat hilbert_restored = obs::deobfuscate_content(
            obs::crop_content(hilbert_transport, hilbert_layout), hilbert_layout,
            hilbert_key, false, false, 0);
        const double hilbert_diff =
            cv::norm(hilbert_restored, hilbert_reference, cv::NORM_INF);
        const auto hilbert_det = obs::detect_homography(hilbert_transport);
        const auto hilbert_meta =
            obs::decode_metadata(hilbert_transport, hilbert_det.H, hilbert_layout);
        const bool hilbert_ok =
            perm_ok && curve_ok && hilbert_diff == 0.0 && hilbert_meta.has_value() &&
            hilbert_meta->algorithm() ==
                static_cast<int>(obs::ScrambleAlgorithm::Hilbert) &&
            !hilbert_meta->use_color() && !hilbert_meta->use_dihedral() &&
            hilbert_meta->preset_id == hilbert_options.preset_id;

        // Verify every spatial-curve preset produces a true permutation.
        bool curves_ok = true;
        for (const char* id : {"h8_g0_s100", "h16_g0_s100", "h32_g0_s100"}) {
            const int pi = obs::preset_index_by_id(id);
            if (pi < 0) {
                curves_ok = false;
                continue;
            }
            const auto& p = obs::kPresets[pi];
            obs::Layout curve_layout = layout;
            curve_layout.cell = p.cell;
            curve_layout.guard = p.guard;
            curve_layout.algorithm = p.algorithm;
            const auto order =
                obs::hilbert_curve_order(curve_layout.cols(), curve_layout.rows());
            bool ok = static_cast<int>(order.size()) == curve_layout.blocks();
            std::vector<int> used(static_cast<size_t>(curve_layout.blocks()), 0);
            for (int32_t v : order) {
                if (!ok || v < 0 || v >= curve_layout.blocks() ||
                    used[static_cast<size_t>(v)] != 0) {
                    ok = false;
                    break;
                }
                used[static_cast<size_t>(v)] = 1;
            }
            const obs::KeyMaterial pkey = obs::derive_key(curve_layout);
            std::fill(used.begin(), used.end(), 0);
            for (int32_t v : pkey.perm) {
                if (!ok || v < 0 || v >= curve_layout.blocks() ||
                    used[static_cast<size_t>(v)] != 0) {
                    ok = false;
                    break;
                }
                used[static_cast<size_t>(v)] = 1;
            }
            if (!ok) {
                curves_ok = false;
                std::cout << "  curve fail preset=" << id << "\n";
            }
        }

        // Public mode must also work with the spatial-curve algorithm.
        bool public_curve_ok = false;
        {
            obs::Layout pub_layout = hilbert_layout;
            pub_layout.key = obs::kPublicKey;
            const obs::KeyMaterial pub_key = obs::derive_key(pub_layout);
            obs::TransportOptions pub_options = hilbert_options;
            pub_options.public_mode = true;
            const cv::Mat pub_border =
                obs::build_border(pub_layout, pub_key, pub_options);
            const cv::Mat pub_content = obs::obfuscate_content(
                source, pub_layout, pub_key, 1.0, false, false);
            const cv::Mat pub_transport =
                obs::compose_transport(pub_content, pub_border, pub_layout);
            const cv::Mat pub_reference =
                obs::make_reference(source, pub_layout, 1.0);
            const cv::Mat pub_restored = obs::deobfuscate_content(
                obs::crop_content(pub_transport, pub_layout), pub_layout, pub_key,
                false, false, 0);
            const auto pub_det = obs::detect_homography(pub_transport);
            const auto pub_meta =
                obs::decode_metadata(pub_transport, pub_det.H, pub_layout);
            public_curve_ok =
                cv::norm(pub_restored, pub_reference, cv::NORM_INF) == 0.0 &&
                pub_meta.has_value() && pub_meta->public_mode() &&
                pub_meta->algorithm() ==
                    static_cast<int>(obs::ScrambleAlgorithm::Hilbert) &&
                pub_meta->key_id == obs::public_key_id() &&
                pub_key.key_id == obs::public_key_id();
        }

        std::cout << "selftest: exact=" << exact << " maxdiff=" << max_diff
                  << " markers=" << det.marker_count << " h_err=" << det.reproj_error
                  << " metadata=" << meta_ok << " public=" << public_ok
                  << " dihedral=" << dihedral_ok << " color=" << color_ok
                  << " hilbert=" << hilbert_ok << " curves=" << curves_ok
                  << " public_curve=" << public_curve_ok << "\n";
        const bool ok = exact && markers_ok && meta_ok && public_ok && dihedral_ok &&
                        color_ok && hilbert_ok && curves_ok && public_curve_ok;
        std::cout << (ok ? "SELFTEST OK" : "SELFTEST FAILED") << "\n";
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "selftest error: " << e.what() << "\n";
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 0;
    }
    const std::string mode = argv[1];
    if (mode == "-h" || mode == "--help" || mode == "help") {
        usage();
        return 0;
    }
    if (mode == "selftest") {
        return run_selftest();
    }
    if (mode != "encode" && mode != "decode") {
        usage();
        return 2;
    }

    const auto args = parse_args(argc, argv);
    const std::string input = get(args, "input", "");
    const std::string output = get(args, "output", "");
    const std::string algo_arg = get(args, "algo", "");
    const std::string config =
        get(args, "config", algo_arg == "hilbert" || args.count("hilbert")
                                ? "h16_g0_s100"
                                : "c32_g2_s120");
    const bool public_key = args.find("public") != args.end();
    const bool use_color_arg = args.find("no-color") == args.end();
    const std::string key_text =
        public_key ? std::string(obs::kPublicKey) : get(args, "key", "obscure-demo-key");
    const std::string ffmpeg = get(args, "ffmpeg", default_ffmpeg());
    const int max_frames = get_int(args, "frames", 0);
    const double start_sec = get_double(args, "start", 0.0);
    const double bitrate = get_double(args, "bitrate", 8.0);
    const int dump_every = get_int(args, "dump-every", 0);
    const std::string dump_dir = get(args, "dump-dir", "");

    if (input.empty()) {
        std::cerr << "missing --input\n";
        return 2;
    }
    if (mode == "encode" && output.empty()) {
        std::cerr << "missing --output\n";
        return 2;
    }

    try {
        const int preset_index = obs::preset_index_by_id(config);
        if (preset_index < 0) {
            throw std::runtime_error("unknown config: " + config);
        }
        const obs::Preset& preset = obs::kPresets[preset_index];
        const bool hilbert = preset.algorithm != 0;
        const bool use_color = hilbert ? false : use_color_arg;
        const bool use_dihedral = !hilbert;

        obs::Layout layout;
        layout.cell = preset.cell;
        layout.guard = preset.guard;
        layout.algorithm = preset.algorithm;
        layout.key = key_text;
        layout.validate();
        const obs::KeyMaterial key = obs::derive_key(layout);
        obs::TransportOptions options;
        options.use_color = use_color;
        options.use_dihedral = use_dihedral;
        options.public_mode = public_key;
        options.preset_id = preset_index + 1;
        options.scale_x10 = obs::preset_scale_x10(preset);
        options.algorithm = preset.algorithm;
        const cv::Mat border = obs::build_border(layout, key, options);

        std::cout << "[config] " << config << " cell=" << layout.cell
                  << " guard=" << layout.guard << " scale=" << preset.scale
                  << " algo=" << obs::scramble_algorithm_id(layout.algorithm)
                  << " grid=" << layout.cols() << "x" << layout.rows()
                  << " ref=" << layout.ref_w() << "x" << layout.ref_h()
                  << " key=" << std::hex << std::uppercase << key.key_id << std::dec
                  << " color=" << (use_color ? 1 : 0)
                  << " public=" << (public_key ? 1 : 0) << "\n";

        cv::VideoCapture cap;
        bool opened = cap.open(input, cv::CAP_MSMF);
        if (!opened) opened = cap.open(input, cv::CAP_ANY);
        if (!opened) {
            throw std::runtime_error("cannot open input: " + input);
        }
        if (start_sec > 0.0) {
            cap.set(cv::CAP_PROP_POS_MSEC, start_sec * 1000.0);
        }
        const int src_w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
        const int src_h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
        double fps = cap.get(cv::CAP_PROP_FPS);
        if (fps <= 0.0 || fps > 240.0) fps = 60.0;
        std::cout << "[input] " << input << "  " << src_w << "x" << src_h << " @ "
                  << fps << " fps\n";

        if (!output.empty()) ensure_parent(output);
        obs::FfmpegWriter writer;
        const bool write_output = !output.empty();
        if (write_output) {
            const int out_w = mode == "encode" ? layout.canvas_w : layout.ref_w();
            const int out_h = mode == "encode" ? layout.canvas_h : layout.ref_h();
            if (!writer.open(ffmpeg, output, out_w, out_h, fps, bitrate,
                             mode == "decode")) {
                throw std::runtime_error(writer.error());
            }
        }

        if (!dump_dir.empty()) fs::create_directories(dump_dir);

        int frames = 0;
        int detect_attempts = 0;
        int detect_ok = 0;
        int meta_attempts = 0;
        int meta_ok = 0;
        int margin_sum = 0;
        cv::Mat H;
        bool key_warned = false;
        bool meta_logged = false;
        const auto t0 = std::chrono::steady_clock::now();

        while (max_frames <= 0 || frames < max_frames) {
            cv::Mat frame;
            if (!cap.read(frame) || frame.empty()) break;

            if (mode == "encode") {
                cv::Mat content =
                    obs::obfuscate_content(frame, layout, key, preset.scale,
                                           use_color, use_dihedral);
                cv::Mat transport = obs::compose_transport(content, border, layout);
                if (write_output && !writer.write(transport)) {
                    throw std::runtime_error(writer.error());
                }
                if (dump_every > 0 && frames % dump_every == 0 && !dump_dir.empty()) {
                    cv::imwrite((fs::path(dump_dir) /
                                 ("transport_" + std::to_string(frames) + ".png"))
                                    .string(),
                                transport);
                }
            } else {
                const bool no_detect = args.find("no-detect") != args.end();
                if (!no_detect && frames % 30 == 0) {
                    const auto det = obs::detect_homography(frame);
                    ++detect_attempts;
                    if (!det.H.empty()) {
                        H = det.H;
                        ++detect_ok;
                    }
                    if (!H.empty() && frames % 60 == 0) {
                        const auto md = obs::decode_metadata(frame, H, layout);
                        ++meta_attempts;
                        if (md) {
                            if (!meta_logged) {
                                meta_logged = true;
                                std::cout << "[meta] key=" << std::hex
                                          << std::uppercase << md->key_id << std::dec
                                          << " public=" << (md->public_mode() ? 1 : 0)
                                          << " color=" << (md->use_color() ? 1 : 0)
                                          << " dihedral="
                                          << (md->use_dihedral() ? 1 : 0)
                                          << " algo=" << md->algorithm()
                                          << " preset=" << md->preset_id
                                          << " scale_x10=" << md->scale_x10
                                          << " cell=" << md->cell
                                          << " guard=" << md->guard << "\n";
                            }
                            if (md->key_id == key.key_id) {
                                ++meta_ok;
                            } else if (!key_warned) {
                                std::cerr << "[warn] metadata key id " << std::hex
                                          << md->key_id << " != " << key.key_id
                                          << std::dec << "\n";
                                key_warned = true;
                            }
                        }
                    }
                }

                int margin = 0;
                const std::string m = get(args, "margin", "auto");
                if (m == "auto") {
                    margin = obs::adaptive_margin(H, layout.cell);
                } else {
                    margin = std::stoi(m);
                }
                margin_sum += margin;

                cv::Mat canonical = obs::warp_to_canonical(frame, H, layout);
                cv::Mat restored = obs::deobfuscate_content(
                    obs::crop_content(canonical, layout), layout, key, use_color,
                    use_dihedral, margin);
                if (write_output && !writer.write(restored)) {
                    throw std::runtime_error(writer.error());
                }
                if (dump_every > 0 && frames % dump_every == 0 && !dump_dir.empty()) {
                    cv::imwrite((fs::path(dump_dir) /
                                 ("captured_" + std::to_string(frames) + ".png"))
                                    .string(),
                                frame);
                    cv::imwrite((fs::path(dump_dir) /
                                 ("restored_" + std::to_string(frames) + ".png"))
                                    .string(),
                                restored);
                }
            }

            ++frames;
            if (frames % 60 == 0) {
                std::cout << "  " << frames << " frames\n";
            }
        }

        cap.release();
        if (write_output && !writer.close()) {
            throw std::runtime_error(writer.error());
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        std::cout << "[" << mode << "] frames=" << frames << "  " << sec << "s  "
                  << (sec > 0 ? frames / sec : 0.0) << " fps\n";
        if (mode == "decode") {
            std::cout << "  markers=" << detect_ok << "/" << detect_attempts
                      << "  metadata=" << meta_ok << "/" << meta_attempts
                      << "  avg_margin="
                      << (frames > 0 ? static_cast<double>(margin_sum) / frames : 0.0)
                      << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
