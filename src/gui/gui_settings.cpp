#include "gui/gui_settings.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "platform/win_util.h"

namespace obs {

namespace {

std::string trim(std::string text) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

int parse_int(const std::map<std::string, std::string>& values, const char* key, int def) {
    const auto it = values.find(key);
    if (it == values.end()) return def;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return def;
    }
}

bool parse_bool(const std::map<std::string, std::string>& values, const char* key,
                bool def) {
    const auto it = values.find(key);
    if (it == values.end()) return def;
    return it->second == "1" || it->second == "true" || it->second == "yes";
}

std::wstring parse_wide(const std::map<std::string, std::string>& values,
                        const char* key, const std::wstring& def = {}) {
    const auto it = values.find(key);
    if (it == values.end()) return def;
    return utf8_to_wide(it->second);
}

}  // namespace

std::wstring gui_settings_path() {
    return app_data_dir() + L"\\settings.ini";
}

bool load_gui_settings(GuiSettings* settings) {
    if (!settings) return false;
    *settings = GuiSettings{};
    std::ifstream in(std::filesystem::path(gui_settings_path()), std::ios::binary);
    if (!in) return false;

    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }

    settings->window_x = parse_int(values, "window_x", settings->window_x);
    settings->window_y = parse_int(values, "window_y", settings->window_y);
    settings->window_w = parse_int(values, "window_w", settings->window_w);
    settings->window_h = parse_int(values, "window_h", settings->window_h);
    settings->maximized = parse_bool(values, "maximized", settings->maximized);
    settings->console_x = parse_int(values, "console_x", settings->console_x);
    settings->console_y = parse_int(values, "console_y", settings->console_y);
    settings->console_w = parse_int(values, "console_w", settings->console_w);
    settings->console_h = parse_int(values, "console_h", settings->console_h);

    settings->detached_preview =
        parse_bool(values, "detached_preview", settings->detached_preview);
    settings->preview_x = parse_int(values, "preview_x", settings->preview_x);
    settings->preview_y = parse_int(values, "preview_y", settings->preview_y);
    settings->preview_w = parse_int(values, "preview_w", settings->preview_w);
    settings->preview_h = parse_int(values, "preview_h", settings->preview_h);
    settings->preview_topmost =
        parse_bool(values, "preview_topmost", settings->preview_topmost);

    settings->export_dir = parse_wide(values, "export_dir");
    settings->export_format = values.count("export_format")
                                  ? values["export_format"]
                                  : settings->export_format;
    settings->export_codec = values.count("export_codec") ? values["export_codec"]
                                                          : settings->export_codec;
    settings->export_bitrate =
        parse_int(values, "export_bitrate", settings->export_bitrate);
    settings->export_path_encode = parse_wide(values, "export_path_encode");
    settings->export_path_decode = parse_wide(values, "export_path_decode");
    settings->export_source_encode = parse_wide(values, "export_source_encode");
    settings->export_source_decode = parse_wide(values, "export_source_decode");
    settings->player_loop = parse_bool(values, "player_loop", settings->player_loop);
    settings->audio_device_id = parse_wide(values, "audio_device_id");
    const auto vol = values.find("audio_volume");
    if (vol != values.end()) {
        try {
            settings->audio_volume = std::clamp(std::stof(vol->second), 0.0f, 2.0f);
        } catch (...) {
        }
    }
    settings->enc_audio = parse_bool(values, "enc_audio", settings->enc_audio);
    settings->enc_audio_algorithm =
        parse_int(values, "enc_audio_algorithm", settings->enc_audio_algorithm);
    settings->dec_audio = parse_bool(values, "dec_audio", settings->dec_audio);
    settings->dec_audio_algorithm =
        parse_int(values, "dec_audio_algorithm", settings->dec_audio_algorithm);
    return true;
}

bool save_gui_settings(const GuiSettings& settings) {
    const std::filesystem::path path(gui_settings_path());
    const std::filesystem::path tmp = path.wstring() + L".tmp";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ostringstream out;
    out << "window_x=" << settings.window_x << "\n";
    out << "window_y=" << settings.window_y << "\n";
    out << "window_w=" << settings.window_w << "\n";
    out << "window_h=" << settings.window_h << "\n";
    out << "maximized=" << (settings.maximized ? 1 : 0) << "\n";
    out << "console_x=" << settings.console_x << "\n";
    out << "console_y=" << settings.console_y << "\n";
    out << "console_w=" << settings.console_w << "\n";
    out << "console_h=" << settings.console_h << "\n";
    out << "detached_preview=" << (settings.detached_preview ? 1 : 0) << "\n";
    out << "preview_x=" << settings.preview_x << "\n";
    out << "preview_y=" << settings.preview_y << "\n";
    out << "preview_w=" << settings.preview_w << "\n";
    out << "preview_h=" << settings.preview_h << "\n";
    out << "preview_topmost=" << (settings.preview_topmost ? 1 : 0) << "\n";
    out << "export_dir=" << wide_to_utf8(settings.export_dir) << "\n";
    out << "export_format=" << settings.export_format << "\n";
    out << "export_codec=" << settings.export_codec << "\n";
    out << "export_bitrate=" << settings.export_bitrate << "\n";
    out << "export_path_encode=" << wide_to_utf8(settings.export_path_encode) << "\n";
    out << "export_path_decode=" << wide_to_utf8(settings.export_path_decode) << "\n";
    out << "export_source_encode=" << wide_to_utf8(settings.export_source_encode) << "\n";
    out << "export_source_decode=" << wide_to_utf8(settings.export_source_decode) << "\n";
    out << "player_loop=" << (settings.player_loop ? 1 : 0) << "\n";
    out << "audio_device_id=" << wide_to_utf8(settings.audio_device_id) << "\n";
    out << "audio_volume=" << settings.audio_volume << "\n";
    out << "enc_audio=" << (settings.enc_audio ? 1 : 0) << "\n";
    out << "enc_audio_algorithm=" << settings.enc_audio_algorithm << "\n";
    out << "dec_audio=" << (settings.dec_audio ? 1 : 0) << "\n";
    out << "dec_audio_algorithm=" << settings.dec_audio_algorithm << "\n";

    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        const std::string text = out.str();
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) return false;
    }

    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // Keep the new settings rather than silently losing them.
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
        return !ec;
    }
    return true;
}

}  // namespace obs
