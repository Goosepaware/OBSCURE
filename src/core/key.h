#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/layout.h"

namespace obs {

// Fixed built-in key for "no key" / public transports. Anyone running OBScure
// can decode these; the tile permutation is still applied, so the video is not
// plaintext, but the key is not secret.
inline constexpr char kPublicKey[] = "obscure-public-v1";

struct KeyMaterial {
    std::vector<int32_t> perm;      // dest cell -> source cell
    std::vector<uint8_t> dihedral;  // dest cell -> 0..7
    std::vector<uint8_t> color;     // dest cell -> 0..7
    uint32_t key_id = 0;
    std::array<uint8_t, 32> seed{};
};

std::array<uint8_t, 32> sha256(const void* data, size_t size);
std::array<uint8_t, 32> sha256(const std::string& text);

uint32_t key_id_from_text(const std::string& text);
uint32_t public_key_id();

KeyMaterial derive_key(const Layout& layout);

}  // namespace obs
