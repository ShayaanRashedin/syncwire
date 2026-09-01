#include "syncwire/common/ping_pong.hpp"
#include "syncwire/common/tcp_socket.hpp"
#include "syncwire/common/unique_fd.hpp"

#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace {

template <typename Integer>
[[nodiscard]] bool parse_unsigned(const std::string_view text, Integer& output) noexcept {
    Integer value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return false;
    }
    output = value;
    return true;
}

void print_tcp_error(const syncwire::net::TcpError& error) {
    std::cerr << "Failed to " << syncwire::net::tcp_operation_message(error.operation);
    if (error.system_error != 0) {
        std::cerr << ": " << std::strerror(error.system_error);
    }
    std::cerr << '\n';
}

void print_exchange_error(const syncwire::protocol::PingPongResult& result) {
    std::cerr << syncwire::protocol::ping_pong_status_message(result.status);
    if (result.status == syncwire::protocol::PingPongStatus::FrameIoError) {
        std::cerr << ": "
                  << syncwire::protocol::frame_io_status_message(result.frame_io.status);
        if (result.frame_io.system_error != 0) {
            std::cerr << ": " << std::strerror(result.frame_io.system_error);
        }
    } else if (result.status == syncwire::protocol::PingPongStatus::MismatchedRequestId) {
        std::cerr << " (expected " << result.expected_request_id << ", received "
                  << result.actual_request_id << ')';
    }
    std::cerr << '\n';
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: syncwire-client <IPv4 address> <port> [request-id]\n";
        return 2;
    }

    unsigned port_value = 0U;
    std::uint64_t request_id = 1U;
    if (!parse_unsigned(std::string_view(argv[2]), port_value) || port_value == 0U ||
        port_value > 65'535U ||
        (argc == 4 && (!parse_unsigned(std::string_view(argv[3]), request_id) || request_id == 0U))) {
        std::cerr << "Port must be 1-65535 and request ID must be nonzero\n";
        return 2;
    }

    auto socket_result = syncwire::net::connect_ipv4(
        argv[1], static_cast<std::uint16_t>(port_value));
    if (const auto* error = std::get_if<syncwire::net::TcpError>(&socket_result);
        error != nullptr) {
        print_tcp_error(*error);
        return 1;
    }
    auto socket = std::get<syncwire::UniqueFd>(std::move(socket_result));

    const auto exchange = syncwire::protocol::perform_ping(socket.get(), request_id);
    if (!exchange.ok()) {
        print_exchange_error(exchange);
        return 1;
    }

    std::cout << "PONG received for request " << request_id << '\n';
    return 0;
}

