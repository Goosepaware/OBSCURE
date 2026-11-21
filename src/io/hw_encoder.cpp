#include "io/hw_encoder.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace obs {

namespace {

std::wstring quote_arg(const std::wstring& text) {
    if (text.find_first_of(L" \t") == std::wstring::npos) return text;
    std::wstring out = L"\"";
    for (wchar_t c : text) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L"\"";
    return out;
}

std::string lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::wstring widen(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

// Runs a command with hidden console, optional stdin payload, and captures
// stdout+stderr into one string. Returns true when the process exits with 0.
bool run_capture(const std::wstring& command, const void* stdin_data,
                 size_t stdin_size, DWORD timeout_ms, std::string* output) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE in_read = nullptr;
    HANDLE in_write = nullptr;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, &sa, 0)) return false;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
        CloseHandle(in_read);
        CloseHandle(in_write);
        return false;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = out_write;

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = command;
    const BOOL created =
        CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (!created) {
        CloseHandle(in_write);
        CloseHandle(out_read);
        return false;
    }
    CloseHandle(pi.hThread);

    std::thread feeder;
    if (stdin_data && stdin_size > 0) {
        feeder = std::thread([in_write, stdin_data, stdin_size] {
            const char* p = static_cast<const char*>(stdin_data);
            size_t left = stdin_size;
            while (left > 0) {
                const DWORD chunk =
                    static_cast<DWORD>(std::min<size_t>(left, 1u << 16));
                DWORD written = 0;
                if (!WriteFile(in_write, p, chunk, &written, nullptr) || written == 0) {
                    break;
                }
                p += written;
                left -= written;
            }
            CloseHandle(in_write);
        });
    } else {
        CloseHandle(in_write);
    }

    std::string data;
    const ULONGLONG start = GetTickCount64();
    bool timed_out = false;
    for (;;) {
        DWORD available = 0;
        if (PeekNamedPipe(out_read, nullptr, 0, nullptr, &available, nullptr) &&
            available > 0) {
            char buffer[4096];
            DWORD got = 0;
            const DWORD want = std::min<DWORD>(available, sizeof(buffer));
            if (ReadFile(out_read, buffer, want, &got, nullptr) && got > 0) {
                data.append(buffer, got);
            }
        }
        if (WaitForSingleObject(pi.hProcess, 15) == WAIT_OBJECT_0) {
            for (;;) {
                DWORD rest = 0;
                if (!PeekNamedPipe(out_read, nullptr, 0, nullptr, &rest, nullptr) ||
                    rest == 0) {
                    break;
                }
                char buffer[4096];
                DWORD got = 0;
                if (!ReadFile(out_read, buffer, std::min<DWORD>(rest, sizeof(buffer)),
                              &got, nullptr) ||
                    got == 0) {
                    break;
                }
                data.append(buffer, got);
            }
            break;
        }
        if (GetTickCount64() - start > timeout_ms) {
            timed_out = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }
    }

    if (feeder.joinable()) feeder.join();
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(out_read);
    if (output) *output = std::move(data);
    return !timed_out && exit_code == 0;
}

// A tiny three-frame BGRA clip is enough to prove that the encoder can open
// the GPU and produce yuv420p output.
bool test_encoder(const std::wstring& ffmpeg, const std::string& name) {
    constexpr int w = 160;
    constexpr int h = 90;
    std::vector<uint8_t> frame(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            frame[i + 0] = static_cast<uint8_t>(x * 255 / (w - 1));
            frame[i + 1] = static_cast<uint8_t>(y * 255 / (h - 1));
            frame[i + 2] = 96;
            frame[i + 3] = 255;
        }
    }
    std::vector<uint8_t> clip;
    clip.reserve(frame.size() * 3);
    for (int i = 0; i < 3; ++i) {
        clip.insert(clip.end(), frame.begin(), frame.end());
    }

    std::wostringstream cmd;
    cmd << quote_arg(ffmpeg)
        << L" -hide_banner -loglevel error -f rawvideo -pixel_format bgra"
        << L" -video_size " << w << L"x" << h << L" -framerate 30 -i -"
        << L" -frames:v 3 -an -c:v " << widen(name)
        << L" -pix_fmt yuv420p -f null -";
    std::string ignored;
    return run_capture(cmd.str(), clip.data(), clip.size(), 8000, &ignored);
}

std::mutex g_probe_mutex;
std::map<std::wstring, HwEncoderStatus> g_probe_cache;

std::string hardware_name(VideoCodec codec, HwAccel accel) {
    const char* family = "";
    switch (accel) {
        case HwAccel::Nvidia: family = "nvenc"; break;
        case HwAccel::Intel: family = "qsv"; break;
        case HwAccel::Amd: family = "amf"; break;
        default: return {};
    }
    switch (codec) {
        case VideoCodec::H265:
            return std::string("hevc_") + family;
        case VideoCodec::Av1:
            return std::string("av1_") + family;
        default:
            return std::string("h264_") + family;
    }
}

bool usable(const HwEncoderStatus& status, VideoCodec codec, HwAccel accel) {
    switch (accel) {
        case HwAccel::Nvidia:
            return status.nvenc && (codec != VideoCodec::Av1 || status.av1_nvenc);
        case HwAccel::Intel:
            return status.qsv && (codec != VideoCodec::Av1 || status.av1_qsv);
        case HwAccel::Amd:
            return status.amf && (codec != VideoCodec::Av1 || status.av1_amf);
        default:
            return false;
    }
}

const char* software_encoder(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::H265: return "libx265";
        case VideoCodec::Av1: return "libsvtav1";
        default: return "libx264";
    }
}

}  // namespace

