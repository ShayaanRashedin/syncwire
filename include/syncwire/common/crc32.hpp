#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace syncwire {

class Crc32 {
public:
    void update(std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] std::uint32_t value() const noexcept;

private:
    std::uint32_t state_{0xFFFF'FFFFU};
};

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;

} // namespace syncwire
