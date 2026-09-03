#include "syncwire/common/authentication.hpp"
#include "syncwire/common/directory_manifest.hpp"
#include "syncwire/common/directory_sync.hpp"
#include "syncwire/common/file_transfer.hpp"
#include "syncwire/common/ping_pong.hpp"
#include "syncwire/common/sync_codec.hpp"
#include "syncwire/common/tcp_socket.hpp"
#include "syncwire/common/transfer_codec.hpp"
#include "syncwire/common/unique_fd.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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
                 "[remote-name] [request-id]\n"
              << "  syncwire-client <IPv4 address> <port> sync <source-directory> "
                 "[request-id]\n"
              << "Append --retries <0-5> to retry transport failures (default: 2).\n";
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

void print_authentication_error(
    const syncwire::protocol::AuthenticationResult& result) {
    std::cerr
        << syncwire::protocol::authentication_status_message(result.status);
    if (result.status ==
        syncwire::protocol::AuthenticationStatus::FrameIoError) {
        std::cerr << ": ";
        print_frame_error(result.frame_io);
    }
    std::cerr << '\n';
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
    unsigned retries = 2U;
    if (argc >= 3 && std::string_view(argv[argc - 2]) == "--retries") {
        if (!parse_unsigned(std::string_view(argv[argc - 1]), retries) || retries > 5U) {
            std::cerr << "Retries must be in the range 0-5\n";
            return 2;
        }
        argc -= 2;
    }
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
    std::filesystem::path source_directory;
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
    } else if (command == "sync") {
        if (argc < 5 || argc > 6) {
            print_usage();
            return 2;
        }
        source_directory = argv[4];
        if (argc == 6 &&
            (!parse_unsigned(std::string_view(argv[5]), request_id) || request_id == 0U)) {
            std::cerr << "Request ID must be nonzero\n";
            return 2;
        }
    } else {
        print_usage();
        return 2;
    }

    const char* secret_value = std::getenv("SYNCWIRE_PSK");
    if (secret_value == nullptr ||
        !syncwire::protocol::is_valid_authentication_secret(secret_value)) {
        std::cerr << "SYNCWIRE_PSK must contain 16-1024 bytes\n";
        return 2;
    }
    const std::string_view authentication_secret(secret_value);

    auto transport_failure = [](const syncwire::protocol::FrameIoResult& io) {
        return io.status == syncwire::protocol::FrameIoStatus::PeerClosed ||
               io.status == syncwire::protocol::FrameIoStatus::SystemError;
    };
    for (unsigned attempt = 0U; attempt <= retries; ++attempt) {
        bool retryable = false;
        const auto run_once = [&]() -> int {
            auto socket_result = syncwire::net::connect_ipv4(
                argv[1], static_cast<std::uint16_t>(port_value));
            if (const auto* error = std::get_if<syncwire::net::TcpError>(&socket_result);
                error != nullptr) {
                print_tcp_error(*error);
                retryable = true;
                return 1;
            }
            auto socket = std::get<syncwire::UniqueFd>(std::move(socket_result));
            const auto authentication = syncwire::protocol::authenticate_client(
                socket.get(), authentication_secret);
            if (!authentication.ok()) {
                print_authentication_error(authentication);
                retryable = authentication.status == syncwire::protocol::AuthenticationStatus::FrameIoError &&
                            transport_failure(authentication.frame_io);
                return 1;
            }

            if (command == "ping") {
                const auto exchange = syncwire::protocol::perform_ping(socket.get(), request_id);
                if (!exchange.ok()) {
                    print_exchange_error(exchange);
                    retryable = exchange.status == syncwire::protocol::PingPongStatus::FrameIoError &&
                                transport_failure(exchange.frame_io);
                    return 1;
                }
                std::cout << "PONG received for request " << request_id << '\n';
                return 0;
            }

            if (command == "upload") {
                const auto transfer = syncwire::protocol::send_file(
                    socket.get(), source_path, remote_name, request_id);
                if (!transfer.ok()) {
                    print_transfer_error(transfer);
                    retryable = transfer.status == syncwire::protocol::FileTransferStatus::FrameIoError &&
                                transport_failure(transfer.frame_io);
                    return 1;
                }
                std::cout << "Uploaded " << transfer.transferred << " bytes as " << remote_name
                          << " (resumed " << transfer.resumed_from << " bytes, sent "
                          << transfer.transferred - transfer.resumed_from << " bytes)\n";
                return 0;
            }

            const auto sync = syncwire::protocol::sync_directory(
                socket.get(), source_directory, request_id);
            if (!sync.ok()) {
                print_sync_error(sync);
                retryable = (sync.status == syncwire::protocol::DirectorySyncStatus::FrameIoError &&
                             transport_failure(sync.frame_io)) ||
                            (sync.status == syncwire::protocol::DirectorySyncStatus::FileTransferError &&
                             sync.file_transfer.status == syncwire::protocol::FileTransferStatus::FrameIoError &&
                             transport_failure(sync.file_transfer.frame_io));
                return 1;
            }
            std::cout << "Directory synchronized: " << sync.completed_uploads << " uploaded, "
                      << sync.unchanged_files << " unchanged, " << sync.server_only_files
                      << " server-only files preserved\n";
            return 0;
        };
        const int result = run_once();
        if (result == 0 || !retryable || attempt == retries) {
            return result;
        }
        std::cerr << "Transport interrupted; reconnecting (retry " << attempt + 1U << '/' << retries
                  << ")\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100U << attempt));
    }
    return 1;
}
