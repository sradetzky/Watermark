#include "core/watermark.h"
#include "core/internal.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>

namespace wm = watermark;
namespace {
int failures = 0;
void require(bool condition, const std::string& name) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    if (!condition) { ++failures; }
}
template<class Function>
void require_throw(Function function, const std::string& name) {
    try { function(); require(false, name); }
    catch (const std::exception&) { require(true, name); }
}
std::uint8_t byte(double value) {
    return static_cast<std::uint8_t>(std::clamp(std::round(value), 0.0, 255.0));
}

// Reproducible procedural scenes: smooth color fields, edges, and multiscale texture.
// These are algorithm fixtures, not a substitute for a representative photo corpus.
wm::Image scene(int size, unsigned seed) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<double> distribution(-1, 1);
    wm::Image result(size, size);
    std::vector<double> texture(static_cast<std::size_t>(size) * size);
    for (int spacing : {2, 4, 8, 16, 32, 64}) {
        const int grid_size = size / spacing + 2;
        std::vector<double> grid(static_cast<std::size_t>(grid_size) * grid_size);
        for (auto& value : grid) { value = distribution(random); }
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const int gx = x / spacing, gy = y / spacing;
                const double fx = double(x % spacing) / spacing, fy = double(y % spacing) / spacing;
                const double top = grid[gy * grid_size + gx] * (1 - fx) + grid[gy * grid_size + gx + 1] * fx;
                const double bottom = grid[(gy + 1) * grid_size + gx] * (1 - fx) + grid[(gy + 1) * grid_size + gx + 1] * fx;
                texture[static_cast<std::size_t>(y) * size + x] +=
                    (top * (1 - fy) + bottom * fy) * (9 + 3 * std::log2(spacing));
            }
        }
    }
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double nx = double(x) / size, ny = double(y) / size;
            const double edge = nx > 0.18 && nx < 0.47 && ny > 0.3 && ny < 0.76 ? 24 : 0;
            const double light = 100 + 38 * ny + 20 * std::sin(nx * 13 + seed) + edge +
                                 texture[static_cast<std::size_t>(y) * size + x];
            result.at(x, y) = {byte(light + 18 * nx), byte(light + 10 * ny), byte(light - 12 * nx), 255};
        }
    }
    return result;
}
wm::Image brightness(wm::Image image, double offset) {
    for (auto& pixel : image.pixels) {
        pixel.r = byte(pixel.r + offset); pixel.g = byte(pixel.g + offset); pixel.b = byte(pixel.b + offset);
    }
    return image;
}
wm::Image blur(const wm::Image& image) {
    wm::Image result = image;
    constexpr double kernel[3] = {0.106507, 0.786986, 0.106507};
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            double r = 0, g = 0, b = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto& pixel = image.at(std::clamp(x + dx, 0, image.width - 1),
                                                  std::clamp(y + dy, 0, image.height - 1));
                    const double weight = kernel[dy + 1] * kernel[dx + 1];
                    r += weight * pixel.r; g += weight * pixel.g; b += weight * pixel.b;
                }
            }
            result.at(x, y) = {byte(r), byte(g), byte(b), image.at(x, y).a};
        }
    }
    return result;
}
wm::Image tone(wm::Image image, bool gamma) {
    for (auto& pixel : image.pixels) {
        const auto transform = [&](std::uint8_t value) {
            return byte(gamma ? 255 * std::pow(value / 255.0, 1.15) : (value - 128) * 1.15 + 128);
        };
        pixel.r = transform(pixel.r); pixel.g = transform(pixel.g); pixel.b = transform(pixel.b);
    }
    return image;
}
wm::Image noise(wm::Image image) {
    std::mt19937 random(101);
    std::normal_distribution<double> distribution(0, 2);
    for (auto& pixel : image.pixels) {
        const double delta = distribution(random);
        pixel.r = byte(pixel.r + delta); pixel.g = byte(pixel.g + delta); pixel.b = byte(pixel.b + delta);
    }
    return image;
}
wm::Image aligned_crop(const wm::Image& image) {
    wm::Image result(image.width - 128, image.height - 128);
    for (int y = 0; y < result.height; ++y) {
        for (int x = 0; x < result.width; ++x) { result.at(x, y) = image.at(x + 128, y + 128); }
    }
    return result;
}
wm::Image crop(const wm::Image& image, int left, int top, int width, int height) {
    wm::Image result(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) { result.at(x, y) = image.at(x + left, y + top); }
    }
    return result;
}
void check_detection(const wm::Image& image, const wm::Payload& payload,
                     const wm::Parameters& parameters, const std::string& label) {
    const auto result = wm::detect(image, payload, parameters);
    std::cout << "  " << label << ": " << wm::verdict_name(result.verdict)
              << ", bits=" << result.bit_accuracy.value_or(-1) << ", confidence="
              << result.confidence << ", scale=" << result.scale << '\n';
    require(result.verdict == wm::Verdict::present && result.bit_accuracy == 1.0 &&
            result.hash_matches == true && result.recovered_hash == payload.source_hash, label);
}
class TestFiles {
public:
    TestFiles() {
        root = std::filesystem::absolute(std::filesystem::current_path()) /
            ("selftest-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        if (!std::filesystem::create_directory(root)) { throw std::runtime_error("Cannot create test directory."); }
    }
    ~TestFiles() {
        // The freshly created, absolute child is the only directory this test owns.
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path root;
};

void primitives(const TestFiles& files) {
    wm::detail::Block block{};
    for (int i = 0; i < 64; ++i) { block[i] = 40 + i * 2.1; }
    const auto restored = wm::detail::inverse_dct(wm::detail::forward_dct(block));
    double error = 0;
    for (int i = 0; i < 64; ++i) { error = std::max(error, std::abs(block[i] - restored[i])); }
    require(error < 1e-10, "DCT round trip");
    const std::vector<std::uint8_t> abc = {'a', 'b', 'c'};
    const std::array<std::uint8_t, 32> expected_digest = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    require(wm::detail::sha256(abc) == expected_digest, "BCrypt SHA-256 known vector");
    const wm::Payload payload{0x123456789ABCDEF0ULL};
    const auto bits = payload.bits();
    const auto decoded = wm::detail::decode_payload(bits);
    require(decoded && decoded->source_hash == payload.source_hash, "Payload wire round trip");
    bool rejects_all = true;
    for (std::size_t i = 0; i < bits.size(); ++i) {
        auto corrupted = bits;
        corrupted[i] = !corrupted[i];
        rejects_all = rejects_all && !wm::detail::decode_payload(corrupted);
    }
    require(rejects_all, "CRC/magic reject every single-bit corruption");
    auto rgba = scene(128, 7);
    rgba.at(4, 5).a = 17;
    const auto path = files.root / L"roundtrip-\u00e4-\u6c34.png";
    wm::save_image(rgba, path);
    const auto png = wm::load_image(path);
    bool same = rgba.width == png.width && rgba.height == png.height;
    for (std::size_t i = 0; i < rgba.pixels.size(); ++i) {
        const auto a = rgba.pixels[i], b = png.pixels[i];
        same = same && a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }
    require(same, "WIC PNG lossless RGBA and Unicode path");
    require_throw([] { wm::Image invalid(0, 128); }, "Reject invalid dimensions");
    require_throw([&] { rgba.at(128, 0); }, "Reject pixel coordinates crossing a row boundary");
    require_throw([&] { rgba.at(-1, 1); }, "Reject negative pixel coordinates");
    require_throw([&] { wm::embed(rgba, payload, {}); }, "Reject empty passphrase");
    require_throw([&] { wm::embed(wm::Image(127, 128), payload, {"key"}); }, "Reject undersized embed");
    require_throw([&] { wm::load_image(files.root / "missing.png"); }, "Report missing input");
    require_throw([&] { wm::save_image(rgba, files.root / "missing" / "out.png"); }, "Report unwritable output");
    require_throw([&] { wm::save_image(rgba, files.root / "bad.jpg", wm::ImageFormat::jpeg, 101); }, "Reject invalid JPEG quality");
}
void robustness(const TestFiles& files) {
    const auto source = scene(128, 91);
    const auto payload = wm::payload_from_source(source);
    require(payload.source_hash == wm::payload_from_source(source).source_hash, "Source hash deterministic");
    require(payload.source_hash != wm::payload_from_source(scene(128, 92)).source_hash, "Distinct source fixtures differ");
    const wm::Parameters parameters{"selftest passphrase \xC3\xA4"};
    wm::Parameters wrong_key{"wrong passphrase"};
    const wm::Payload other_payload{payload.source_hash ^ 1};
    wm::Image flat(512, 512);
    for (auto& pixel : flat.pixels) { pixel = {128, 128, 128, 255}; }
    require(wm::detect(flat, payload, parameters).verdict == wm::Verdict::absent, "Unmarked flat image absent");
    check_detection(wm::embed(flat, payload, parameters), payload, parameters, "Flat image PNG exact");
    for (unsigned seed : {13, 29, 47}) {
        const int size = seed == 47 ? 768 : 512;
        const auto host = scene(size, seed);
        const auto prefix = "scene " + std::to_string(seed) + " ";
        require(wm::detect(host, payload, parameters).verdict == wm::Verdict::absent, prefix + "unmarked absent");
        const auto marked = wm::embed(host, payload, parameters);
        const auto quality = wm::psnr(host, marked);
        std::cout << "  " << prefix << "PSNR=" << quality << " dB\n";
        require(quality >= 40, prefix + "default PSNR >= 40 dB");
        const auto png_path = files.root / ("marked-" + std::to_string(seed) + ".png");
        wm::save_image(marked, png_path);
        check_detection(wm::load_image(png_path), payload, parameters, prefix + "PNG exact");
        const auto blind = wm::detect(marked, std::nullopt, parameters);
        require(blind.verdict == wm::Verdict::present && blind.recovered_hash == payload.source_hash &&
                !blind.bit_accuracy && !blind.hash_matches, prefix + "blind payload recovery");
        require(wm::detect(marked, payload, wrong_key).verdict == wm::Verdict::absent, prefix + "wrong key absent");
        const auto mismatch = wm::detect(marked, other_payload, parameters);
        require(mismatch.verdict == wm::Verdict::present && mismatch.hash_matches == false &&
                mismatch.recovered_hash == payload.source_hash, prefix + "wrong source reports mismatch");
        for (int jpeg_quality : {70, 50}) {
            const auto jpeg_path = files.root / ("marked-" + std::to_string(seed) + "-" + std::to_string(jpeg_quality) + ".jpg");
            wm::save_image(marked, jpeg_path, wm::ImageFormat::jpeg, jpeg_quality);
            check_detection(wm::load_image(jpeg_path), payload, parameters, prefix + "JPEG " + std::to_string(jpeg_quality));
        }
        for (double scale : {0.75, 1.5}) {
            check_detection(wm::resize_image(marked, static_cast<int>(size * scale), static_cast<int>(size * scale)),
                            payload, parameters, prefix + "resize " + std::to_string(scale));
        }
        for (double offset : {-38.0, 38.0}) {
            check_detection(brightness(marked, offset), payload, parameters, prefix + "brightness " + std::to_string(offset));
        }
        check_detection(blur(marked), payload, parameters, prefix + "Gaussian blur sigma 0.5");
        check_detection(tone(marked, false), payload, parameters, prefix + "contrast +15 percent");
        check_detection(tone(marked, true), payload, parameters, prefix + "gamma 1.15");
        check_detection(noise(marked), payload, parameters, prefix + "Gaussian noise sigma 2");
        check_detection(aligned_crop(marked), payload, parameters, prefix + "tile-aligned crop");
        const auto twice_path = files.root / ("twice-" + std::to_string(seed) + ".jpg");
        wm::save_image(marked, twice_path, wm::ImageFormat::jpeg, 70);
        const auto first_jpeg = wm::load_image(twice_path);
        wm::save_image(first_jpeg, twice_path, wm::ImageFormat::jpeg, 50);
        check_detection(wm::load_image(twice_path), payload, parameters, prefix + "JPEG 70 then 50");
        if (seed == 13) {
            for (const auto offset : {std::pair{37, 23}, std::pair{127, 99}, std::pair{8, 8}, std::pair{0, 27}}) {
                const auto cropped = crop(marked, offset.first, offset.second, 384, 384);
                const auto result = wm::detect(cropped, std::nullopt, parameters);
                std::cout << "  crop " << offset.first << ',' << offset.second << ": " << wm::verdict_name(result.verdict)
                          << " phase=" << result.offset_x << ',' << result.offset_y << '\n';
                require(result.verdict == wm::Verdict::present && result.recovered_hash == payload.source_hash &&
                        result.offset_x == offset.first && result.offset_y == offset.second,
                        prefix + "blind crop offset " + std::to_string(offset.first) + "," + std::to_string(offset.second));
            }
            const auto arbitrary_crop = crop(marked, 37, 23, 384, 384);
            const auto crop_jpeg = files.root / "cropped.jpg";
            wm::save_image(arbitrary_crop, crop_jpeg, wm::ImageFormat::jpeg, 70);
            check_detection(wm::load_image(crop_jpeg), payload, parameters, prefix + "arbitrary crop then JPEG 70");
            require(wm::detect(arbitrary_crop, payload, wrong_key).verdict == wm::Verdict::absent,
                    prefix + "cropped image wrong key absent");
            const auto cropped_mismatch = wm::detect(arbitrary_crop, other_payload, parameters);
            require(cropped_mismatch.verdict == wm::Verdict::present && cropped_mismatch.hash_matches == false &&
                    cropped_mismatch.recovered_hash == payload.source_hash, prefix + "crop recovery independent of expected hash");
            auto rectangle = crop(host, 0, 0, 509, 487);
            rectangle = wm::embed(rectangle, payload, parameters);
            for (double scale : {0.80, 0.85, 0.95, 1.05, 1.15, 1.30, 1.45}) {
                check_detection(wm::resize_image(rectangle, static_cast<int>(std::round(509 * scale)),
                                static_cast<int>(std::round(487 * scale))), payload, parameters,
                                prefix + "rectangular rounded resize " + std::to_string(scale));
            }
            auto cancelled = parameters;
            cancelled.cancelled = [] { return true; };
            require_throw([&] { wm::detect(marked, payload, cancelled); }, "Detection cancellation");
            require_throw([&] { wm::embed(host, payload, cancelled); }, "Embedding cancellation");
        }
    }
}
}
int main() {
    std::cout << std::fixed << std::setprecision(4);
    try {
        TestFiles files;
        primitives(files);
        robustness(files);
    } catch (const std::exception& error) {
        std::cerr << "Self-test error: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1 : 0;
}
