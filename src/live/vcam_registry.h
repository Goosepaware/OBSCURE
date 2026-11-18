#pragma once

#include <string>

namespace obs {

// Per-user DirectShow registration for the OBScure virtual camera. No admin
// rights are required; the keys live under HKCU\Software\Classes.
bool vcam_installed(std::string* error = nullptr);
bool vcam_install(std::string* error = nullptr);
bool vcam_uninstall(std::string* error = nullptr);
std::wstring vcam_dll_path();

}  // namespace obs
