#include "core/internal.h"

#include <cmath>

namespace watermark {
namespace {
constexpr std::uint16_t magic = 0x574D;
}
std::array<bool, payload_bit_count> Payload::bits() const {
    std::array<bool, payload_bit_count> result{};
    // Version 1 wire order: 64-bit hash, 16-bit magic, CRC-16/CCITT-FALSE, MSB first.
    for (int i = 0; i < 64; ++i) { result[i] = ((source_hash >> (63 - i)) & 1) != 0; }
    for (int i = 0; i < 16; ++i) { result[64 + i] = ((magic >> (15 - i)) & 1) != 0; }
    const auto crc = detail::crc16(result);
    for (int i = 0; i < 16; ++i) { result[80 + i] = ((crc >> (15 - i)) & 1) != 0; }
    return result;
}

Payload payload_from_source(const Image& source) {
    const auto normalized = resize_image(source, 32, 32);
    std::array<double, 32 * 32> gray{};
    for (std::size_t i = 0; i < gray.size(); ++i) { gray[i] = detail::luminance(normalized.pixels[i]); }
    constexpr double pi = 3.14159265358979323846;
    Payload payload;
    for (int v = 1; v <= 8; ++v) {
        for (int u = 1; u <= 8; ++u) {
            double coefficient = 0;
            for (int y = 0; y < 32; ++y) {
                const double cy = std::cos((2 * y + 1) * v * pi / 64);
                for (int x = 0; x < 32; ++x) {
                    coefficient += gray[y * 32 + x] * std::cos((2 * x + 1) * u * pi / 64) * cy;
                }
            }
            payload.source_hash = (payload.source_hash << 1) | (coefficient > 1e-8 ? 1ULL : 0ULL);
        }
    }
    return payload;
}

namespace detail {
std::uint16_t crc16(const std::array<bool, payload_bit_count>& bits) {
    std::uint16_t crc = 0xFFFF;
    for (int i = 0; i < 80; ++i) {
        const bool feedback = ((crc & 0x8000) != 0) != bits[i];
        crc = static_cast<std::uint16_t>(crc << 1);
        if (feedback) { crc ^= 0x1021; }
    }
    return crc;
}
std::optional<Payload> decode_payload(const std::array<bool, payload_bit_count>& bits) {
    std::uint16_t recovered_magic = 0, recovered_crc = 0;
    Payload payload;
    for (int i = 0; i < 64; ++i) { payload.source_hash = (payload.source_hash << 1) | (bits[i] ? 1ULL : 0ULL); }
    for (int i = 0; i < 16; ++i) {
        recovered_magic = static_cast<std::uint16_t>((recovered_magic << 1) | (bits[64 + i] ? 1 : 0));
        recovered_crc = static_cast<std::uint16_t>((recovered_crc << 1) | (bits[80 + i] ? 1 : 0));
    }
    if (recovered_magic != magic || recovered_crc != crc16(bits)) { return std::nullopt; }
    return payload;
}
}
} // namespace watermark
