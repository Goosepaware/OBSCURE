#include "io/ffmpeg_sink.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <sstream>

#include "platform/win_util.h"

namespace obs {

namespace {

std::wstring find_ffmpeg() {
    namespace fs = std::filesystem;
    const fs::path candidates[] = {
        fs::path(exe_dir()) / L"ffmpeg.exe",
        fs::path(exe_dir()).parent_path() / L"ffmpeg.exe",
        fs::path(L"D:/program/OBScure/ffmpeg.exe"),
    };
    std::error_code ec;
    for (const auto& path : candidates) {
        if (fs::exists(path, ec)) return path.wstring();
    }
    return L"ffmpeg.exe";
}

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

std::string tail(const std::string& text, size_t max_len) {
    if (text.size() <= max_len) return text;
    return text.substr(text.size() - max_len);
}

}  // namespace

FfmpegSink::~FfmpegSink() {
    stop();
}

bool FfmpegSink::start(ID3D11Device* device, ID3D11DeviceContext* context, int width,
                       int height, double fps, double bitrate_mbps,
                       const std::wstring& output, std::string* error,
                       VideoCodec codec) {
    stop();
    if (!device || !context || width <= 0 || height <= 0) {
        if (error) *error = "ffmpeg sink: bad frame size";
        return false;
    }
    device_ = device;
    context_ = context;
    width_ = width;
    height_ = height;
    codec_ = codec;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging_.GetAddressOf()))) {
        if (error) *error = "ffmpeg sink: staging texture failed";
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
        if (error) *error = "ffmpeg sink: pipe creation failed";
        return false;
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    const double br = bitrate_mbps > 0 ? bitrate_mbps : 8.0;
    std::wstring ext = std::filesystem::path(output).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    const bool mp4_like = ext == L".mp4" || ext == L".mov";

    std::wostringstream cmd;
    cmd << quote_arg(find_ffmpeg())
        << L" -hide_banner -loglevel error -y"
        << L" -f rawvideo -pixel_format bgra -video_size " << width_ << L"x" << height_
        << L" -framerate " << fps << L" -i - -an";
    if (codec_ == VideoCodec::Av1) {
        cmd << L" -c:v libsvtav1 -preset 8"
            << L" -b:v " << br << L"M";
        if (mp4_like) cmd << L" -tag:v av01";
    } else if (codec_ == VideoCodec::H265) {
        cmd << L" -c:v libx265 -preset medium"
            << L" -b:v " << br << L"M -maxrate " << br << L"M -bufsize " << (br * 2)
            << L"M"
            << L" -x265-params keyint=60:min-keyint=60:scenecut=0:log-level=error";
        if (mp4_like) cmd << L" -tag:v hvc1";
    } else {
        cmd << L" -c:v libx264 -preset veryfast"
            << L" -b:v " << br << L"M -minrate " << br << L"M -maxrate " << br
            << L"M -bufsize " << (br * 2) << L"M -x264-params nal-hrd=cbr"
            << L" -keyint_min 60 -sc_threshold 0";
    }
    cmd << L" -pix_fmt yuv420p -g 60 " << quote_arg(output);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = nul;
    si.hStdError = stderr_write;
    PROCESS_INFORMATION pi{};
    std::wstring command = cmd.str();
    std::vector<wchar_t> mutable_cmd(command.begin(), command.end());
    mutable_cmd.push_back(L'\0');
    const BOOL created = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(stdin_read);
    CloseHandle(stderr_write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!created) {
        CloseHandle(stdin_write);
        CloseHandle(stderr_read);
        if (error) *error = "ffmpeg sink: CreateProcessW failed";
        return false;
    }
    CloseHandle(pi.hThread);

    process_ = pi.hProcess;
    stdin_write_ = stdin_write;
    stderr_read_ = stderr_read;
    stop_ = false;
    failed_ = false;
    error_.clear();
    stderr_text_.clear();
    writer_ = std::thread([this] { writer_loop(); });
    stderr_thread_ = std::thread([this] { stderr_loop(); });
    return true;
}

void FfmpegSink::write(ID3D11Texture2D* texture) {
    if (!process_ || !texture || !context_ || !staging_) return;
    context_->CopyResource(staging_.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return;
    }
    std::vector<uint8_t> frame(static_cast<size_t>(width_) * height_ * 4);
    for (int y = 0; y < height_; ++y) {
        std::memcpy(frame.data() + static_cast<size_t>(y) * width_ * 4,
                    static_cast<const uint8_t*>(mapped.pData) +
                        static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(width_) * 4);
    }
    context_->Unmap(staging_.Get(), 0);

    std::unique_lock lock(mutex_);
    space_cv_.wait(lock, [this] { return stop_ || failed_ || queue_.size() < 3; });
    if (stop_ || failed_) return;
    queue_.push_back(std::move(frame));
    cv_.notify_one();
}

void FfmpegSink::writer_loop() {
    for (;;) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || failed_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stop_ || failed_) return;
                continue;
            }
            frame = std::move(queue_.front());
            queue_.pop_front();
            space_cv_.notify_all();
        }
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(stdin_write_), frame.data(),
                       static_cast<DWORD>(frame.size()), &written, nullptr) ||
            written != frame.size()) {
            std::lock_guard lock(mutex_);
            failed_ = true;
            error_ = "ffmpeg sink: write failed";
            cv_.notify_all();
            space_cv_.notify_all();
            return;
        }
    }
}

void FfmpegSink::stderr_loop() {
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(static_cast<HANDLE>(stderr_read_), buffer, sizeof(buffer), &read,
                      nullptr) ||
            read == 0) {
            return;
        }
        std::lock_guard lock(mutex_);
        stderr_text_.append(buffer, read);
        if (stderr_text_.size() > 8192) {
            stderr_text_ = stderr_text_.substr(stderr_text_.size() - 4096);
        }
    }
}

void FfmpegSink::stop() {
    if (!process_) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    space_cv_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (stdin_write_) {
        CloseHandle(static_cast<HANDLE>(stdin_write_));
        stdin_write_ = nullptr;
    }
    const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(process_), 5000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(static_cast<HANDLE>(process_), 1);
        WaitForSingleObject(static_cast<HANDLE>(process_), 2000);
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &exit_code);
    if (stderr_thread_.joinable()) stderr_thread_.join();
    if (stderr_read_) {
        CloseHandle(static_cast<HANDLE>(stderr_read_));
        stderr_read_ = nullptr;
    }
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
    if (exit_code != 0 && error_.empty()) {
        std::lock_guard lock(mutex_);
        error_ = "ffmpeg failed: " + tail(stderr_text_, 600);
    }
    staging_.Reset();
    device_.Reset();
    context_.Reset();
}

}  // namespace obs
