#include "core/watermark.h"

#include <algorithm>
#include <cmath>

namespace watermark {
Image apply_visible_watermark(const Image& host, const Image& silhouette,
                              const VisibleWatermark& options,
                              const std::function<bool()>& cancelled) {
    host.validate(); silhouette.validate();
    if (options.size_percent < 1 || options.size_percent > 50 ||
        options.opacity_percent < 1 || options.opacity_percent > 100 ||
        (options.ink != VisibleInk::black && options.ink != VisibleInk::white)) {
        throw std::invalid_argument("Visible size must be 1..50 percent, opacity 1..100 percent, and ink black or white.");
    }
    const auto check_cancelled = [&] { if (cancelled && cancelled()) { throw OperationCancelled(); } };
    int left = silhouette.width, top = silhouette.height, right = -1, bottom = -1;
    bool transparent = false;
    for (int y = 0; y < silhouette.height; ++y) {
        check_cancelled();
        for (int x = 0; x < silhouette.width; ++x) {
            if (silhouette.at(x, y).a == 0) { transparent = true; continue; }
            left = std::min(left, x); right = std::max(right, x);
            top = std::min(top, y); bottom = std::max(bottom, y);
        }
    }
    if (right < left || !transparent) {
        throw std::invalid_argument("Visible image must contain a silhouette and a transparent background. Use a cutout PNG.");
    }
    Image cropped(right - left + 1, bottom - top + 1);
    for (int y = 0; y < cropped.height; ++y) {
        check_cancelled();
        for (int x = 0; x < cropped.width; ++x) { cropped.at(x, y) = silhouette.at(left + x, top + y); }
    }
    const int short_side = std::min(host.width, host.height);
    const int edge = std::max(1, short_side * options.size_percent / 100);
    const double scale = double(edge) / std::max(cropped.width, cropped.height);
    const auto stamp = resize_image(cropped, std::max(1, static_cast<int>(std::round(cropped.width * scale))),
                                   std::max(1, static_cast<int>(std::round(cropped.height * scale))));
    check_cancelled();
    const int margin = short_side * 2 / 100;
    int x0 = margin, y0 = margin;
    switch (options.position) {
    case Position::bottom_right: x0 = host.width - margin - stamp.width; y0 = host.height - margin - stamp.height; break;
    case Position::bottom_left: y0 = host.height - margin - stamp.height; break;
    case Position::top_right: x0 = host.width - margin - stamp.width; break;
    case Position::top_left: break;
    case Position::center: x0 = (host.width - stamp.width) / 2; y0 = (host.height - stamp.height) / 2; break;
    default: throw std::invalid_argument("Invalid visible watermark position.");
    }
    Image result = host;
    const double ink = options.ink == VisibleInk::black ? 0.0 : 255.0;
    for (int y = 0; y < stamp.height; ++y) {
        check_cancelled();
        for (int x = 0; x < stamp.width; ++x) {
            const double alpha = stamp.at(x, y).a / 255.0 * options.opacity_percent / 100.0;
            if (alpha == 0) { continue; }
            auto& pixel = result.at(x0 + x, y0 + y);
            const double retained = pixel.a / 255.0 * (1 - alpha);
            const double output_alpha = alpha + retained;
            const auto blend = [&](std::uint8_t channel) {
                return static_cast<std::uint8_t>(std::clamp(std::round((ink * alpha + channel * retained) / output_alpha), 0.0, 255.0));
            };
            pixel = {blend(pixel.r), blend(pixel.g), blend(pixel.b),
                     static_cast<std::uint8_t>(std::round(output_alpha * 255))};
        }
    }
    return result;
}
} // namespace watermark
