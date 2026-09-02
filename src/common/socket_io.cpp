#include "syncwire/common/socket_io.hpp"

#include <cerrno>
#include <cstddef>
#include <sys/socket.h>

namespace syncwire::net {

IoResult send_all(const int fd, const std::span<const std::byte> bytes) noexcept {
    std::size_t sent = 0U;
    while (sent < bytes.size()) {
        const auto result = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            return IoResult{.status = IoStatus::PeerClosed, .transferred = sent};
        }
        if (errno == EINTR) {
            continue;
        }
        return IoResult{
            .status = IoStatus::SystemError,
            .transferred = sent,
            .system_error = errno,
        };
    }

    return IoResult{.status = IoStatus::Complete, .transferred = sent};
}

IoResult recv_exact(const int fd, const std::span<std::byte> bytes) noexcept {
    std::size_t received = 0U;
    while (received < bytes.size()) {
        const auto result = ::recv(fd, bytes.data() + received, bytes.size() - received, 0);
        if (result > 0) {
            received += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            return IoResult{.status = IoStatus::PeerClosed, .transferred = received};
        }
        if (errno == EINTR) {
            continue;
        }
        return IoResult{
            .status = IoStatus::SystemError,
            .transferred = received,
            .system_error = errno,
        };
    }

    return IoResult{.status = IoStatus::Complete, .transferred = received};
}

std::string_view io_status_message(const IoStatus status) noexcept {
    switch (status) {
    case IoStatus::Complete:
        return "I/O completed";
    case IoStatus::PeerClosed:
        return "peer closed the connection";
    case IoStatus::SystemError:
        return "system I/O error";
    }
    return "unknown I/O status";
}

} // namespace syncwire::net

