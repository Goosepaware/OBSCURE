#include "io/ffmpeg_sink.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <sstream>
#include <thread>

#include "io/hw_encoder.h"
#include "platform/win_util.h"

namespace obs {

namespace {

std::wstring find_ffmpeg() {
    namespace fs = std::filesystem;
    const fs::path candidates[] = {
        fs::path(exe_dir()) / L"ffmpeg.exe",
        fs::path(exe_dir()).parent_path() / L"ffmpeg.exe",
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
                       VideoCodec codec, const std::string& encoder) {
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
    for (auto& staging : staging_) {
        if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) {
            if (error) *error = "ffmpeg sink: staging texture failed";
            stop();
            return false;
        }
    }
    next_slot_ = 0;
    pending_slots_.clear();

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

    encoder_ = encoder.empty() ? (codec_ == VideoCodec::H265
                                      ? "libx265"
                                      : codec_ == VideoCodec::Av1 ? "libsvtav1"
                                                                  : "libx264")
                               : encoder;
    const std::string args = video_encoder_args(encoder_, br, 60, mp4_like);

    std::wostringstream cmd;
    cmd << quote_arg(find_ffmpeg())
        << L" -hide_banner -loglevel error -y"
        << L" -f rawvideo -pixel_format bgra -video_size " << width_ << L"x" << height_
        << L" -framerate " << fps << L" -i - -an"
        << std::wstring(args.begin(), args.end())
        << L" -pix_fmt yuv420p -g 60 " << quote_arg(output);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = nul;
    si.hStdError = stderr_write;
    PROCESS_INFORMATION pi{};
    std::wstring command = cmd.str();
    command_line_ = wide_to_utf8(command);
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

void FfmpegSink::emit_slot(int slot) {
    if (slot < 0 || slot >= 3 || !staging_[slot] || !context_) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_[slot].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return;
    }
    std::vector<uint8_t> frame(static_cast<size_t>(width_) * height_ * 4);
    for (int y = 0; y < height_; ++y) {
        std::memcpy(frame.data() + static_cast<size_t>(y) * width_ * 4,
                    static_cast<const uint8_t*>(mapped.pData) +
                        static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(width_) * 4);
    }
    context_->Unmap(staging_[slot].Get(), 0);

    std::unique_lock lock(mutex_);
    space_cv_.wait(lock, [this] { return stop_ || failed_ || queue_.size() < 3; });
    if (stop_ || failed_) return;
    queue_.push_back(std::move(frame));
    cv_.notify_one();
}

void FfmpegSink::write(ID3D11Texture2D* texture) {
    if (!process_ || !texture || !context_) return;
    {
        std::lock_guard lock(mutex_);
        if (stop_ || failed_) return;
    }
    const int slot = next_slot_;
    next_slot_ = (next_slot_ + 1) % 3;
    if (!staging_[slot]) return;
    // Copy into a staging texture now; the map for this slot happens two calls
    // later so the GPU has finished the copy without stalling the caller.
    context_->CopyResource(staging_[slot].Get(), texture);

    int ready = -1;
    {
        std::lock_guard lock(mutex_);
        if (stop_ || failed_) return;
        pending_slots_.push_back(slot);
        if (pending_slots_.size() > 2) {
            ready = pending_slots_.front();
            pending_slots_.pop_front();
        }
    }
    if (ready >= 0) emit_slot(ready);
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
            const DWORD code = GetLastError();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            std::lock_guard lock(mutex_);
            failed_ = true;
            error_ = "ffmpeg sink: write failed (" + std::to_string(code) + ")";
            if (!stderr_text_.empty()) {
                error_ += " · " + tail(stderr_text_, 400);
            }
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
    // Flush the frames still inside the three-slot rotation so short recordings
    // keep every frame.
    for (;;) {
        int slot = -1;
        {
            std::lock_guard lock(mutex_);
            if (pending_slots_.empty() || failed_) break;
            slot = pending_slots_.front();
            pending_slots_.pop_front();
        }
        emit_slot(slot);
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
    for (auto& staging : staging_) staging.Reset();
    pending_slots_.clear();
    device_.Reset();
    context_.Reset();
}

}  // namespace obs
