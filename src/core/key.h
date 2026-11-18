#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/layout.h"

namespace obs {

struct KeyMaterial {
    std::vector<int32_t> perm;      // dest cell -> source cell
    std::vector<uint8_t> dihedral;  // dest cell -> 0..7
    std::vector<uint8_t> color;     // dest cell -> 0..7
    uint32_t key_id = 0;
    std::array<uint8_t, 32> seed{};
};

std::array<uint8_t, 32> sha256(const void* data, size_t size);
std::array<uint8_t, 32> sha256(const std::string& text);

KeyMaterial derive_key(const Layout& layout);

}  // namespace obs
