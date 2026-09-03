#include "syncwire/server/concurrent_server.hpp"

#include "syncwire/common/tcp_socket.hpp"
#include "syncwire/common/unique_fd.hpp"
#include "syncwire/server/client_session.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <sys/epoll.h>
#include <sys/socket.h>

namespace syncwire::server {
namespace {

inline constexpr std::size_t kMaximumWorkerCount = 256U;
inline constexpr std::size_t kMaximumPendingConnections = 65'536U;
inline constexpr int kMaximumEpollTimeoutMs = 60'000;
inline constexpr int kEpollEventCapacity = 8;

[[nodiscard]] bool valid_config(const ConcurrentServerConfig& config) {
    return !config.bind_address.empty() && !config.destination_root.empty() &&
           protocol::is_valid_authentication_secret(
               config.authentication_secret) &&
           config.worker_count > 0U &&
           config.worker_count <= kMaximumWorkerCount &&
           config.max_pending_connections > 0U &&
           config.max_pending_connections <= kMaximumPendingConnections &&
           config.backlog > 0 && config.epoll_timeout_ms > 0 &&
           config.epoll_timeout_ms <= kMaximumEpollTimeoutMs;
}

} // namespace

struct ConcurrentServer::Impl {
    explicit Impl(ConcurrentServerConfig server_config, SessionObserver session_observer)
        : config(std::move(server_config)), observer(std::move(session_observer)) {}

    ConcurrentServerConfig config;
    SessionObserver observer;
    UniqueFd listener;
    UniqueFd epoll;
    std::uint16_t bound_port{0U};

    mutable std::mutex lifecycle_mutex;
    bool start_attempted{false};
    bool started{false};
    bool running{false};

    std::atomic_bool stop_requested{false};
    std::mutex queue_mutex;
    std::condition_variable queue_ready;
    std::deque<UniqueFd> pending;
    bool workers_stopping{false};
    std::vector<std::jthread> workers;

    std::mutex active_mutex;
    std::unordered_set<int> active_connections;
    std::mutex destination_mutex;

    std::atomic<std::uint64_t> accepted{0U};
    std::atomic<std::uint64_t> completed{0U};
    std::atomic<std::uint64_t> failed{0U};
    std::atomic<std::uint64_t> rejected{0U};
    std::atomic<std::uint64_t> cancelled{0U};
    std::atomic<std::size_t> peak_pending{0U};

    void observe_peak(const std::size_t depth) noexcept {
        auto current = peak_pending.load(std::memory_order_relaxed);
        while (depth > current &&
               !peak_pending.compare_exchange_weak(
                   current, depth, std::memory_order_relaxed)) {
        }
    }

    void shutdown_active_connections() noexcept {
        const std::scoped_lock lock(active_mutex);
        for (const int fd : active_connections) {
            static_cast<void>(::shutdown(fd, SHUT_RDWR));
        }
    }

    void worker_loop(const std::size_t worker_id) {
        while (true) {
            UniqueFd client;
            {
                std::unique_lock lock(queue_mutex);
                queue_ready.wait(lock, [this] {
                    return workers_stopping || !pending.empty();
                });
                if (pending.empty()) {
                    return;
                }
                client = std::move(pending.front());
                pending.pop_front();
            }

            {
                const std::scoped_lock lock(active_mutex);
                active_connections.insert(client.get());
            }
            auto result =
                serve_client_session(client.get(),
                                     config.destination_root,
                                     config.authentication_secret,
                                     &destination_mutex);
            {
                const std::scoped_lock lock(active_mutex);
                active_connections.erase(client.get());
            }

            if (result.ok()) {
                completed.fetch_add(1U, std::memory_order_relaxed);
            } else {
                failed.fetch_add(1U, std::memory_order_relaxed);
            }
            if (observer) {
                try {
                    observer(worker_id, result);
                } catch (...) {
                    // Observability must never terminate a networking worker.
                }
            }
        }
    }

