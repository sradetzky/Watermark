#pragma once

#include "core/watermark.h"
#include <array>
#include <string_view>

namespace watermark::detail {
using Block = std::array<double, 64>;
using DctBasis = std::array<std::array<double, 8>, 8>;
const DctBasis& dct_basis();
void check_cancelled(const Parameters& parameters);
Block forward_dct(const Block& pixels);
Block inverse_dct(const Block& coefficients);
double luminance(const Pixel& pixel);
Block read_block(const Image& image, int x, int y);
std::array<std::uint8_t, 32> sha256(const std::vector<std::uint8_t>& bytes);
std::uint16_t crc16(const std::array<bool, payload_bit_count>& bits);
std::optional<Payload> decode_payload(const std::array<bool, payload_bit_count>& bits);

constexpr int tile_blocks = 16;
constexpr int coefficient_count = 8;
constexpr int pattern_size = tile_blocks * tile_blocks * coefficient_count;
// Indices are v * 8 + u. DC and the highest frequency coefficients are excluded.
constexpr std::array<int, coefficient_count> coefficient_indices = {11, 18, 25, 4, 32, 19, 26, 33};
struct Chip { int bit = 0; double sign = 1; };
using Pattern = std::array<Chip, pattern_size>;
Pattern make_pattern(std::string_view passphrase);
void validate_parameters(const Parameters& parameters);
} // namespace watermark::detail
