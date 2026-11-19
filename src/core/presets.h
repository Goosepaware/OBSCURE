#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace obs {

// Stable preset IDs are 1-based table positions and are stored in transport
// metadata. Append new presets at the end; never reorder existing entries.
struct Preset {
    const char* id;
    const char* label;
    int cell;
    int guard;
    double scale;
    int bitrate;
    int algorithm = 0;  // 0 = classic grid, 1 = Hilbert spatial curve
};

inline constexpr Preset kPresets[] = {
    {"c32_g2_s100", "高质量 32px / 1.0x", 32, 2, 1.0, 10},
    {"c32_g2_s120", "平衡 32px / 1.2x（推荐）", 32, 2, 1.2, 8},
    {"c32_g2_s140", "隐私 32px / 1.4x", 32, 2, 1.4, 6},
    {"c16_g2_s100", "强隐私 16px / 2px", 16, 2, 1.0, 10},
    {"c16_g1_s100", "强隐私 16px / 1px", 16, 1, 1.0, 10},
    {"c64_g2_s100", "画质优先 64px / 1.0x", 64, 2, 1.0, 12},
    {"c64_g2_s120", "画质优先 64px / 1.2x", 64, 2, 1.2, 12},
    {"h8_g0_s100", "空间曲线 8px 满屏（细粒度）", 8, 0, 1.0, 12, 1},
    {"h16_g0_s100", "空间曲线 16px 满屏（推荐）", 16, 0, 1.0, 12, 1},
    {"h32_g0_s100", "空间曲线 32px 满屏（极清）", 32, 0, 1.0, 12, 1},
};

inline constexpr int kPresetCount = static_cast<int>(std::size(kPresets));

inline const Preset* preset_by_index(int index) {
    if (index < 0 || index >= kPresetCount) return nullptr;
    return &kPresets[index];
}

inline const Preset* preset_by_id(int id) {
    return preset_by_index(id - 1);
}

inline int preset_index_by_id(const std::string& id) {
    for (int i = 0; i < kPresetCount; ++i) {
        if (id == kPresets[i].id) return i;
    }
    return -1;
}

inline int preset_scale_x10(const Preset& preset) {
    return static_cast<int>(std::lround(preset.scale * 10.0));
}

// Best-effort match for old recordings that predate preset_id. scale_x10 == 0
// means the exact scale is unknown, so geometry (cell/guard) is enough.
inline int preset_index_by_geometry(int cell, int guard, int scale_x10,
                                    int algorithm = 0) {
    for (int i = 0; i < kPresetCount; ++i) {
        const auto& p = kPresets[i];
        if (p.cell != cell || p.guard != guard || p.algorithm != algorithm) continue;
        if (scale_x10 == 0 || preset_scale_x10(p) == scale_x10) return i;
    }
    // Fall back to geometry only for legacy metadata without an algorithm bit.
    if (algorithm == 0) {
        for (int i = 0; i < kPresetCount; ++i) {
            const auto& p = kPresets[i];
            if (p.cell != cell || p.guard != guard) continue;
            if (scale_x10 == 0 || preset_scale_x10(p) == scale_x10) return i;
        }
    }
    return -1;
}

inline int default_preset_for_algorithm(int algorithm) {
    if (algorithm == 1) {
        const int idx = preset_index_by_id("h16_g0_s100");
        if (idx >= 0) return idx;
    }
    const int idx = preset_index_by_id("c32_g2_s120");
    return idx >= 0 ? idx : 0;
}

}  // namespace obs
