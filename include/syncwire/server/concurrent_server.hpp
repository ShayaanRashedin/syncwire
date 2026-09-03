#pragma once

#include "syncwire/common/tcp_socket.hpp"
#include "syncwire/server/client_session.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace syncwire::server {

struct ConcurrentServerConfig {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{4040U};
    std::filesystem::path destination_root{"received"};
    std::string authentication_secret;
    std::size_t worker_count{4U};
    std::size_t max_pending_connections{64U};
    int backlog{128};
    int epoll_timeout_ms{100};
};

struct ConcurrentServerStats {
    std::uint64_t accepted{0U};
    std::uint64_t completed{0U};
    std::uint64_t failed{0U};
    std::uint64_t rejected{0U};
    std::uint64_t cancelled{0U};
    std::size_t peak_pending{0U};
};

enum class ConcurrentServerStatus {
    Success,
    InvalidConfiguration,
    AlreadyStarted,
    NotStarted,
    AlreadyRunning,
    ListenerError,
    EpollCreateError,
    EpollControlError,
    EpollWaitError,
    AcceptError,
    WorkerStartError,
};

struct ConcurrentServerResult {
    ConcurrentServerStatus status{ConcurrentServerStatus::Success};
    std::optional<net::TcpError> tcp_error{};
    int system_error{0};

    [[nodiscard]] bool ok() const noexcept {
        return status == ConcurrentServerStatus::Success;
    }
};

using SessionObserver =
    std::function<void(std::size_t worker_id, const ClientSessionResult& result)>;
using StopPredicate = std::function<bool()>;

class ConcurrentServer {
public:
    explicit ConcurrentServer(ConcurrentServerConfig config,
                              SessionObserver observer = {});
    ~ConcurrentServer();

    ConcurrentServer(const ConcurrentServer&) = delete;
    ConcurrentServer& operator=(const ConcurrentServer&) = delete;
    ConcurrentServer(ConcurrentServer&&) = delete;
    ConcurrentServer& operator=(ConcurrentServer&&) = delete;

    [[nodiscard]] ConcurrentServerResult start();
    [[nodiscard]] ConcurrentServerResult run(StopPredicate external_stop = {});
    void request_stop() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] ConcurrentServerStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view
concurrent_server_status_message(ConcurrentServerStatus status) noexcept;

} // namespace syncwire::server

