#include "core/internal.h"

#include <stdexcept>

namespace watermark::detail {
namespace {
class KeyStream {
public:
    explicit KeyStream(std::string_view key) {
        constexpr std::string_view domain = "Watermark/v1/pattern";
        prefix_.insert(prefix_.end(), domain.begin(), domain.end());
        for (int i = 0; i < 8; ++i) { prefix_.push_back(static_cast<std::uint8_t>(key.size() >> (i * 8))); }
        prefix_.insert(prefix_.end(), key.begin(), key.end());
    }
    std::uint32_t next() {
        if (position_ == block_.size()) {
            auto input = prefix_;
            for (int i = 0; i < 8; ++i) { input.push_back(static_cast<std::uint8_t>(counter_ >> (i * 8))); }
            ++counter_;
            block_ = sha256(input);
            position_ = 0;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) { value = (value << 8) | block_[position_++]; }
        return value;
    }
private:
    std::vector<std::uint8_t> prefix_;
    std::array<std::uint8_t, 32> block_{};
    std::size_t position_ = 32;
    std::uint64_t counter_ = 0;
};
}
Pattern make_pattern(std::string_view passphrase) {
    KeyStream stream(passphrase);
    Pattern result{};
    for (int i = 0; i < pattern_size; ++i) { result[i].bit = i % payload_bit_count; }
    for (int i = pattern_size - 1; i > 0; --i) {
        std::swap(result[i], result[stream.next() % (i + 1)]);
    }
    for (auto& chip : result) { chip.sign = (stream.next() & 1) ? 1.0 : -1.0; }
    return result;
}
void validate_parameters(const Parameters& parameters) {
    if (parameters.passphrase.empty()) { throw std::invalid_argument("A nonempty passphrase is required."); }
}
void check_cancelled(const Parameters& parameters) {
    if (parameters.cancelled && parameters.cancelled()) { throw OperationCancelled(); }
}
} // namespace watermark::detail
