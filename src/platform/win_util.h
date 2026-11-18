#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace obs {

std::wstring utf8_to_wide(const std::string& text);
std::string wide_to_utf8(const std::wstring& text);

// OpenCV 4.10 MSMF converts paths with CP_ACP internally. Always hand it an
// ANSI string, never UTF-8, or Chinese paths will fail.
std::string wide_to_acp(const std::wstring& text);
std::wstring acp_to_wide(const std::string& text);

std::wstring exe_dir();
std::wstring app_data_dir();

std::wstring open_file_dialog(const wchar_t* title, const wchar_t* filter);
std::wstring save_file_dialog(const wchar_t* title, const wchar_t* filter,
                              const std::wstring& default_name);

struct WindowInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring process;
    int width = 0;
    int height = 0;
    bool minimized = false;
};

struct MonitorInfo {
    int index = 0;
    std::wstring name;
    int width = 0;
    int height = 0;
};

std::vector<WindowInfo> enumerate_capture_windows();
std::vector<MonitorInfo> enumerate_monitors();
std::wstring window_title(HWND hwnd);
std::wstring process_name(HWND hwnd);
bool window_client_size(HWND hwnd, int* width, int* height);

}  // namespace obs
