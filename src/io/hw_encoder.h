#pragma once

#include <string>

#include "io/video_codec.h"

namespace obs {

// Hardware encoder preference. Auto probes NVIDIA -> Intel -> AMD once and
// falls back to the software encoder when none is usable.
enum class HwAccel {
    Auto = 0,
    Nvidia = 1,
    Intel = 2,
    Amd = 3,
    Cpu = 4,
};

const char* hw_accel_id(HwAccel accel);
HwAccel hw_accel_from_id(const std::string& id);
const char* hw_accel_label(HwAccel accel);

struct HwEncoderStatus {
    bool probed = false;
    bool nvenc = false;
    bool qsv = false;
    bool amf = false;
    bool av1_nvenc = false;
    bool av1_qsv = false;
    bool av1_amf = false;
    std::string detail;
};

// Probes the ffmpeg binary once per process. Each available family is verified
// with a three-frame null-muxer encode, so a listed-but-broken encoder (no GPU
// driver, unsupported codec) is not reported as usable.
HwEncoderStatus probe_hw_encoders(const std::wstring& ffmpeg, bool force = false);

// Picks the ffmpeg encoder name for the codec, honouring the preference and
// falling back to libx264 / libx265 / libsvtav1.
std::string select_video_encoder(VideoCodec codec, HwAccel preference,
                                 const std::wstring& ffmpeg);

// Builds the "-c:v ... <rate control>" fragment used by FfmpegSink and the CLI.
std::string video_encoder_args(const std::string& encoder, double bitrate_mbps,
                               int gop, bool mp4_like);

bool is_hardware_encoder(const std::string& encoder);

}  // namespace obs
