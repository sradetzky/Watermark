#include "core/internal.h"

#include <algorithm>
#include <cmath>

namespace watermark {
namespace {
struct Votes {
    std::array<double, payload_bit_count> sums{}, energy{}, counts{};
    void add(const detail::Chip& chip, double sum, double squared, double count) {
        sums[chip.bit] += sum * chip.sign;
        energy[chip.bit] += squared;
        counts[chip.bit] += count;
    }
    double magic_score() const {
        const auto magic = Payload{}.bits();
        double score = 0;
        for (int i = 64; i < 80; ++i) {
            if (energy[i] > 0) {
                score += sums[i] * (magic[i] ? 1 : -1) / std::sqrt(energy[i] * counts[i]);
            }
        }
        return score / 16;
    }
    DetectionResult decode(const std::optional<Payload>& expected) const {
        std::array<bool, payload_bit_count> bits{};
        double margin = 0;
        for (std::size_t i = 0; i < bits.size(); ++i) {
            bits[i] = sums[i] > 0;
            if (energy[i] > 0) { margin += std::abs(sums[i]) / std::sqrt(energy[i] * counts[i]); }
        }
        DetectionResult result;
        result.confidence = margin / payload_bit_count;
        if (expected) {
            const auto expected_bits = expected->bits();
            std::size_t matching = 0;
            for (std::size_t i = 0; i < bits.size(); ++i) { matching += bits[i] == expected_bits[i] ? 1 : 0; }
            result.bit_accuracy = static_cast<double>(matching) / payload_bit_count;
        }
        const auto payload = detail::decode_payload(bits);
        if (payload) {
            result.recovered_hash = payload->source_hash;
            if (expected) { result.hash_matches = payload->source_hash == expected->source_hash; }
            if (result.confidence >= 0.08) {
                result.verdict = result.confidence >= 0.18 ? Verdict::present : Verdict::weak;
            }
        }
        return result;
    }
};
Votes aligned_votes(const Image& image, const detail::Pattern& pattern, const Parameters& parameters,
                    int pixel_x = 0, int pixel_y = 0, int block_x = 0, int block_y = 0) {
    Votes votes;
    for (int y = pixel_y; y + 8 <= image.height; y += 8) {
        detail::check_cancelled(parameters);
        for (int x = pixel_x; x + 8 <= image.width; x += 8) {
            const auto coefficients = detail::forward_dct(detail::read_block(image, x, y));
            const int block = ((y / 8 + block_y) % 16) * 16 + (x / 8 + block_x) % 16;
            for (int c = 0; c < detail::coefficient_count; ++c) {
                const double value = std::clamp(coefficients[detail::coefficient_indices[c]], -30.0, 30.0);
                votes.add(pattern[block * detail::coefficient_count + c], value, value * value, 1);
            }
        }
    }
    return votes;
}

// Cache separable horizontal DCT filters across the 64 pixel-grid phases.
// Bound the synchronization sample; final CRC verification uses the full image.
class PhaseSample {
public:
    PhaseSample(const Image& image, const Parameters& parameters)
        : width_(std::min(512, image.width)), height_(std::min(512, image.height)),
          stride_(width_ - 7), horizontal_(static_cast<std::size_t>(stride_) * height_ * 5) {
        const auto& basis = detail::dct_basis();
        for (int y = 0; y < height_; ++y) {
            detail::check_cancelled(parameters);
            for (int x = 0; x < stride_; ++x) {
                for (int u = 0; u < 5; ++u) {
                    double value = 0;
                    for (int dx = 0; dx < 8; ++dx) { value += detail::luminance(image.at(x + dx, y)) * basis[u][dx]; }
                    horizontal_[(static_cast<std::size_t>(y) * stride_ + x) * 5 + u] = value;
                }
            }
        }
    }
    struct Bin { double sum = 0, energy = 0, count = 0; };
    using Bins = std::array<Bin, detail::pattern_size>;
    Bins bins(int px, int py) const {
        Bins result{};
        const auto& basis = detail::dct_basis();
        for (int y = py; y + 8 <= height_; y += 8) {
            for (int x = px; x + 8 <= width_; x += 8) {
                const int block = ((y / 8) % 16) * 16 + (x / 8) % 16;
                for (int c = 0; c < detail::coefficient_count; ++c) {
                    const int u = detail::coefficient_indices[c] % 8, v = detail::coefficient_indices[c] / 8;
                    double value = 0;
                    for (int dy = 0; dy < 8; ++dy) {
                        value += horizontal_[(static_cast<std::size_t>(y + dy) * stride_ + x) * 5 + u] * basis[v][dy];
                    }
                    value = std::clamp(value, -30.0, 30.0);
                    auto& bin = result[block * detail::coefficient_count + c];
                    bin.sum += value; bin.energy += value * value; bin.count += 1;
                }
            }
        }
        return result;
    }
private:
    int width_, height_, stride_;
    std::vector<double> horizontal_;
};
Votes magic_votes(const PhaseSample::Bins& bins, const detail::Pattern& pattern, int bx, int by) {
    Votes votes;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int shifted = ((y + by) % 16) * 16 + (x + bx) % 16;
            for (int c = 0; c < detail::coefficient_count; ++c) {
                const auto chip = pattern[shifted * detail::coefficient_count + c];
                if (chip.bit < 64 || chip.bit >= 80) { continue; }
                const auto& bin = bins[(y * 16 + x) * detail::coefficient_count + c];
                votes.add(chip, bin.sum, bin.energy, bin.count);
            }
        }
    }
    return votes;
}
std::optional<DetectionResult> search_phase(const Image& image, const std::optional<Payload>& expected,
                                           const detail::Pattern& pattern, const Parameters& parameters) {
    if (image.width < 128 || image.height < 128) { return {}; }
    PhaseSample sample(image, parameters);
    struct Candidate { double score; int x, y; };
    std::optional<DetectionResult> best;
    for (int py = 0; py < 8; ++py) {
        for (int px = 0; px < 8; ++px) {
            detail::check_cancelled(parameters);
            const auto bins = sample.bins(px, py);
            std::array<Candidate, 256> candidates{};
            for (int by = 0; by < 16; ++by) {
                for (int bx = 0; bx < 16; ++bx) {
                    candidates[by * 16 + bx] = {magic_votes(bins, pattern, bx, by).magic_score(), bx, by};
                }
            }
            std::partial_sort(candidates.begin(), candidates.begin() + 8, candidates.end(),
                              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
            for (int i = 0; i < 8; ++i) {
                const auto candidate = candidates[i];
                if (candidate.score < 0.10) { continue; }
                const auto sample_votes = magic_votes(bins, pattern, candidate.x, candidate.y);
                const auto magic = Payload{}.bits();
                int matching = 0;
                for (int bit = 64; bit < 80; ++bit) { matching += (sample_votes.sums[bit] > 0) == magic[bit] ? 1 : 0; }
                if (matching < 15) { continue; }
                // Magic ranks synchronization; it never repairs payload bits.
                auto result = aligned_votes(image, pattern, parameters, px, py, candidate.x, candidate.y).decode(expected);
                if (result.verdict != Verdict::absent) {
                    result.offset_x = (candidate.x * 8 - px + 128) % 128;
                    result.offset_y = (candidate.y * 8 - py + 128) % 128;
                    if (!best || result.confidence > best->confidence) { best = result; }
                }
            }
        }
    }
    return best;
}
}
const char* verdict_name(Verdict verdict) {
    switch (verdict) {
    case Verdict::present: return "present";
    case Verdict::weak: return "weak";
    default: return "absent";
    }
}
DetectionResult detect(const Image& image, const std::optional<Payload>& expected, const Parameters& parameters) {
    image.validate();
    detail::validate_parameters(parameters);
    detail::check_cancelled(parameters);
    const auto pattern = detail::make_pattern(parameters.passphrase);
    auto best = aligned_votes(image, pattern, parameters).decode(expected);
    if (best.verdict != Verdict::absent) { return best; }
    if (parameters.search_scales) {
        struct ScaleCandidate { double score; int width, height; };
        std::vector<ScaleCandidate> candidates;
        constexpr std::array<int, 15> percentages = {75, 150, 125, 80, 110, 90, 85, 95, 105, 115, 120, 130, 135, 140, 145};
        for (const int percentage : percentages) {
            detail::check_cancelled(parameters);
            const int width = static_cast<int>(std::round(image.width * 100.0 / percentage));
            const int height = static_cast<int>(std::round(image.height * 100.0 / percentage));
            if (width < 128 || height < 128) { continue; }
            const auto votes = aligned_votes(resize_image(image, width, height), pattern, parameters);
            auto result = votes.decode(expected);
            result.scale = double(width) / image.width;
            if (result.verdict != Verdict::absent) { return result; }
            candidates.push_back({votes.magic_score(), width, height});
            if (result.confidence > best.confidence) { best = result; }
        }
        std::sort(candidates.begin(), candidates.end(), [](const ScaleCandidate& a, const ScaleCandidate& b) {
            return a.score > b.score;
        });
        // Undo independent dimension rounding around the three best magic correlations.
        for (std::size_t i = 0; i < std::min<std::size_t>(3, candidates.size()); ++i) {
            const auto candidate = candidates[i];
            if (candidate.score < 0.10) { continue; }
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((!dx && !dy) || candidate.width + dx < 128 || candidate.height + dy < 128) { continue; }
                    detail::check_cancelled(parameters);
                    auto result = aligned_votes(resize_image(image, candidate.width + dx, candidate.height + dy),
                                                pattern, parameters).decode(expected);
                    result.scale = double(candidate.width + dx) / image.width;
                    if (result.verdict != Verdict::absent) { return result; }
                }
            }
        }
    }
    if (parameters.search_alignment) {
        if (auto result = search_phase(image, expected, pattern, parameters)) { return *result; }
    }
    return best;
}
} // namespace watermark
