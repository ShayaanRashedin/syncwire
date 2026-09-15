#include "test_harness.hpp"

#include "syncwire/common/authentication.hpp"
#include "syncwire/common/directory_sync.hpp"
#include "syncwire/common/file_transfer.hpp"
#include "syncwire/common/ping_pong.hpp"
#include "syncwire/common/tcp_socket.hpp"
#include "syncwire/common/unique_fd.hpp"
#include "syncwire/server/concurrent_server.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

inline constexpr std::string_view kServerTestSecret =
    "syncwire-concurrent-test-secret";

class TemporaryServerDirectory {
public:
    TemporaryServerDirectory() {
        std::array<char, 40U> pattern{};
        constexpr std::string_view value = "/tmp/syncwire-server-XXXXXX";
        std::copy(value.begin(), value.end(), pattern.begin());
        if (char* created = ::mkdtemp(pattern.data()); created != nullptr) {
            path_ = created;
        }
    }

    TemporaryServerDirectory(const TemporaryServerDirectory&) = delete;
    TemporaryServerDirectory& operator=(const TemporaryServerDirectory&) = delete;

    ~TemporaryServerDirectory() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return !path_.empty();
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] bool write_file(const std::filesystem::path& path,
                              const std::string_view content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream output(path, std::ios::binary);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] syncwire::UniqueFd connect_to(const std::uint16_t port) {
    auto connected = syncwire::net::connect_ipv4("127.0.0.1", port);
    if (auto* socket = std::get_if<syncwire::UniqueFd>(&connected);
        socket != nullptr) {
        return std::move(*socket);
    }
    return syncwire::UniqueFd{};
}

[[nodiscard]] syncwire::UniqueFd
connect_authenticated(const std::uint16_t port) {
    auto socket = connect_to(port);
    if (!socket.valid() ||
        !syncwire::protocol::authenticate_client(
             socket.get(), kServerTestSecret)
             .ok()) {
        return syncwire::UniqueFd{};
    }
    return socket;
}

void test_concurrent_ping_sessions(TestRunner& runner) {
    TemporaryServerDirectory temporary;
    runner.expect(temporary.valid(), "temporary concurrent server directory opens");
    if (!temporary.valid()) {
        return;
    }

    syncwire::server::ConcurrentServer server(
        syncwire::server::ConcurrentServerConfig{
            .port = 0U,
            .destination_root = temporary.path() / "received",
            .authentication_secret = std::string(kServerTestSecret),
            .worker_count = 4U,
            .max_pending_connections = 16U,
        });
    const auto started = server.start();
    runner.expect(started.ok(), "concurrent server starts on an ephemeral port");
    runner.expect(server.port() != 0U, "concurrent server exposes its bound port");
    if (!started.ok()) {
        return;
    }

    std::optional<syncwire::server::ConcurrentServerResult> run_result;
    std::jthread server_thread([&] {
        run_result = server.run();
    });

    constexpr std::size_t client_count = 12U;
    std::atomic<std::size_t> successful_clients{0U};
    std::vector<std::jthread> clients;
    clients.reserve(client_count);
    for (std::size_t index = 0U; index < client_count; ++index) {
        clients.emplace_back([&, index] {
            auto client = connect_authenticated(server.port());
            if (!client.valid()) {
                return;
            }
            const auto ping = syncwire::protocol::perform_ping(
                client.get(), static_cast<std::uint64_t>(index + 1U));
            if (ping.ok()) {
                successful_clients.fetch_add(1U, std::memory_order_relaxed);
            }
        });
    }
    clients.clear();
    server.request_stop();
    server_thread.join();

    const auto stats = server.stats();
    runner.expect(successful_clients.load(std::memory_order_relaxed) == client_count,
                  "all simultaneous PING clients receive PONG");
    runner.expect(run_result.has_value() && run_result->ok(),
                  "concurrent accept loop stops cleanly");
    runner.expect(stats.accepted == client_count,
                  "concurrent server accepts every PING connection");
    runner.expect(stats.completed == client_count && stats.failed == 0U,
                  "worker pool completes every PING session");
    runner.expect(stats.rejected == 0U && stats.cancelled == 0U,
                  "adequately sized queue drops no PING sessions");
}

