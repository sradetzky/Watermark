#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace watermark {

struct Pixel {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<Pixel> pixels;

    Image() = default;
    Image(int width, int height);
    void validate() const;
    Pixel& at(int x, int y);
    const Pixel& at(int x, int y) const;
};

enum class ImageFormat { png, jpeg };
Image load_image(const std::filesystem::path& path);
void save_image(const Image& image, const std::filesystem::path& path,
                ImageFormat format = ImageFormat::png, int jpeg_quality = 90);
Image resize_image(const Image& image, int width, int height);
double psnr(const Image& original, const Image& modified);

enum class Position { bottom_right, bottom_left, top_right, top_left, center };
enum class VisibleInk { black, white };
struct VisibleWatermark {
    Position position = Position::bottom_right;
    int size_percent = 15; // Longest edge, relative to the host's short side.
    int opacity_percent = 50;
    VisibleInk ink = VisibleInk::black;
};
// The transparent image's alpha defines the silhouette; RGB is ignored.
Image apply_visible_watermark(const Image& host, const Image& silhouette,
                              const VisibleWatermark& options,
                              const std::function<bool()>& cancelled = {});

constexpr std::size_t payload_bit_count = 96;
struct Payload {
    std::uint64_t source_hash = 0;
    std::array<bool, payload_bit_count> bits() const;
};
// Source-key mode composites alpha onto white; false preserves legacy hashing.
Payload payload_from_source(const Image& source, bool composite_alpha = false);

enum class Strength { low, normal, high };
struct Parameters {
    std::string passphrase; // UTF-8 bytes; exact bytes must match on detection.
    Strength strength = Strength::normal;
    bool search_scales = true;
    bool search_alignment = true;
    std::function<bool()> cancelled;
};
class OperationCancelled : public std::runtime_error {
public:
    OperationCancelled() : std::runtime_error("Operation cancelled.") {}
};

enum class Verdict { absent, weak, present };
const char* verdict_name(Verdict verdict);
struct DetectionResult {
    Verdict verdict = Verdict::absent;
    double confidence = 0; // Signal margin, not a calibrated probability.
    std::optional<double> bit_accuracy; // Only defined when an expected payload is supplied.
    std::optional<std::uint64_t> recovered_hash; // Only populated after magic and CRC validate.
    std::optional<bool> hash_matches;
    double scale = 1; // Resampling factor applied by the detector.
    int offset_x = 0; // Original tile phase of the observed crop origin, in pixels.
    int offset_y = 0;
};

Image embed(const Image& host, const Payload& payload, const Parameters& parameters);
DetectionResult detect(const Image& image, const std::optional<Payload>& expected,
                       const Parameters& parameters);

} // namespace watermark
