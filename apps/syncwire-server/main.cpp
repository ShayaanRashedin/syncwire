#include "syncwire/common/authentication.hpp"
#include "syncwire/common/directory_sync.hpp"
#include "syncwire/common/file_transfer.hpp"
#include "syncwire/common/ping_pong.hpp"
#include "syncwire/common/tcp_socket.hpp"
#include "syncwire/server/client_session.hpp"
#include "syncwire/server/concurrent_server.hpp"

#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <syncstream>
#include <system_error>

namespace {

volatile std::sig_atomic_t shutdown_requested = 0;

extern "C" void request_shutdown(const int) {
    shutdown_requested = 1;
}

template <typename Integer>
[[nodiscard]] bool parse_unsigned(const char* text, Integer& output) noexcept {
    const char* end = text;
    while (*end != '\0') {
        ++end;
    }
    Integer value{};
    const auto [parsed_end, error] = std::from_chars(text, end, value);
    if (error != std::errc{} || parsed_end != end) {
        return false;
    }
    output = value;
    return true;
}

void print_usage() {
    std::cerr << "Usage: syncwire-server [port] [destination-directory] [workers]\n";
}

void print_server_error(const syncwire::server::ConcurrentServerResult& result) {
    std::cerr << syncwire::server::concurrent_server_status_message(result.status);
    if (result.tcp_error.has_value()) {
        std::cerr << ": "
                  << syncwire::net::tcp_operation_message(result.tcp_error->operation);
    }
    if (result.system_error != 0) {
        std::cerr << ": " << std::strerror(result.system_error);
    }
    std::cerr << '\n';
}

void log_session(const std::size_t worker_id,
                 const syncwire::server::ClientSessionResult& result) {
    std::osyncstream output(result.ok() ? std::cout : std::cerr);
    output << "[worker " << worker_id << "] request " << result.request_id << ' '
           << syncwire::server::session_operation_message(result.operation) << ": ";
    if (!result.ok()) {
        output << syncwire::server::session_status_message(result.status);
        if (result.status ==
            syncwire::server::SessionStatus::AuthenticationError) {
            output << ": "
                   << syncwire::protocol::authentication_status_message(
                          result.authentication.status);
        }
        output << '\n';
        return;
    }

    switch (result.operation) {
    case syncwire::server::SessionOperation::Ping:
        output << "PONG sent\n";
        return;
    case syncwire::server::SessionOperation::Upload:
        output << result.transfer.transferred << " bytes committed\n";
        return;
    case syncwire::server::SessionOperation::DirectorySync:
        output << result.sync.completed_uploads << " uploaded, "
               << result.sync.unchanged_files << " unchanged, "
               << result.sync.server_only_files << " server-only preserved\n";
        return;
    case syncwire::server::SessionOperation::Unknown:
        output << "completed\n";
        return;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 4) {
        print_usage();
        return 2;
    }

    unsigned port_value = 4040U;
    if (argc >= 2 &&
        (!parse_unsigned(argv[1], port_value) || port_value > 65'535U)) {
        std::cerr << "Port must be in the range 0-65535\n";
        return 2;
    }

    std::size_t worker_count = 4U;
    if (argc == 4 &&
        (!parse_unsigned(argv[3], worker_count) || worker_count == 0U ||
         worker_count > 256U)) {
        std::cerr << "Worker count must be in the range 1-256\n";
        return 2;
    }

    const char* secret_value = std::getenv("SYNCWIRE_PSK");
    if (secret_value == nullptr ||
        !syncwire::protocol::is_valid_authentication_secret(secret_value)) {
        std::cerr << "SYNCWIRE_PSK must contain 16-1024 bytes\n";
        return 2;
    }

    syncwire::server::ConcurrentServerConfig config{
        .bind_address = "127.0.0.1",
        .port = static_cast<std::uint16_t>(port_value),
        .destination_root = argc >= 3 ? std::filesystem::path(argv[2])
                                     : std::filesystem::path("received"),
        .authentication_secret = secret_value,
        .worker_count = worker_count,
    };
    syncwire::server::ConcurrentServer server(config, log_session);

    const auto started = server.start();
    if (!started.ok()) {
        print_server_error(started);
        return 1;
    }

    static_cast<void>(std::signal(SIGINT, request_shutdown));
    static_cast<void>(std::signal(SIGTERM, request_shutdown));

    std::cout << "SyncWire concurrent server listening on 127.0.0.1:"
              << server.port() << " with " << worker_count
              << " workers and HMAC authentication (Ctrl+C to stop)\n";

    const auto run_result = server.run([] {
        return shutdown_requested != 0;
    });
    const auto stats = server.stats();
    std::cout << "Server stopped: " << stats.accepted << " accepted, "
              << stats.completed << " completed, " << stats.failed << " failed, "
              << stats.rejected << " rejected, " << stats.cancelled
              << " cancelled; peak queue " << stats.peak_pending << '\n';

    if (!run_result.ok()) {
        print_server_error(run_result);
        return 1;
    }
    return 0;
}

