#include "platform/win_util.h"

#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace obs {

namespace {

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

struct EnumContext {
    std::vector<WindowInfo>* out = nullptr;
    DWORD own_pid = 0;
};

struct MonitorContext {
    std::vector<MonitorInfo>* out = nullptr;
    int index = 0;
};

BOOL CALLBACK enum_monitor_proc(HMONITOR monitor, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<MonitorContext*>(lp);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        MonitorInfo item;
        item.index = ctx->index++;
        item.name = info.szDevice;
        item.width = info.rcMonitor.right - info.rcMonitor.left;
        item.height = info.rcMonitor.bottom - info.rcMonitor.top;
        ctx->out->push_back(std::move(item));
    }
    return TRUE;
}

BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumContext*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->own_pid) return TRUE;
    const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;

    wchar_t title[512]{};
    const int n = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    if (n <= 0) return TRUE;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width < 64 || height < 64) return TRUE;

    WindowInfo info;
    info.hwnd = hwnd;
    info.title = title;
    info.process = process_name(hwnd);
    info.width = width;
    info.height = height;
    info.minimized = IsIconic(hwnd) != FALSE;
    ctx->out->push_back(std::move(info));
    return TRUE;
}

}  // namespace

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

std::string wide_to_acp(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, text.data(), static_cast<int>(text.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

std::wstring acp_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::wstring exe_dir() {
    wchar_t path[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (n == 0) return L".";
    std::wstring text(path, n);
    const size_t pos = text.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return text.substr(0, pos);
}

std::wstring app_data_dir() {
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buffer,
                                            static_cast<DWORD>(std::size(buffer)));
    std::wstring base = (n > 0) ? std::wstring(buffer, n) : exe_dir();
    const std::wstring dir = base + L"\\OBScure";
    std::filesystem::create_directories(dir);
    return dir;
}

std::wstring open_file_dialog(const wchar_t* title, const wchar_t* filter) {
    wchar_t file[MAX_PATH * 4]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    return file;
}

std::wstring save_file_dialog(const wchar_t* title, const wchar_t* filter,
                              const std::wstring& default_name) {
    wchar_t file[MAX_PATH * 4]{};
    if (!default_name.empty()) {
        wcsncpy_s(file, default_name.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return {};
    return file;
}

std::wstring window_title(HWND hwnd) {
    wchar_t title[512]{};
    const int n = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    return std::wstring(title, static_cast<size_t>(std::max(0, n)));
}

std::wstring process_name(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH * 4]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        std::wstring full(path, size);
        const size_t pos = full.find_last_of(L"\\/");
        name = (pos == std::wstring::npos) ? full : full.substr(pos + 1);
    }
    CloseHandle(process);
    return name;
}

bool window_client_size(HWND hwnd, int* width, int* height) {
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    if (width) *width = rc.right - rc.left;
    if (height) *height = rc.bottom - rc.top;
    return true;
}

std::vector<WindowInfo> enumerate_capture_windows() {
    std::vector<WindowInfo> out;
    EnumContext ctx;
    ctx.out = &out;
    ctx.own_pid = GetCurrentProcessId();
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
    std::sort(out.begin(), out.end(), [](const WindowInfo& a, const WindowInfo& b) {
        const std::wstring at = lower(a.title);
        const std::wstring bt = lower(b.title);
        if (at != bt) return at < bt;
        return a.hwnd < b.hwnd;
    });
    return out;
}

std::vector<MonitorInfo> enumerate_monitors() {
    std::vector<MonitorInfo> out;
    MonitorContext ctx;
    ctx.out = &out;
    EnumDisplayMonitors(nullptr, nullptr, enum_monitor_proc,
                        reinterpret_cast<LPARAM>(&ctx));
    return out;
}

}  // namespace obs
