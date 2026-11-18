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
    const std::wstring dll = vcam_dll_path();
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec)) {
        if (error) {
            *error = "virtual camera DLL not found; expected " +
                     wide_to_utf8(dll);
        }
        return false;
    }

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
