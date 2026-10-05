#include "core/internal.h"

#include <Windows.h>
#include <bcrypt.h>
#include <limits>
#include <stdexcept>

namespace watermark::detail {
namespace {
void check(NTSTATUS status) {
    if (status < 0) { throw std::runtime_error("BCrypt SHA-256 operation failed."); }
}
class Algorithm {
public:
    Algorithm() { check(BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0)); }
    ~Algorithm() { BCryptCloseAlgorithmProvider(handle, 0); }
    BCRYPT_ALG_HANDLE handle = nullptr;
};
class Hash {
public:
    ~Hash() { if (handle) { BCryptDestroyHash(handle); } }
    BCRYPT_HASH_HANDLE handle = nullptr;
};
}
std::array<std::uint8_t, 32> sha256(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() > std::numeric_limits<ULONG>::max()) { throw std::invalid_argument("SHA-256 input too large."); }
    Algorithm algorithm;
    DWORD object_size = 0, copied = 0;
    check(BCryptGetProperty(algorithm.handle, BCRYPT_OBJECT_LENGTH,
          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0));
    std::vector<std::uint8_t> object(object_size);
    Hash hash;
    check(BCryptCreateHash(algorithm.handle, &hash.handle, object.data(), object_size, nullptr, 0, 0));
    check(BCryptHashData(hash.handle, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0));
    std::array<std::uint8_t, 32> result{};
    check(BCryptFinishHash(hash.handle, result.data(), static_cast<ULONG>(result.size()), 0));
    return result;
}
} // namespace watermark::detail
