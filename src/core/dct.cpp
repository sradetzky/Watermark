#include "core/internal.h"

#include <cmath>

namespace watermark::detail {
const DctBasis& dct_basis() {
    static const DctBasis value = [] {
        DctBasis result{};
        constexpr double pi = 3.14159265358979323846;
        for (int u = 0; u < 8; ++u) {
            const double scale = u == 0 ? std::sqrt(1.0 / 8) : 0.5;
            for (int x = 0; x < 8; ++x) { result[u][x] = scale * std::cos((2 * x + 1) * u * pi / 16); }
        }
        return result;
    }();
    return value;
}
Block forward_dct(const Block& pixels) {
    Block rows{}, result{};
    const auto& b = dct_basis();
    for (int y = 0; y < 8; ++y) {
        for (int u = 0; u < 8; ++u) {
            for (int x = 0; x < 8; ++x) { rows[y * 8 + u] += pixels[y * 8 + x] * b[u][x]; }
        }
    }
    for (int v = 0; v < 8; ++v) {
        for (int u = 0; u < 8; ++u) {
            for (int y = 0; y < 8; ++y) { result[v * 8 + u] += rows[y * 8 + u] * b[v][y]; }
        }
    }
    return result;
}
Block inverse_dct(const Block& coefficients) {
    Block columns{}, result{};
    const auto& b = dct_basis();
    for (int y = 0; y < 8; ++y) {
        for (int u = 0; u < 8; ++u) {
            for (int v = 0; v < 8; ++v) { columns[y * 8 + u] += coefficients[v * 8 + u] * b[v][y]; }
        }
    }
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            for (int u = 0; u < 8; ++u) { result[y * 8 + x] += columns[y * 8 + u] * b[u][x]; }
        }
    }
    return result;
}
double luminance(const Pixel& pixel) { return 0.299 * pixel.r + 0.587 * pixel.g + 0.114 * pixel.b; }
Block read_block(const Image& image, int x, int y) {
    Block result{};
    for (int dy = 0; dy < 8; ++dy) {
        for (int dx = 0; dx < 8; ++dx) { result[dy * 8 + dx] = luminance(image.at(x + dx, y + dy)); }
    }
    return result;
}
} // namespace watermark::detail
