#include "core/key.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <stdexcept>

// Tile scrambling algorithms. The curve walk is a clean-room implementation of
// the classic generalized Hilbert (Gilbert) space-filling curve: it maps a 2D
// cell grid to a 1D path where consecutive cells are always spatial neighbours.
// The pixel positions stay deterministic and fully covered; only the traversal
// order is keyed.

namespace obs {

namespace {

bool bcrypt_ok(NTSTATUS s) { return BCRYPT_SUCCESS(s); }

// Recursive generalized Hilbert curve over an arbitrary cols x rows rectangle.
// Based on the well-known Gilbert2d construction (public-domain algorithm);
// this is an independent implementation, not copied from any application.
class GilbertBuilder {
public:
    explicit GilbertBuilder(int cols, int rows) : cols_(cols), rows_(rows) {}

    std::vector<int32_t> build() {
        out_.clear();
        out_.reserve(static_cast<size_t>(cols_) * static_cast<size_t>(rows_));
        if (cols_ >= rows_) {
            generate(0, 0, cols_, 0, 0, rows_);
        } else {
            generate(0, 0, 0, rows_, cols_, 0);
        }
        return std::move(out_);
    }

private:
    void generate(int x, int y, int ax, int ay, int bx, int by) {
        const int w = std::abs(ax + ay);
        const int h = std::abs(bx + by);
        const int dax = sign(ax);
        const int day = sign(ay);
        const int dbx = sign(bx);
        const int dby = sign(by);

        if (h == 1) {
            for (int i = 0; i < w; ++i) {
                push(x, y);
                x += dax;
                y += day;
            }
            return;
        }
        if (w == 1) {
            for (int i = 0; i < h; ++i) {
                push(x, y);
                x += dbx;
                y += dby;
            }
            return;
        }

        int ax2 = ax / 2;
        int ay2 = ay / 2;
        int bx2 = bx / 2;
        int by2 = by / 2;
        const int w2 = std::abs(ax2 + ay2);
        const int h2 = std::abs(bx2 + by2);

        if (2 * w > 3 * h) {
            if ((w2 & 1) != 0 && w > 2) {
                ax2 += dax;
                ay2 += day;
            }
            generate(x, y, ax2, ay2, bx, by);
            generate(x + ax2, y + ay2, ax - ax2, ay - ay2, bx, by);
        } else {
            if ((h2 & 1) != 0 && h > 2) {
                bx2 += dbx;
                by2 += dby;
            }
            generate(x, y, bx2, by2, ax2, ay2);
            generate(x + bx2, y + by2, ax, ay, bx - bx2, by - by2);
            generate(x + (ax - dax) + (bx2 - dbx), y + (ay - day) + (by2 - dby),
                     -bx2, -by2, -(ax - ax2), -(ay - ay2));
        }
    }

