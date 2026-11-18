#include "live/shader_path.h"

#include <windows.h>

#include <filesystem>

#include "platform/win_util.h"

namespace obs {

std::wstring resolve_shader_path(const std::wstring& file_name) {
    namespace fs = std::filesystem;
    const std::wstring exe = exe_dir();
    const fs::path candidates[] = {
        fs::path(exe) / L"shaders" / file_name,
        fs::path(exe).parent_path() / L"shaders" / file_name,
        fs::path(L"D:/program/OBScure/shaders") / file_name,
    };
    for (const auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path, ec)) return path.wstring();
    }
    return (fs::path(exe) / L"shaders" / file_name).wstring();
}

}  // namespace obs