const char* hw_accel_id(HwAccel accel) {
    switch (accel) {
        case HwAccel::Nvidia: return "nvenc";
        case HwAccel::Intel: return "qsv";
        case HwAccel::Amd: return "amf";
        case HwAccel::Cpu: return "cpu";
        default: return "auto";
    }
}

HwAccel hw_accel_from_id(const std::string& id) {
    if (id == "nvenc" || id == "nvidia") return HwAccel::Nvidia;
    if (id == "qsv" || id == "intel") return HwAccel::Intel;
    if (id == "amf" || id == "amd") return HwAccel::Amd;
    if (id == "cpu" || id == "software" || id == "x264") return HwAccel::Cpu;
    return HwAccel::Auto;
}

const char* hw_accel_label(HwAccel accel) {
    switch (accel) {
        case HwAccel::Nvidia: return "NVIDIA NVENC";
        case HwAccel::Intel: return "Intel Quick Sync";
        case HwAccel::Amd: return "AMD AMF";
        case HwAccel::Cpu: return "CPU 软编（兼容性最好）";
        default: return "自动（优先硬件）";
    }
}

HwEncoderStatus probe_hw_encoders(const std::wstring& ffmpeg, bool force) {
    std::lock_guard lock(g_probe_mutex);
    const auto it = g_probe_cache.find(ffmpeg);
    if (it != g_probe_cache.end() && !force) return it->second;

    HwEncoderStatus status;
    status.probed = true;

    std::string listing;
    run_capture(quote_arg(ffmpeg) + L" -hide_banner -encoders", nullptr, 0, 8000,
                &listing);
    const std::string lowered = lower_ascii(listing);
    auto listed = [&](const char* name) {
        return lowered.find(name) != std::string::npos;
    };

    status.nvenc = listed("h264_nvenc") && test_encoder(ffmpeg, "h264_nvenc");
    status.qsv = listed("h264_qsv") && test_encoder(ffmpeg, "h264_qsv");
    status.amf = listed("h264_amf") && test_encoder(ffmpeg, "h264_amf");
    status.av1_nvenc = status.nvenc && listed("av1_nvenc");
    status.av1_qsv = status.qsv && listed("av1_qsv");
    status.av1_amf = status.amf && listed("av1_amf");

    std::ostringstream detail;
    detail << "nvenc=" << (status.nvenc ? "ok" : "no")
           << " qsv=" << (status.qsv ? "ok" : "no")
           << " amf=" << (status.amf ? "ok" : "no");
    status.detail = detail.str();

    g_probe_cache[ffmpeg] = status;
    return status;
}

std::string select_video_encoder(VideoCodec codec, HwAccel preference,
                                 const std::wstring& ffmpeg) {
    const HwEncoderStatus status = probe_hw_encoders(ffmpeg);
    if (preference == HwAccel::Cpu) return software_encoder(codec);
    if (preference != HwAccel::Auto) {
        return usable(status, codec, preference) ? hardware_name(codec, preference)
                                                 : software_encoder(codec);
    }
    for (const HwAccel candidate :
         {HwAccel::Nvidia, HwAccel::Intel, HwAccel::Amd}) {
        if (usable(status, codec, candidate)) return hardware_name(codec, candidate);
    }
    return software_encoder(codec);
}

std::string video_encoder_args(const std::string& encoder, double bitrate_mbps,
                               int gop, bool mp4_like) {
    const double br = bitrate_mbps > 0 ? bitrate_mbps : 8.0;
    const int keyint = gop > 0 ? gop : 60;
    std::ostringstream cmd;
    if (encoder == "libx264") {
        cmd << " -c:v libx264 -preset veryfast"
            << " -b:v " << br << "M -minrate " << br << "M -maxrate " << br
            << "M -bufsize " << (br * 2) << "M -x264-params nal-hrd=cbr"
            << " -keyint_min " << keyint << " -sc_threshold 0";
    } else if (encoder == "libx265") {
        cmd << " -c:v libx265 -preset medium"
            << " -b:v " << br << "M -maxrate " << br << "M -bufsize " << (br * 2)
            << "M"
            << " -x265-params keyint=" << keyint << ":min-keyint=" << keyint
            << ":scenecut=0:log-level=error";
        if (mp4_like) cmd << " -tag:v hvc1";
    } else if (encoder == "libsvtav1") {
        cmd << " -c:v libsvtav1 -preset 8 -b:v " << br << "M";
        if (mp4_like) cmd << " -tag:v av01";
    } else if (encoder.find("_nvenc") != std::string::npos) {
        cmd << " -c:v " << encoder << " -preset p4 -rc cbr -b:v " << br
            << "M -maxrate " << br << "M -bufsize " << (br * 2) << "M"
            << " -g " << keyint;
    } else if (encoder.find("_qsv") != std::string::npos) {
        cmd << " -c:v " << encoder << " -preset veryfast -b:v " << br
            << "M -maxrate " << br << "M -bufsize " << (br * 2) << "M"
            << " -g " << keyint;
    } else if (encoder.find("_amf") != std::string::npos) {
        cmd << " -c:v " << encoder << " -quality balanced -rc cbr -b:v " << br
            << "M -maxrate " << br << "M -bufsize " << (br * 2) << "M"
            << " -g " << keyint;
    } else {
        // Unknown name: pass it through with conservative CBR settings.
        cmd << " -c:v " << encoder << " -b:v " << br << "M -maxrate " << br
            << "M -bufsize " << (br * 2) << "M -g " << keyint;
    }
    return cmd.str();
}

bool is_hardware_encoder(const std::string& encoder) {
    return encoder.find("_nvenc") != std::string::npos ||
           encoder.find("_qsv") != std::string::npos ||
           encoder.find("_amf") != std::string::npos;
}

}  // namespace obs
