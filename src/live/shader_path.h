#pragma once

#include <string>

namespace obs {

// Resolve shaders/<name> relative to the executable, then the source tree.
std::wstring resolve_shader_path(const std::wstring& file_name);

}  // namespace obs
