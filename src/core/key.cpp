#include "core/key.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <stdexcept>

namespace obs {

namespace {

bool bcrypt_ok(NTSTATUS s) { return BCRYPT_SUCCESS(s); }

}  // namespace

std::array<uint8_t, 32> sha256(const void* data, size_t size) {
    std::array<uint8_t, 32> out{};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!bcrypt_ok(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    }
    if (!bcrypt_ok(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptCreateHash failed");
    }
    if (size > 0) {
        if (!bcrypt_ok(BCryptHashData(
                hash,
                const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data)),
                static_cast<ULONG>(size), 0))) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            throw std::runtime_error("BCryptHashData failed");
        }
    }
    if (!bcrypt_ok(BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0))) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptFinishHash failed");
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::array<uint8_t, 32> sha256(const std::string& text) {
    return sha256(text.data(), text.size());
}

KeyMaterial derive_key(const Layout& layout) {
    layout.validate();
    const std::string seed_text =
        layout.key + "|v1|" + std::to_string(layout.cols()) + "x" +
        std::to_string(layout.rows()) + "|" + std::to_string(layout.cell) + "|" +
        std::to_string(layout.guard);
    const auto seed = sha256(seed_text);

    uint64_t seed64 = 0;
    for (int i = 0; i < 8; ++i) {
        seed64 |= static_cast<uint64_t>(seed[i]) << (8 * i);
    }
    std::mt19937_64 rng(seed64);

    const int n = layout.blocks();
    KeyMaterial km;
    km.seed = seed;
    km.perm.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        km.perm[static_cast<size_t>(i)] = i;
    }
    for (int i = n - 1; i > 0; --i) {
        const uint64_t bound = static_cast<uint64_t>(i) + 1;
        const uint64_t limit = UINT64_MAX - (UINT64_MAX % bound);
        uint64_t r = 0;
        do {
            r = rng();
        } while (r >= limit);
        const int j = static_cast<int>(r % bound);
        std::swap(km.perm[static_cast<size_t>(i)], km.perm[static_cast<size_t>(j)]);
    }

    km.dihedral.resize(static_cast<size_t>(n));
    km.color.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        km.dihedral[static_cast<size_t>(i)] = static_cast<uint8_t>(rng() & 7u);
        km.color[static_cast<size_t>(i)] = static_cast<uint8_t>(rng() & 7u);
    }

    const auto id = sha256("OBSCure-key-id|" + layout.key);
    km.key_id = (static_cast<uint32_t>(id[0]) << 24) |
                (static_cast<uint32_t>(id[1]) << 16) |
                (static_cast<uint32_t>(id[2]) << 8) |
                static_cast<uint32_t>(id[3]);
    return km;
}

}  // namespace obs
