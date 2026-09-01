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

[[nodiscard]] bool parse_port(const std::string_view text, std::uint16_t& port) noexcept {
    unsigned value = 0U;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value > 65'535U) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
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
    }
    std::cerr << '\n';
}

} // namespace

int main(int argc, char* argv[]) {
    std::uint16_t requested_port = 4040U;
    if (argc > 2 || (argc == 2 && !parse_port(argv[1], requested_port))) {
        std::cerr << "Usage: syncwire-server [port]\n";
        return 2;
    }

    auto listener_result = syncwire::net::listen_ipv4("127.0.0.1", requested_port);
    const auto* listener_error = std::get_if<syncwire::net::TcpError>(&listener_result);
    if (listener_error != nullptr) {
        print_tcp_error(*listener_error);
        return 1;
    }
    auto listener = std::get<syncwire::UniqueFd>(std::move(listener_result));

    const auto port_result = syncwire::net::local_port(listener.get());
    if (const auto* error = std::get_if<syncwire::net::TcpError>(&port_result); error != nullptr) {
        print_tcp_error(*error);
        return 1;
    }
    std::cout << "SyncWire server listening on 127.0.0.1:"
              << std::get<std::uint16_t>(port_result) << '\n'
              << std::flush;

    auto client_result = syncwire::net::accept_one(listener.get());
    if (const auto* error = std::get_if<syncwire::net::TcpError>(&client_result);
        error != nullptr) {
        print_tcp_error(*error);
        return 1;
    }
    auto client = std::get<syncwire::UniqueFd>(std::move(client_result));

    const auto exchange = syncwire::protocol::serve_ping_once(client.get());
    if (!exchange.ok()) {
        print_exchange_error(exchange);
        return 1;
    }

    std::cout << "PING received; PONG sent\n";
    return 0;
}