void test_mixed_concurrent_protocol_sessions(TestRunner& runner) {
    TemporaryServerDirectory temporary;
    runner.expect(temporary.valid(), "temporary mixed-session directory opens");
    if (!temporary.valid()) {
        return;
    }

    const auto destination = temporary.path() / "received";
    const auto upload_source = temporary.path() / "upload-source.txt";
    const auto sync_source = temporary.path() / "sync-source";
    runner.expect(write_file(upload_source, "standalone upload\n"),
                  "mixed-session upload fixture is written");
    runner.expect(write_file(sync_source / "nested" / "tree.txt", "directory sync\n"),
                  "mixed-session directory fixture is written");

    std::atomic<std::size_t> observed_sessions{0U};
    syncwire::server::ConcurrentServer server(
        syncwire::server::ConcurrentServerConfig{
            .port = 0U,
            .destination_root = destination,
            .authentication_secret = std::string(kServerTestSecret),
            .worker_count = 3U,
            .max_pending_connections = 8U,
        },
        [&](const std::size_t, const syncwire::server::ClientSessionResult&) {
            observed_sessions.fetch_add(1U, std::memory_order_relaxed);
        });
    const auto started = server.start();
    runner.expect(started.ok(), "mixed-session concurrent server starts");
    if (!started.ok()) {
        return;
    }

    std::optional<syncwire::server::ConcurrentServerResult> run_result;
    std::jthread server_thread([&] {
        run_result = server.run();
    });

    std::atomic_bool ping_ok{false};
    std::atomic_bool upload_ok{false};
    std::atomic_bool sync_ok{false};
    std::jthread ping_client([&] {
        auto socket = connect_authenticated(server.port());
        if (socket.valid()) {
            ping_ok.store(
                syncwire::protocol::perform_ping(socket.get(), 100U).ok(),
                std::memory_order_relaxed);
        }
    });
    std::jthread upload_client([&] {
        auto socket = connect_authenticated(server.port());
        if (socket.valid()) {
            upload_ok.store(
                syncwire::protocol::send_file(
                    socket.get(), upload_source, "single.txt", 200U)
                    .ok(),
                std::memory_order_relaxed);
        }
    });
    std::jthread sync_client([&] {
        auto socket = connect_authenticated(server.port());
        if (socket.valid()) {
            sync_ok.store(
                syncwire::protocol::sync_directory(
                    socket.get(), sync_source, 500U)
                    .ok(),
                std::memory_order_relaxed);
        }
    });

    ping_client.join();
    upload_client.join();
    sync_client.join();
    server.request_stop();
    server_thread.join();

    runner.expect(ping_ok.load(std::memory_order_relaxed),
                  "concurrent server completes a PING session");
    runner.expect(upload_ok.load(std::memory_order_relaxed),
                  "concurrent server completes an upload session");
    runner.expect(sync_ok.load(std::memory_order_relaxed),
                  "concurrent server completes a directory sync session");
    runner.expect(std::filesystem::exists(destination / "single.txt"),
                  "concurrent upload is committed");
    runner.expect(std::filesystem::exists(destination / "nested" / "tree.txt"),
                  "concurrent directory sync is committed");

    const auto stats = server.stats();
    runner.expect(run_result.has_value() && run_result->ok(),
                  "mixed-session accept loop stops cleanly");
    runner.expect(stats.accepted == 3U && stats.completed == 3U &&
                      stats.failed == 0U,
                  "server statistics account for mixed protocol sessions");
    runner.expect(observed_sessions.load(std::memory_order_relaxed) == 3U,
                  "observer receives one event per completed session");
}

void test_active_connection_shutdown(TestRunner& runner) {
    TemporaryServerDirectory temporary;
    runner.expect(temporary.valid(), "temporary shutdown-test directory opens");
    if (!temporary.valid()) {
        return;
    }

    syncwire::server::ConcurrentServer server(
        syncwire::server::ConcurrentServerConfig{
            .port = 0U,
            .destination_root = temporary.path() / "received",
            .authentication_secret = std::string(kServerTestSecret),
            .worker_count = 1U,
            .max_pending_connections = 2U,
            .epoll_timeout_ms = 10,
        });
    const auto started = server.start();
    runner.expect(started.ok(), "shutdown-test server starts");
    if (!started.ok()) {
        return;
    }

    std::optional<syncwire::server::ConcurrentServerResult> run_result;
    std::jthread server_thread([&] {
        run_result = server.run();
    });
    auto silent_client = connect_to(server.port());
    runner.expect(silent_client.valid(), "silent shutdown-test client connects");

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.stats().accepted == 0U &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    server.request_stop();
    server_thread.join();

    const auto stats = server.stats();
    runner.expect(run_result.has_value() && run_result->ok(),
                  "server stops with a silent client connected");
    runner.expect(stats.accepted == 1U,
                  "shutdown-test connection reaches the acceptor");
    runner.expect(stats.failed + stats.cancelled == 1U,
                  "shutdown accounts for the interrupted or queued session");
}

void test_invalid_server_lifecycle(TestRunner& runner) {
    syncwire::server::ConcurrentServer server(
        syncwire::server::ConcurrentServerConfig{
            .authentication_secret = std::string(kServerTestSecret),
            .worker_count = 0U,
        });
    runner.expect(
        server.run().status == syncwire::server::ConcurrentServerStatus::NotStarted,
        "server run rejects calls before start");
    runner.expect(
        server.start().status ==
            syncwire::server::ConcurrentServerStatus::InvalidConfiguration,
        "server start rejects an empty worker pool");
    runner.expect(
        server.start().status == syncwire::server::ConcurrentServerStatus::AlreadyStarted,
        "server start is single-attempt");

    syncwire::server::ConcurrentServer weak_secret_server(
        syncwire::server::ConcurrentServerConfig{
            .authentication_secret = "too-short",
        });
    runner.expect(
        weak_secret_server.start().status ==
            syncwire::server::ConcurrentServerStatus::InvalidConfiguration,
        "concurrent server rejects a weak authentication secret");
}

} // namespace

void run_concurrent_server_tests(TestRunner& runner) {
    test_concurrent_ping_sessions(runner);
    test_mixed_concurrent_protocol_sessions(runner);
    // Exercise the queued -> active transition against immediate shutdown repeatedly.
    for (unsigned iteration = 0U; iteration < 128U; ++iteration) {
        test_active_connection_shutdown(runner);
    }
    test_invalid_server_lifecycle(runner);
}
