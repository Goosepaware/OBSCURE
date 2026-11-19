#pragma once

#include <string>

#include "core/audio_scramble.h"

namespace obs {

// Wide-path variants are used by the GUI so Chinese/space paths work with
// ffmpeg.exe through CreateProcessW.
bool audio_extract_wav(const std::wstring& ffmpeg, const std::wstring& input,
                       const std::wstring& wav, std::string* error);

// Extract the input audio to a temporary WAV, run it through the scrambler, and
// write the processed float WAV.
bool audio_process_to_wav(const std::wstring& ffmpeg, const std::wstring& input,
                          const std::wstring& wav,
                          const AudioScrambleParams& params, int sample_rate,
                          std::string* error, bool scramble = true);

// Mux a video file and a WAV into the output, copying the video stream and
// encoding the audio as AAC.
bool audio_mux(const std::wstring& ffmpeg, const std::wstring& video,
               const std::wstring& wav, const std::wstring& output,
               int bitrate_kbps, std::string* error);

}  // namespace obs
