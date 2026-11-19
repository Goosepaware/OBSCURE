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

// Tile scrambling algorithms. Grid is the original OBScure v1 layout (random
// permutation, dihedral flips, guard bands). Hilbert walks the cell grid with a
// 2D Hilbert space-filling curve, then key-shifts and key-shuffles contiguous
// curve segments. Every cell is used exactly once, tiles stay upright, and most
// neighbouring cells stay neighbours, which preserves motion for video codecs.
enum class ScrambleAlgorithm : int {
    Grid = 0,
    Hilbert = 1,
};

const char* scramble_algorithm_id(int algorithm);
int scramble_algorithm_from_id(const std::string& id);

// Generalized 2D Hilbert ("Gilbert") traversal of a cols x rows cell grid.
// Returns physical cell indices (y * cols + x) in curve order. Every cell
// appears exactly once for any cols >= 1, rows >= 1.
std::vector<int32_t> hilbert_curve_order(int cols, int rows);

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
