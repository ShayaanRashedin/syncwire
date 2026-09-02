#include "syncwire/common/directory_manifest.hpp"
#include "syncwire/common/directory_sync.hpp"
#include "syncwire/common/file_transfer.hpp"
#include "syncwire/common/frame_io.hpp"
#include "syncwire/common/frame_parser.hpp"
#include "syncwire/common/ping_pong.hpp"
#include "syncwire/common/protocol.hpp"
#include "syncwire/common/sync_codec.hpp"
#include "syncwire/common/tcp_socket.hpp"
#include "syncwire/common/transfer_codec.hpp"
#include "syncwire/common/unique_fd.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
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

void print_frame_error(const syncwire::protocol::FrameIoResult& result) {
    std::cerr << syncwire::protocol::frame_io_status_message(result.status);
    if (result.system_error != 0) {
        std::cerr << ": " << std::strerror(result.system_error);
    }
}

void print_transfer_error(const syncwire::protocol::FileTransferResult& result) {
    std::cerr << syncwire::protocol::file_transfer_status_message(result.status);
    if (result.status == syncwire::protocol::FileTransferStatus::FrameIoError) {
        std::cerr << ": ";
        print_frame_error(result.frame_io);
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

void print_sync_error(const syncwire::protocol::DirectorySyncResult& result) {
    std::cerr << syncwire::protocol::directory_sync_status_message(result.status);
    if (result.status == syncwire::protocol::DirectorySyncStatus::FrameIoError) {
        std::cerr << ": ";
        print_frame_error(result.frame_io);
    }
    if (result.status == syncwire::protocol::DirectorySyncStatus::FileTransferError) {
        std::cerr << ": ";
        print_transfer_error(result.file_transfer);
        return;
    }
    if (result.scan_error.has_value()) {
        std::cerr << ": " << syncwire::directory_scan_error_message(*result.scan_error);
    }
    if (result.codec_error.has_value()) {
        std::cerr << ": "
                  << syncwire::protocol::sync_codec_error_message(*result.codec_error);
    }
    if (result.system_error != 0) {
        std::cerr << ": " << std::strerror(result.system_error);
    }
    std::cerr << '\n';
}

} // namespace

int main(int argc, char* argv[]) {
    std::uint16_t requested_port = 4040U;
    if (argc > 3 || (argc >= 2 && !parse_port(argv[1], requested_port))) {
        std::cerr << "Usage: syncwire-server [port] [destination-directory]\n";
        return 2;
    }
    const std::filesystem::path destination_root = argc == 3 ? argv[2] : "received";

    auto listener_result = syncwire::net::listen_ipv4("127.0.0.1", requested_port);
    if (const auto* error = std::get_if<syncwire::net::TcpError>(&listener_result);
        error != nullptr) {
        print_tcp_error(*error);
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

    const auto received = syncwire::protocol::receive_frame(client.get());
    if (const auto* error = std::get_if<syncwire::protocol::FrameIoResult>(&received);
        error != nullptr) {
        print_frame_error(*error);
        std::cerr << '\n';
        return 1;
    }
    const auto& first_frame = std::get<syncwire::protocol::Frame>(received);

    if (first_frame.header.message_type == syncwire::protocol::MessageType::Ping) {
        const auto exchange = syncwire::protocol::serve_ping_frame(client.get(), first_frame);
        if (!exchange.ok()) {
            std::cerr << syncwire::protocol::ping_pong_status_message(exchange.status) << '\n';
            return 1;
        }
        std::cout << "PING received; PONG sent\n";
        return 0;
    }
    if (first_frame.header.message_type == syncwire::protocol::MessageType::UploadRequest) {
        const auto transfer = syncwire::protocol::receive_file(
            client.get(), first_frame, destination_root);
        if (!transfer.ok()) {
            print_transfer_error(transfer);
            return 1;
        }
        std::cout << "Committed upload of " << transfer.transferred << " bytes to "
                  << destination_root << '\n';
        return 0;
    }
    if (first_frame.header.message_type == syncwire::protocol::MessageType::SyncManifest) {
        const auto sync = syncwire::protocol::receive_directory_sync(
            client.get(), first_frame, destination_root);
        if (!sync.ok()) {
            print_sync_error(sync);
            return 1;
        }
        std::cout << "Directory synchronized: " << sync.completed_uploads << " uploaded, "
                  << sync.unchanged_files << " unchanged, " << sync.server_only_files
                  << " server-only files preserved\n";
        return 0;
    }

    std::cerr << "First frame must be PING, UPLOAD_REQUEST, or SYNC_MANIFEST\n";
    return 1;
}
