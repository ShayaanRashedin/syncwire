#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace syncwire::net {

enum class IoStatus {
    Complete,
    PeerClosed,
    SystemError,
};

struct IoResult {
    IoStatus status{IoStatus::Complete};
    std::size_t transferred{0U};
    int system_error{0};

    [[nodiscard]] bool ok() const noexcept {
        return status == IoStatus::Complete;
    }
};

[[nodiscard]] IoResult send_all(int fd, std::span<const std::byte> bytes) noexcept;
[[nodiscard]] IoResult recv_exact(int fd, std::span<std::byte> bytes) noexcept;
[[nodiscard]] std::string_view io_status_message(IoStatus status) noexcept;

} // namespace syncwire::net

