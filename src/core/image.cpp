#include "core/watermark.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace watermark {
namespace {
std::size_t pixel_count(int width, int height) {
    // WIC's buffers and strides are UINT-sized. Keep every image under that bound.
    if (width <= 0 || height <= 0 ||
        static_cast<std::uint64_t>(width) * height >
            std::numeric_limits<std::uint32_t>::max() / sizeof(Pixel)) {
        throw std::invalid_argument("Invalid or excessively large image dimensions.");
    }
    return static_cast<std::size_t>(width) * height;
}
std::uint8_t channel(double value) {
    return static_cast<std::uint8_t>(std::clamp(std::round(value), 0.0, 255.0));
}
}

Image::Image(int image_width, int image_height)
    : width(image_width), height(image_height), pixels(pixel_count(width, height)) {}

void Image::validate() const {
    if (pixels.size() != pixel_count(width, height)) {
        throw std::invalid_argument("Image buffer does not match its dimensions.");
    }
}
Pixel& Image::at(int x, int y) {
    if (x < 0 || y < 0 || x >= width || y >= height) { throw std::out_of_range("Pixel coordinates out of range."); }
    return pixels.at(static_cast<std::size_t>(y) * width + x);
}
const Pixel& Image::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= width || y >= height) { throw std::out_of_range("Pixel coordinates out of range."); }
    return pixels.at(static_cast<std::size_t>(y) * width + x);
}

Image resize_image(const Image& image, int width, int height) {
    image.validate();
    if (width == image.width && height == image.height) { return image; }
    Image result(width, height);
    for (int y = 0; y < height; ++y) {
        const double sy = std::clamp((y + 0.5) * image.height / height - 0.5,
                                     0.0, static_cast<double>(image.height - 1));
        const int y0 = static_cast<int>(sy);
        const int y1 = std::min(y0 + 1, image.height - 1);
        const double fy = sy - y0;
        for (int x = 0; x < width; ++x) {
            const double sx = std::clamp((x + 0.5) * image.width / width - 0.5,
                                         0.0, static_cast<double>(image.width - 1));
            const int x0 = static_cast<int>(sx);
            const int x1 = std::min(x0 + 1, image.width - 1);
            const double fx = sx - x0;
            const auto interpolate = [&](std::uint8_t Pixel::*member) {
                const double top = image.at(x0, y0).*member * (1 - fx) + image.at(x1, y0).*member * fx;
                const double bottom = image.at(x0, y1).*member * (1 - fx) + image.at(x1, y1).*member * fx;
                return channel(top * (1 - fy) + bottom * fy);
            };
            result.at(x, y) = {interpolate(&Pixel::r), interpolate(&Pixel::g),
                               interpolate(&Pixel::b), interpolate(&Pixel::a)};
        }
    }
    return result;
}

double psnr(const Image& original, const Image& modified) {
    original.validate();
    modified.validate();
    if (original.width != modified.width || original.height != modified.height) {
        throw std::invalid_argument("PSNR requires equal image dimensions.");
    }
    double squared_error = 0;
    for (std::size_t i = 0; i < original.pixels.size(); ++i) {
        const auto& a = original.pixels[i];
        const auto& b = modified.pixels[i];
        for (const int delta : {int(a.r) - b.r, int(a.g) - b.g, int(a.b) - b.b}) {
            squared_error += delta * delta;
        }
    }
    if (squared_error == 0) { return std::numeric_limits<double>::infinity(); }
    return 10 * std::log10(255.0 * 255.0 * original.pixels.size() * 3 / squared_error);
}
} // namespace watermark
