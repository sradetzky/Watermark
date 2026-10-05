#include "core/internal.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace watermark {
Image embed(const Image& host, const Payload& payload, const Parameters& parameters) {
    host.validate();
    detail::validate_parameters(parameters);
    if (host.width < 128 || host.height < 128) {
        throw std::invalid_argument("Embedding requires at least a 128 by 128 image.");
    }
    const auto pattern = detail::make_pattern(parameters.passphrase);
    const auto bits = payload.bits();
    const double strength = parameters.strength == Strength::low ? 0.7 :
                            parameters.strength == Strength::high ? 1.4 : 1.0;
    Image output = host;
    for (int y = 0; y + 8 <= host.height; y += 8) {
        detail::check_cancelled(parameters);
        for (int x = 0; x + 8 <= host.width; x += 8) {
            const auto original = detail::read_block(host, x, y);
            auto coefficients = detail::forward_dct(original);
            double energy = 0;
            for (int i = 1; i < 64; ++i) { energy += coefficients[i] * coefficients[i]; }
            const double texture = std::clamp(std::sqrt(energy / 63) / 12, 0.55, 1.0);
            const int block = ((y / 8) % 16) * 16 + (x / 8) % 16;
            for (int c = 0; c < detail::coefficient_count; ++c) {
                const auto chip = pattern[block * detail::coefficient_count + c];
                coefficients[detail::coefficient_indices[c]] +=
                    6.0 * strength * texture * chip.sign * (bits[chip.bit] ? 1 : -1);
            }
            const auto changed = detail::inverse_dct(coefficients);
            for (int dy = 0; dy < 8; ++dy) {
                for (int dx = 0; dx < 8; ++dx) {
                    auto& pixel = output.at(x + dx, y + dy);
                    const double delta = changed[dy * 8 + dx] - original[dy * 8 + dx];
                    const auto shift = [&](std::uint8_t value) {
                        return static_cast<std::uint8_t>(std::clamp(std::round(value + delta), 0.0, 255.0));
                    };
                    pixel.r = shift(pixel.r); pixel.g = shift(pixel.g); pixel.b = shift(pixel.b);
                }
            }
        }
    }
    return output;
}
} // namespace watermark
