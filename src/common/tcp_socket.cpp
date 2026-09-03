#include "syncwire/common/tcp_socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <utility>

namespace syncwire::net {

TcpSocketResult listen_ipv4(const std::string_view address, const std::uint16_t port,
                            const int backlog, const bool nonblocking) {
    const int socket_flags =
        SOCK_STREAM | SOCK_CLOEXEC | (nonblocking ? SOCK_NONBLOCK : 0);
    UniqueFd listener(::socket(AF_INET, socket_flags, 0));
    if (!listener.valid()) {
        return TcpError{.operation = TcpOperation::CreateSocket, .system_error = errno};
    }

    const int enabled = 1;
    if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        return TcpError{.operation = TcpOperation::SetSocketOption, .system_error = errno};
    }

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    const std::string address_string(address);
    const int parsed = ::inet_pton(AF_INET, address_string.c_str(), &endpoint.sin_addr);
    if (parsed != 1) {
        return TcpError{
            .operation = TcpOperation::ParseAddress,
            .system_error = parsed < 0 ? errno : 0,
        };
    }

    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) < 0) {
        return TcpError{.operation = TcpOperation::Bind, .system_error = errno};
    }
    if (::listen(listener.get(), backlog) < 0) {
        return TcpError{.operation = TcpOperation::Listen, .system_error = errno};
    }

    return std::move(listener);
}

TcpSocketResult connect_ipv4(const std::string_view address, const std::uint16_t port) {
    UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket.valid()) {
        return TcpError{.operation = TcpOperation::CreateSocket, .system_error = errno};
    }

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    const std::string address_string(address);
    const int parsed = ::inet_pton(AF_INET, address_string.c_str(), &endpoint.sin_addr);
    if (parsed != 1) {
        return TcpError{
            .operation = TcpOperation::ParseAddress,
            .system_error = parsed < 0 ? errno : 0,
        };
    }

    if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) < 0) {
        return TcpError{.operation = TcpOperation::Connect, .system_error = errno};
    }
    return std::move(socket);
}

TcpSocketResult accept_one(const int listener_fd) noexcept {
    while (true) {
        const int client_fd = ::accept4(listener_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client_fd >= 0) {
            return UniqueFd(client_fd);
        }
        if (errno != EINTR) {
            return TcpError{.operation = TcpOperation::Accept, .system_error = errno};
        }
    }
}

TcpPortResult local_port(const int socket_fd) noexcept {
    sockaddr_in endpoint{};
    socklen_t endpoint_size = sizeof(endpoint);
    if (::getsockname(socket_fd, reinterpret_cast<sockaddr*>(&endpoint), &endpoint_size) < 0) {
        return TcpError{.operation = TcpOperation::GetSocketName, .system_error = errno};
    }
    return ntohs(endpoint.sin_port);
}

std::string_view tcp_operation_message(const TcpOperation operation) noexcept {
    switch (operation) {
    case TcpOperation::CreateSocket:
        return "create socket";
    case TcpOperation::SetSocketOption:
        return "set socket option";
    case TcpOperation::ParseAddress:
        return "parse IPv4 address";
    case TcpOperation::Bind:
        return "bind socket";
    case TcpOperation::Listen:
        return "listen on socket";
    case TcpOperation::Accept:
        return "accept client";
    case TcpOperation::Connect:
        return "connect to server";
    case TcpOperation::GetSocketName:
        return "read local socket address";
    }
    return "unknown TCP operation";
}

} // namespace syncwire::net

