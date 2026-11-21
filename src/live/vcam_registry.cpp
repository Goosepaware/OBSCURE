#include "live/vcam_registry.h"

#include <windows.h>

#include <filesystem>

#include "platform/win_util.h"

namespace obs {

namespace {

constexpr wchar_t kClsid[] = L"{5c2cd55c-92ad-4999-8666-912bd3e70010}";
constexpr wchar_t kCategory[] = L"{860BB310-5D01-11d0-BD3B-00A0C911CE86}";
constexpr wchar_t kName[] = L"OBScure Virtual Camera";

std::wstring instance_key() {
    return std::wstring(L"Software\\Classes\\CLSID\\") + kCategory + L"\\Instance\\" +
           kClsid;
}

bool set_default(HKEY key, const std::wstring& value) {
    const DWORD bytes =
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, nullptr, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()), bytes) ==
           ERROR_SUCCESS;
}

bool create_key(HKEY root, const std::wstring& path, HKEY* out) {
    return RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, out,
                           nullptr) == ERROR_SUCCESS;
}

}  // namespace

namespace {

bool same_file(const std::wstring& a, const std::wstring& b) {
    std::error_code ec;
    const auto size_a = std::filesystem::file_size(a, ec);
    if (ec) return false;
    const auto size_b = std::filesystem::file_size(b, ec);
    if (ec || size_a != size_b) return false;
    const auto time_a = std::filesystem::last_write_time(a, ec);
    if (ec) return false;
    const auto time_b = std::filesystem::last_write_time(b, ec);
    return !ec && time_a == time_b;
}

// Makes sure the DLL next to the executable is deployed into vcam\. When the
// canonical file is locked (OBS or a browser currently hosts the filter), a
// versioned copy is used instead so the next player start picks up new code.
std::wstring deploy_vcam_dll(std::string* error) {
    namespace fs = std::filesystem;
    const fs::path source = fs::path(exe_dir()) / L"obscure-vcam.dll";
    const fs::path dir = fs::path(exe_dir()) / L"vcam";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path target = dir / L"obscure-vcam.dll";

    if (fs::exists(source, ec) && (!fs::exists(target, ec) || !same_file(source.wstring(), target.wstring()))) {
        if (CopyFileW(source.c_str(), target.c_str(), FALSE)) return target.wstring();
        const auto size = fs::file_size(source, ec);
        wchar_t suffix[24]{};
        swprintf_s(suffix, L"%08llX", static_cast<unsigned long long>(ec ? 0 : size));
        const fs::path alt = dir / (std::wstring(L"obscure-vcam-") + suffix + L".dll");
        if (fs::exists(alt, ec) && same_file(source.wstring(), alt.wstring())) {
            return alt.wstring();
        }
        if (CopyFileW(source.c_str(), alt.c_str(), FALSE)) return alt.wstring();
        if (error) {
            *error =
                "无法更新虚拟摄像头 DLL（文件可能被 OBS/浏览器占用）。"
                "请完全退出 OBS 和浏览器后重启 OBScure。";
        }
        return {};
    }

    if (fs::exists(target, ec)) return target.wstring();
    // Packaged layout: pick the newest versioned DLL if the canonical one is absent.
    fs::path newest;
    fs::file_time_type newest_time{};
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"obscure-vcam", 0) != 0 ||
            entry.path().extension() != L".dll") {
            continue;
        }
        const auto time = fs::last_write_time(entry.path(), ec);
        if (ec) continue;
        if (newest.empty() || time > newest_time) {
            newest = entry.path();
            newest_time = time;
        }
    }
    if (!newest.empty()) return newest.wstring();

    if (error) {
        *error = "virtual camera DLL not found; expected " +
                 wide_to_utf8(target.wstring());
    }
    return {};
}

}  // namespace

std::wstring vcam_dll_path() {
    return (std::filesystem::path(exe_dir()) / "vcam" / "obscure-vcam.dll").wstring();
}

bool vcam_installed(std::string* error) {
    const std::wstring key_path =
        std::wstring(L"Software\\Classes\\CLSID\\") + kClsid + L"\\InprocServer32";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        if (error) *error = "virtual camera is not registered for this user";
        return false;
    }
    wchar_t dll[MAX_PATH * 4]{};
    DWORD type = 0;
    DWORD size = sizeof(dll);
    const LSTATUS status =
        RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(dll),
                         &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ || dll[0] == L'\0') {
        if (error) *error = "virtual camera registration is incomplete";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec)) {
        if (error) {
            *error = "虚拟摄像头 DLL 已被移动或删除，请重启 OBScure 重新注册。";
        }
        return false;
    }
    HKEY instance = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, instance_key().c_str(), 0, KEY_READ,
                      &instance) != ERROR_SUCCESS) {
        if (error) *error = "virtual camera device entry is missing";
        return false;
    }
    RegCloseKey(instance);
    return true;
}

bool vcam_install(std::string* error) {
    const std::wstring dll = deploy_vcam_dll(error);
    if (dll.empty()) return false;

    const std::wstring clsid_key = std::wstring(L"Software\\Classes\\CLSID\\") + kClsid;
    HKEY key = nullptr;
    if (!create_key(HKEY_CURRENT_USER, clsid_key, &key)) {
        if (error) *error = "cannot create virtual camera CLSID key";
        return false;
    }
    set_default(key, kName);
    RegCloseKey(key);

    if (!create_key(HKEY_CURRENT_USER, clsid_key + L"\\InprocServer32", &key)) {
        if (error) *error = "cannot create virtual camera InprocServer32 key";
        return false;
    }
    set_default(key, dll);
    const wchar_t threading[] = L"Both";
    RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(threading), sizeof(threading));
    RegCloseKey(key);

    if (!create_key(HKEY_CURRENT_USER, instance_key(), &key)) {
        if (error) *error = "cannot create virtual camera device entry";
        return false;
    }
    set_default(key, kName);
    const DWORD bytes = static_cast<DWORD>((wcslen(kClsid) + 1) * sizeof(wchar_t));
    RegSetValueExW(key, L"CLSID", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(kClsid), bytes);
    RegSetValueExW(key, L"FriendlyName", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(kName),
                   static_cast<DWORD>((wcslen(kName) + 1) * sizeof(wchar_t)));
    const wchar_t device_path[] = L"foo:bar";
    RegSetValueExW(key, L"DevicePath", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(device_path),
                   sizeof(device_path));
    RegCloseKey(key);
    return true;
}

bool vcam_uninstall(std::string* error) {
    const std::wstring clsid_key = std::wstring(L"Software\\Classes\\CLSID\\") + kClsid;
    const LSTATUS a = RegDeleteTreeW(HKEY_CURRENT_USER, clsid_key.c_str());
    const LSTATUS b = RegDeleteTreeW(HKEY_CURRENT_USER, instance_key().c_str());
    if ((a != ERROR_SUCCESS && a != ERROR_FILE_NOT_FOUND) ||
        (b != ERROR_SUCCESS && b != ERROR_FILE_NOT_FOUND)) {
        if (error) *error = "failed to remove virtual camera registration";
        return false;
    }
    return true;
}

}  // namespace obs
