#include "io/ffmpeg_audio.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "platform/wav_reader.h"
#include "platform/wav_writer.h"

namespace obs {

namespace {

std::wstring quote_w(const std::wstring& s) {
    if (s.find(L' ') == std::wstring::npos &&
        s.find(L'\t') == std::wstring::npos &&
        s.find(L'"') == std::wstring::npos) {
        return s;
    }
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') out += L"\\\"";
        else out += c;
    }
    out += L'"';
    return out;
}

int run_command_w(const std::wstring& command) {
    std::vector<wchar_t> mutable_cmd(command.begin(), command.end());
    mutable_cmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exit_code);
}

}  // namespace

bool audio_extract_wav(const std::wstring& ffmpeg, const std::wstring& input,
                       const std::wstring& wav, std::string* error) {
    const std::wstring command =
        quote_w(ffmpeg) + L" -hide_banner -loglevel error -y -i " +
        quote_w(input) +
        L" -vn -map 0:a:0? -ac 2 -ar 48000 -c:a pcm_f32le " + quote_w(wav);
    if (run_command_w(command) != 0) {
        if (error) *error = "ffmpeg audio extraction failed";
        return false;
    }
    return true;
}

bool audio_process_to_wav(const std::wstring& ffmpeg, const std::wstring& input,
                          const std::wstring& wav,
                          const AudioScrambleParams& params, int sample_rate,
                          std::string* error, bool scramble) {
    const std::wstring temp = wav + L".src.wav";
    if (!audio_extract_wav(ffmpeg, input, temp, error)) return false;

    WavReader reader;
    if (!reader.open(temp, error)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    WavWriter writer;
    if (!writer.open(wav, sample_rate, 2, error)) {
        reader.close();
        DeleteFileW(temp.c_str());
        return false;
    }
    AudioScrambler scrambler;
    if (scramble && !scrambler.configure(sample_rate, params, error)) {
        reader.close();
        writer.close();
        DeleteFileW(temp.c_str());
        return false;
    }

    constexpr int kBlock = 1024;
    std::vector<float> in_buf(static_cast<size_t>(kBlock) * 2);
    std::vector<float> out_buf(static_cast<size_t>(kBlock) * 2);
    bool ok = true;
    for (;;) {
        const int got = reader.read(in_buf.data(), kBlock);
        if (got <= 0) break;
        if (scramble) {
            scrambler.process(in_buf.data(), out_buf.data(), got);
        } else {
            std::copy(in_buf.begin(), in_buf.begin() + static_cast<size_t>(got) * 2,
                      out_buf.begin());
        }
        if (!writer.write(out_buf.data(), got)) {
            ok = false;
            break;
        }
    }
    reader.close();
    if (!writer.close()) ok = false;
    DeleteFileW(temp.c_str());
    if (!ok) {
        if (error) *error = "audio processing failed";
        return false;
    }
    return true;
}

bool audio_mux(const std::wstring& ffmpeg, const std::wstring& video,
               const std::wstring& wav, const std::wstring& output,
               int bitrate_kbps, std::string* error) {
    const int kbps = bitrate_kbps > 0 ? bitrate_kbps : 192;
    const std::wstring command =
        quote_w(ffmpeg) + L" -hide_banner -loglevel error -y -i " +
        quote_w(video) + L" -i " + quote_w(wav) +
        L" -map 0:v:0 -map 1:a:0 -c:v copy -c:a aac -b:a " +
        std::to_wstring(kbps) + L"k -shortest " + quote_w(output);
    if (run_command_w(command) != 0) {
        if (error) *error = "ffmpeg audio mux failed";
        return false;
    }
    return true;
}

}  // namespace obs
