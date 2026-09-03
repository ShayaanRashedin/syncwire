#pragma once

#include "syncwire/common/unique_fd.hpp"

#include <cstdint>
#include <string_view>
#include <variant>

namespace syncwire::net {

enum class TcpOperation {
    CreateSocket,
    SetSocketOption,
    ParseAddress,
    Bind,
    Listen,
    Accept,
    Connect,
    GetSocketName,
};

struct TcpError {
    TcpOperation operation{TcpOperation::CreateSocket};
    int system_error{0};
};

using TcpSocketResult = std::variant<UniqueFd, TcpError>;
using TcpPortResult = std::variant<std::uint16_t, TcpError>;

[[nodiscard]] TcpSocketResult
listen_ipv4(std::string_view address,
            std::uint16_t port,
            int backlog = 16,
            bool nonblocking = false);

[[nodiscard]] TcpSocketResult connect_ipv4(std::string_view address, std::uint16_t port);
[[nodiscard]] TcpSocketResult accept_one(int listener_fd) noexcept;
[[nodiscard]] TcpPortResult local_port(int socket_fd) noexcept;
[[nodiscard]] std::string_view tcp_operation_message(TcpOperation operation) noexcept;

} // namespace syncwire::net