    void stop_workers() noexcept {
        stop_requested.store(true, std::memory_order_relaxed);
        shutdown_active_connections();
        {
            const std::scoped_lock lock(queue_mutex);
            workers_stopping = true;
            cancelled.fetch_add(
                static_cast<std::uint64_t>(pending.size()), std::memory_order_relaxed);
            pending.clear();
        }
        queue_ready.notify_all();
        workers.clear();
    }

    [[nodiscard]] ConcurrentServerResult accept_ready_connections() {
        while (!stop_requested.load(std::memory_order_relaxed)) {
            const int raw_client =
                ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC);
            if (raw_client >= 0) {
                accepted.fetch_add(1U, std::memory_order_relaxed);
                UniqueFd client(raw_client);
                bool enqueued = false;
                std::size_t depth = 0U;
                {
                    const std::scoped_lock lock(queue_mutex);
                    if (!workers_stopping &&
                        pending.size() < config.max_pending_connections) {
                        pending.push_back(std::move(client));
                        depth = pending.size();
                        enqueued = true;
                    }
                }
                if (enqueued) {
                    observe_peak(depth);
                    queue_ready.notify_one();
                } else {
                    rejected.fetch_add(1U, std::memory_order_relaxed);
                }
                continue;
            }

            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return ConcurrentServerResult{};
            }
            return ConcurrentServerResult{
                .status = ConcurrentServerStatus::AcceptError,
                .system_error = errno,
            };
        }
        return ConcurrentServerResult{};
    }
};

ConcurrentServer::ConcurrentServer(ConcurrentServerConfig config,
                                   SessionObserver observer)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(observer))) {}

ConcurrentServer::~ConcurrentServer() {
    request_stop();
    impl_->stop_workers();
}

ConcurrentServerResult ConcurrentServer::start() {
    {
        const std::scoped_lock lock(impl_->lifecycle_mutex);
        if (impl_->start_attempted) {
            return ConcurrentServerResult{
                .status = ConcurrentServerStatus::AlreadyStarted,
            };
        }
        impl_->start_attempted = true;
    }
    if (!valid_config(impl_->config)) {
        return ConcurrentServerResult{
            .status = ConcurrentServerStatus::InvalidConfiguration,
        };
    }

    auto listener_result = net::listen_ipv4(
        impl_->config.bind_address,
        impl_->config.port,
        impl_->config.backlog,
        true);
    if (const auto* error = std::get_if<net::TcpError>(&listener_result);
        error != nullptr) {
        return ConcurrentServerResult{
            .status = ConcurrentServerStatus::ListenerError,
            .tcp_error = *error,
            .system_error = error->system_error,
        };
    }
    impl_->listener = std::get<UniqueFd>(std::move(listener_result));

    const auto port_result = net::local_port(impl_->listener.get());
    if (const auto* error = std::get_if<net::TcpError>(&port_result);
        error != nullptr) {
        impl_->listener.reset();
        return ConcurrentServerResult{
            .status = ConcurrentServerStatus::ListenerError,
            .tcp_error = *error,
            .system_error = error->system_error,
        };
    }
    impl_->bound_port = std::get<std::uint16_t>(port_result);

    impl_->epoll.reset(::epoll_create1(EPOLL_CLOEXEC));
    if (!impl_->epoll.valid()) {
        const int system_error = errno;
        impl_->listener.reset();
        return ConcurrentServerResult{
            .status = ConcurrentServerStatus::EpollCreateError,
            .system_error = system_error,
        };
    }

    epoll_event listener_event{};
    listener_event.events = EPOLLIN;
    listener_event.data.fd = impl_->listener.get();
    if (::epoll_ctl(impl_->epoll.get(),
                    EPOLL_CTL_ADD,
                    impl_->listener.get(),
                    &listener_event) < 0) {
        const int system_error = errno;
        impl_->epoll.reset();
        impl_->listener.reset();
        return ConcurrentServerResult{
            .status = ConcurrentServerStatus::EpollControlError,
            .system_error = system_error,
        };
    }

    try {
        impl_->workers.reserve(impl_->config.worker_count);
        for (std::size_t index = 0U; index < impl_->config.worker_count; ++index) {
            impl_->workers.emplace_back([this, index] {
                impl_->worker_loop(index);
            });
        }
    } catch (const std::system_error& error) {
        impl_->stop_workers();
        impl_->epoll.reset();
        impl_->listener.reset();
        return ConcurrentServerResult{
            .status = ConcurrentServerStatus::WorkerStartError,
            .system_error = error.code().value(),
        };
    }

    {
        const std::scoped_lock lock(impl_->lifecycle_mutex);
        impl_->started = true;
    }
    return ConcurrentServerResult{};
}

