#include "syncwire/common/file_transfer.hpp"
#include "syncwire/common/ping_pong.hpp"
#include "syncwire/common/tcp_socket.hpp"
#include "syncwire/common/transfer_codec.hpp"
#include "syncwire/common/unique_fd.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
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

void print_usage() {
    std::cerr << "Usage:\n"
              << "  syncwire-client <IPv4 address> <port> ping [request-id]\n"
              << "  syncwire-client <IPv4 address> <port> upload <source-path> "
                 "[remote-name] [request-id]\n";
}

void print_tcp_error(const syncwire::net::TcpError& error) {
    std::cerr << "Failed to " << syncwire::net::tcp_operation_message(error.operation);
    if (error.system_error != 0) {
        std::cerr << ": " << std::strerror(error.system_error);
    }
    std::cerr << '\n';
}

void print_frame_error(const syncwire::protocol::FrameIoResult& result) {
    std::cerr << syncwire::protocol::frame_io_status_message(result.status);
    if (result.system_error != 0) {
        std::cerr << ": " << std::strerror(result.system_error);
    }
}

void print_exchange_error(const syncwire::protocol::PingPongResult& result) {
    std::cerr << syncwire::protocol::ping_pong_status_message(result.status);
    if (result.status == syncwire::protocol::PingPongStatus::FrameIoError) {
        std::cerr << ": ";
        print_frame_error(result.frame_io);
    } else if (result.status == syncwire::protocol::PingPongStatus::MismatchedRequestId) {
        std::cerr << " (expected " << result.expected_request_id << ", received "
                  << result.actual_request_id << ')';
    }
    std::cerr << '\n';
}

void print_transfer_error(const syncwire::protocol::FileTransferResult& result) {
    std::cerr << syncwire::protocol::file_transfer_status_message(result.status);
    if (result.status == syncwire::protocol::FileTransferStatus::FrameIoError) {
        std::cerr << ": ";
        print_frame_error(result.frame_io);
    }
    if (result.remote_code.has_value()) {
        std::cerr << ": "
                  << syncwire::protocol::transfer_result_code_message(*result.remote_code);
    }
    if (result.codec_error.has_value()) {
        std::cerr << ": "
                  << syncwire::protocol::transfer_codec_error_message(*result.codec_error);
    }
    if (result.system_error != 0) {
        std::cerr << ": " << std::strerror(result.system_error);
    }
    std::cerr << '\n';
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 4) {
        print_usage();
        return 2;
    }

    unsigned port_value = 0U;
    if (!parse_unsigned(std::string_view(argv[2]), port_value) || port_value == 0U ||
        port_value > 65'535U) {
        std::cerr << "Port must be in the range 1-65535\n";
        return 2;
    }

    const std::string_view command(argv[3]);
    std::uint64_t request_id = 1U;
    std::filesystem::path source_path;
    std::string remote_name;
    if (command == "ping") {
        if (argc > 5 ||
            (argc == 5 &&
             (!parse_unsigned(std::string_view(argv[4]), request_id) || request_id == 0U))) {
            print_usage();
            return 2;
        }
    } else if (command == "upload") {
        if (argc < 5 || argc > 7) {
            print_usage();
            return 2;
        }
        source_path = argv[4];
        remote_name = argc >= 6 ? argv[5] : source_path.filename().string();
        if (argc == 7 &&
            (!parse_unsigned(std::string_view(argv[6]), request_id) || request_id == 0U)) {
            std::cerr << "Request ID must be nonzero\n";
            return 2;
        }
        if (!syncwire::protocol::is_safe_remote_filename(remote_name)) {
            std::cerr << "Remote name must be a plain filename without path separators\n";
            return 2;
        }
    } else {
        print_usage();
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

    if (command == "ping") {
        const auto exchange = syncwire::protocol::perform_ping(socket.get(), request_id);
        if (!exchange.ok()) {
            print_exchange_error(exchange);
            return 1;
        }
        std::cout << "PONG received for request " << request_id << '\n';
        return 0;
    }

    const auto transfer = syncwire::protocol::send_file(
        socket.get(), source_path, remote_name, request_id);
    if (!transfer.ok()) {
        print_transfer_error(transfer);
        return 1;
    }
    std::cout << "Uploaded " << transfer.transferred << " bytes as " << remote_name << '\n';
    return 0;
}
