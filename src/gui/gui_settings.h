#pragma once

#include <climits>
#include <string>

namespace obs {

struct GuiSettings {
    int window_x = INT_MIN;
    int window_y = INT_MIN;
    int window_w = 0;
    int window_h = 0;
    bool maximized = false;

    bool detached_preview = false;
    int preview_x = INT_MIN;
    int preview_y = INT_MIN;
    int preview_w = 960;
    int preview_h = 540;
    bool preview_topmost = false;

    std::wstring export_dir;
    std::string export_format = "mp4";
    std::string export_codec = "h264";
    int export_bitrate = 8;
    std::wstring export_path_encode;
    std::wstring export_path_decode;
    std::wstring export_source_encode;
    std::wstring export_source_decode;

    bool player_loop = true;
};

std::wstring gui_settings_path();
bool load_gui_settings(GuiSettings* settings);
bool save_gui_settings(const GuiSettings& settings);

}  // namespace obs