ConcurrentServerResult ConcurrentServer::run(StopPredicate external_stop) {
    {
        const std::scoped_lock lock(impl_->lifecycle_mutex);
        if (!impl_->started) {
            return ConcurrentServerResult{
                .status = ConcurrentServerStatus::NotStarted,
            };
        }
        if (impl_->running) {
            return ConcurrentServerResult{
                .status = ConcurrentServerStatus::AlreadyRunning,
            };
        }
        impl_->running = true;
    }

    auto finish = [this](ConcurrentServerResult result) {
        impl_->stop_workers();
        impl_->listener.reset();
        impl_->epoll.reset();
        const std::scoped_lock lock(impl_->lifecycle_mutex);
        impl_->running = false;
        return result;
    };

    std::array<epoll_event, kEpollEventCapacity> events{};
    while (!impl_->stop_requested.load(std::memory_order_relaxed) &&
           !(external_stop && external_stop())) {
        const int ready = ::epoll_wait(impl_->epoll.get(),
                                       events.data(),
                                       static_cast<int>(events.size()),
                                       impl_->config.epoll_timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return finish(ConcurrentServerResult{
                .status = ConcurrentServerStatus::EpollWaitError,
                .system_error = errno,
            });
        }
        for (int index = 0; index < ready; ++index) {
            if (events[static_cast<std::size_t>(index)].data.fd !=
                impl_->listener.get()) {
                continue;
            }
            const auto accepted = impl_->accept_ready_connections();
            if (!accepted.ok()) {
                return finish(accepted);
            }
        }
    }
    return finish(ConcurrentServerResult{});
}

void ConcurrentServer::request_stop() noexcept {
    impl_->stop_requested.store(true, std::memory_order_relaxed);
    impl_->shutdown_active_connections();
}

std::uint16_t ConcurrentServer::port() const noexcept {
    return impl_->bound_port;
}

ConcurrentServerStats ConcurrentServer::stats() const noexcept {
    return ConcurrentServerStats{
        .accepted = impl_->accepted.load(std::memory_order_relaxed),
        .completed = impl_->completed.load(std::memory_order_relaxed),
        .failed = impl_->failed.load(std::memory_order_relaxed),
        .rejected = impl_->rejected.load(std::memory_order_relaxed),
        .cancelled = impl_->cancelled.load(std::memory_order_relaxed),
        .peak_pending = impl_->peak_pending.load(std::memory_order_relaxed),
    };
}

std::string_view
concurrent_server_status_message(const ConcurrentServerStatus status) noexcept {
    switch (status) {
    case ConcurrentServerStatus::Success:
        return "server completed";
    case ConcurrentServerStatus::InvalidConfiguration:
        return "server configuration is invalid";
    case ConcurrentServerStatus::AlreadyStarted:
        return "server start was already attempted";
    case ConcurrentServerStatus::NotStarted:
        return "server has not started";
    case ConcurrentServerStatus::AlreadyRunning:
        return "server is already running";
    case ConcurrentServerStatus::ListenerError:
        return "server listener setup failed";
    case ConcurrentServerStatus::EpollCreateError:
        return "epoll creation failed";
    case ConcurrentServerStatus::EpollControlError:
        return "epoll registration failed";
    case ConcurrentServerStatus::EpollWaitError:
        return "epoll wait failed";
    case ConcurrentServerStatus::AcceptError:
        return "client accept failed";
    case ConcurrentServerStatus::WorkerStartError:
        return "worker pool startup failed";
    }
    return "unknown concurrent server status";
}

} // namespace syncwire::server

