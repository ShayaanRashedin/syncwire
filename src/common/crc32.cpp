#include "syncwire/common/crc32.hpp"

#include <cstddef>
#include <cstdint>

namespace syncwire {

void Crc32::update(const std::span<const std::byte> bytes) noexcept {
    for (const auto byte : bytes) {
        state_ ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(byte));
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (state_ & 1U);
            state_ = (state_ >> 1U) ^ (0xEDB8'8320U & mask);
        }
    }
}

std::uint32_t Crc32::value() const noexcept {
    return state_ ^ 0xFFFF'FFFFU;
}

std::uint32_t crc32(const std::span<const std::byte> bytes) noexcept {
    Crc32 checksum;
    checksum.update(bytes);
    return checksum.value();
}

} // namespace syncwire