    static int sign(int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

    void push(int x, int y) {
        if (x < 0 || y < 0 || x >= cols_ || y >= rows_) return;
        out_.push_back(static_cast<int32_t>(y) * cols_ + x);
    }

    int cols_ = 0;
    int rows_ = 0;
    std::vector<int32_t> out_;
};

}  // namespace

const char* scramble_algorithm_id(int algorithm) {
    return algorithm == static_cast<int>(ScrambleAlgorithm::Hilbert) ? "hilbert"
                                                                     : "grid";
}

int scramble_algorithm_from_id(const std::string& id) {
    if (id == "hilbert" || id == "curve" || id == "spatial" || id == "h") {
        return static_cast<int>(ScrambleAlgorithm::Hilbert);
    }
    return static_cast<int>(ScrambleAlgorithm::Grid);
}

std::vector<int32_t> hilbert_curve_order(int cols, int rows) {
    if (cols < 1 || rows < 1) return {};
    return GilbertBuilder(cols, rows).build();
}

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

uint32_t key_id_from_text(const std::string& text) {
    const auto id = sha256("OBSCure-key-id|" + text);
    return (static_cast<uint32_t>(id[0]) << 24) |
           (static_cast<uint32_t>(id[1]) << 16) |
           (static_cast<uint32_t>(id[2]) << 8) |
           static_cast<uint32_t>(id[3]);
}

uint32_t public_key_id() {
    return key_id_from_text(kPublicKey);
}

KeyMaterial derive_key(const Layout& layout) {
    layout.validate();
    std::string seed_text =
        layout.key + "|v1|" + std::to_string(layout.cols()) + "x" +
        std::to_string(layout.rows()) + "|" + std::to_string(layout.cell) + "|" +
        std::to_string(layout.guard);
    if (layout.algorithm == static_cast<int>(ScrambleAlgorithm::Hilbert)) {
        // Domain-separate curve material so the same text key cannot produce the
        // same permutation in the two algorithms.
        seed_text += "|hilbert-v1";
    }
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

    if (layout.algorithm == static_cast<int>(ScrambleAlgorithm::Hilbert)) {
        const std::vector<int32_t> curve = hilbert_curve_order(layout.cols(), layout.rows());
        if (static_cast<int>(curve.size()) == n) {
            // Walk the curve, then cut it into a fixed number of contiguous
            // arcs. Each arc covers a compact region of the picture, so moving
            // whole arcs keeps spatial neighbourhoods and inter-frame motion.
            // The key picks a cyclic start offset, the arc order, and whether
            // each arc is traversed backwards.
            const int segments = std::min(64, n);
            const uint64_t shift = rng() % static_cast<uint64_t>(n);

            std::vector<int> order(static_cast<size_t>(segments));
            for (int i = 0; i < segments; ++i) order[static_cast<size_t>(i)] = i;
            for (int i = segments - 1; i > 0; --i) {
                const uint64_t bound = static_cast<uint64_t>(i) + 1;
                const uint64_t limit = UINT64_MAX - (UINT64_MAX % bound);
                uint64_t r = 0;
                do {
                    r = rng();
                } while (r >= limit);
                const int j = static_cast<int>(r % bound);
                std::swap(order[static_cast<size_t>(i)],
                          order[static_cast<size_t>(j)]);
            }

            std::vector<int> start(static_cast<size_t>(segments) + 1, 0);
            for (int i = 0; i <= segments; ++i) {
                start[static_cast<size_t>(i)] =
                    static_cast<int>((static_cast<int64_t>(i) * n) / segments);
            }
            std::vector<uint8_t> backwards(static_cast<size_t>(segments));
            for (int i = 0; i < segments; ++i) {
                backwards[static_cast<size_t>(i)] = static_cast<uint8_t>(rng() & 1u);
            }

            std::vector<int32_t> src_seq(static_cast<size_t>(n));
            int pos = 0;
            for (int oi = 0; oi < segments; ++oi) {
                const int s = order[static_cast<size_t>(oi)];
                const int len =
                    start[static_cast<size_t>(s) + 1] - start[static_cast<size_t>(s)];
                for (int t = 0; t < len; ++t) {
                    const int u = backwards[static_cast<size_t>(s)] ? (len - 1 - t) : t;
                    const int curve_pos = static_cast<int>(
                        (static_cast<uint64_t>(start[static_cast<size_t>(s)]) +
                         static_cast<uint64_t>(u) + shift) %
                        static_cast<uint64_t>(n));
                    src_seq[static_cast<size_t>(pos++)] = curve[curve_pos];
                }
            }
            // perm maps destination cell -> source cell.
            for (int j = 0; j < n; ++j) {
                km.perm[static_cast<size_t>(curve[static_cast<size_t>(j)])] =
                    src_seq[static_cast<size_t>(j)];
            }
            // Upright, unmodified tiles keep motion vectors and intra
            // prediction valid. Color scrambling is also off for this mode.
            km.dihedral.assign(static_cast<size_t>(n), 0);
            km.color.assign(static_cast<size_t>(n), 0);
            km.key_id = key_id_from_text(layout.key);
            return km;
        }
        // Geometry outside the curve generator's contract: fall back to the
        // classic grid permutation rather than producing a broken mapping.
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

    km.key_id = key_id_from_text(layout.key);
    return km;
}

}  // namespace obs
