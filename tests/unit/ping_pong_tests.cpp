#include "test_harness.hpp"

#include "syncwire/common/frame_io.hpp"
#include "syncwire/common/ping_pong.hpp"
#include "syncwire/common/unique_fd.hpp"

#include <cstdint>
#include <optional>
#include <sys/socket.h>
#include <thread>
#include <variant>

namespace {

[[nodiscard]] bool open_socket_pair(syncwire::UniqueFd& first, syncwire::UniqueFd& second) {
    int sockets[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) {
        return false;
    }
    first.reset(sockets[0]);
    second.reset(sockets[1]);
    return true;
}

void test_successful_exchange(TestRunner& runner) {
    syncwire::UniqueFd client;
    syncwire::UniqueFd server;
    runner.expect(open_socket_pair(client, server), "socketpair opens for PING/PONG test");

    std::optional<syncwire::protocol::PingPongResult> server_result;
    std::jthread server_thread([&] {
        server_result = syncwire::protocol::serve_ping_once(server.get());
    });

    const auto client_result = syncwire::protocol::perform_ping(client.get(), 0x1234U);
    server_thread.join();

    runner.expect(client_result.ok(), "client accepts matching PONG");
    runner.expect(server_result.has_value() && server_result->ok(), "server returns PONG for PING");
}

void test_server_rejects_wrong_message(TestRunner& runner) {
    syncwire::UniqueFd client;
    syncwire::UniqueFd server;
    runner.expect(open_socket_pair(client, server), "socketpair opens for message-order test");

    const syncwire::protocol::Frame wrong{
        .header = syncwire::protocol::FrameHeader{
            .message_type = syncwire::protocol::MessageType::Pong,
            .request_id = 7U,
        },
        .payload = {},
    };
    runner.expect(syncwire::protocol::send_frame(client.get(), wrong).ok(),
                  "test sends a PONG where PING is required");
    const auto result = syncwire::protocol::serve_ping_once(server.get());
    runner.expect(result.status == syncwire::protocol::PingPongStatus::UnexpectedMessageType,
                  "server rejects unexpected message type");
}

void test_client_rejects_mismatched_request_id(TestRunner& runner) {
    syncwire::UniqueFd client;
    syncwire::UniqueFd server;
    runner.expect(open_socket_pair(client, server), "socketpair opens for correlation test");

    std::jthread server_thread([&] {
        const auto received = syncwire::protocol::receive_frame(server.get());
        if (!std::holds_alternative<syncwire::protocol::Frame>(received)) {
            return;
        }
        const syncwire::protocol::Frame wrong_pong{
            .header = syncwire::protocol::FrameHeader{
                .message_type = syncwire::protocol::MessageType::Pong,
                .request_id = 999U,
            },
            .payload = {},
        };
        static_cast<void>(syncwire::protocol::send_frame(server.get(), wrong_pong));
    });

    const auto result = syncwire::protocol::perform_ping(client.get(), 55U);
    server_thread.join();
    runner.expect(result.status == syncwire::protocol::PingPongStatus::MismatchedRequestId &&
                      result.expected_request_id == 55U && result.actual_request_id == 999U,
                  "client rejects PONG with the wrong request ID");
}

void test_invalid_request_id(TestRunner& runner) {
    const auto result = syncwire::protocol::perform_ping(-1, 0U);
    runner.expect(result.status == syncwire::protocol::PingPongStatus::InvalidRequestId,
                  "client rejects zero request ID before socket I/O");
}

} // namespace

void run_ping_pong_tests(TestRunner& runner) {
    test_successful_exchange(runner);
    test_server_rejects_wrong_message(runner);
    test_client_rejects_mismatched_request_id(runner);
    test_invalid_request_id(runner);
}

