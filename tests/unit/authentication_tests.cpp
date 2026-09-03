#include "test_harness.hpp"

#include "syncwire/common/authentication.hpp"
#include "syncwire/common/frame_io.hpp"
#include "syncwire/common/frame_parser.hpp"
#include "syncwire/common/protocol.hpp"
#include "syncwire/common/unique_fd.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

inline constexpr std::string_view kTestSecret =
    "syncwire-test-secret-32-bytes!!";

[[nodiscard]] bool open_socket_pair(syncwire::UniqueFd& first,
                                    syncwire::UniqueFd& second) {
    int sockets[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) {
        return false;
    }
    first.reset(sockets[0]);
    second.reset(sockets[1]);
    return true;
}

void test_secret_validation(TestRunner& runner) {
    runner.expect(
        !syncwire::protocol::is_valid_authentication_secret("short"),
        "authentication rejects a short secret");
    runner.expect(
        syncwire::protocol::is_valid_authentication_secret(kTestSecret),
        "authentication accepts a suitably long secret");
    runner.expect(
        !syncwire::protocol::is_valid_authentication_secret(
            std::string(
                syncwire::protocol::kMaximumAuthenticationSecretSize + 1U,
                'x')),
        "authentication rejects an oversized secret");
    runner.expect(
        syncwire::protocol::authenticate_client(-1, "short").status ==
            syncwire::protocol::AuthenticationStatus::InvalidSecret,
        "client validates its secret before socket I/O");
}

void test_successful_authentication(TestRunner& runner) {
    syncwire::UniqueFd client;
    syncwire::UniqueFd server;
    runner.expect(open_socket_pair(client, server),
                  "socketpair opens for authentication");
    if (!client.valid() || !server.valid()) {
        return;
    }

    std::optional<syncwire::protocol::AuthenticationResult> server_result;
    std::jthread server_thread([&] {
        server_result =
            syncwire::protocol::authenticate_server(server.get(), kTestSecret);
    });
    const auto client_result =
        syncwire::protocol::authenticate_client(client.get(), kTestSecret);
    server_thread.join();

    runner.expect(client_result.ok(),
                  "client accepts a valid HMAC-SHA256 proof exchange");
    runner.expect(server_result.has_value() && server_result->ok(),
                  "server accepts a client that possesses the shared secret");
}

void test_wrong_secret_is_rejected(TestRunner& runner) {
    syncwire::UniqueFd client;
    syncwire::UniqueFd server;
    runner.expect(open_socket_pair(client, server),
                  "socketpair opens for authentication rejection");
    if (!client.valid() || !server.valid()) {
        return;
    }

    std::optional<syncwire::protocol::AuthenticationResult> server_result;
    std::jthread server_thread([&] {
        server_result =
            syncwire::protocol::authenticate_server(server.get(), kTestSecret);
    });
    const auto client_result = syncwire::protocol::authenticate_client(
        client.get(), "different-test-secret-32-bytes!");
    server_thread.join();

    runner.expect(
        client_result.status ==
            syncwire::protocol::AuthenticationStatus::Rejected,
        "client observes rejection when its secret is wrong");
    runner.expect(
        server_result.has_value() &&
            server_result->status ==
                syncwire::protocol::AuthenticationStatus::Rejected,
        "server rejects a proof generated with the wrong secret");
}

void test_malformed_proof_is_rejected(TestRunner& runner) {
    syncwire::UniqueFd client;
    syncwire::UniqueFd server;
    runner.expect(open_socket_pair(client, server),
                  "socketpair opens for malformed authentication proof");
    if (!client.valid() || !server.valid()) {
        return;
    }

    std::optional<syncwire::protocol::AuthenticationResult> server_result;
    std::jthread server_thread([&] {
        server_result =
            syncwire::protocol::authenticate_server(server.get(), kTestSecret);
    });

    const auto challenge_received =
        syncwire::protocol::receive_frame(client.get());
    const auto* challenge =
        std::get_if<syncwire::protocol::Frame>(&challenge_received);
    runner.expect(
        challenge != nullptr &&
            challenge->header.message_type ==
                syncwire::protocol::MessageType::AuthChallenge &&
            challenge->payload.size() ==
                syncwire::protocol::kAuthenticationNonceSize,
        "server sends a correctly shaped authentication challenge");
    if (challenge != nullptr) {
        runner.expect(
            std::ranges::any_of(challenge->payload, [](const std::byte value) {
                return value != std::byte{0U};
            }),
            "authentication challenge contains random nonce bytes");
    }

    const syncwire::protocol::Frame malformed_proof{
        .header = syncwire::protocol::FrameHeader{
            .message_type = syncwire::protocol::MessageType::AuthProof,
            .payload_length = 1U,
        },
        .payload = {std::byte{0x42}},
    };
    runner.expect(
        syncwire::protocol::send_frame(client.get(), malformed_proof).ok(),
        "malformed authentication proof is transmitted");

    const auto result_received =
        syncwire::protocol::receive_frame(client.get());
    const auto* result =
        std::get_if<syncwire::protocol::Frame>(&result_received);
    runner.expect(
        result != nullptr &&
            result->header.message_type ==
                syncwire::protocol::MessageType::AuthResult &&
            result->payload == std::vector<std::byte>{std::byte{0x01}},
        "server returns an explicit rejection result");
    server_thread.join();
    runner.expect(
        server_result.has_value() &&
            server_result->status ==
                syncwire::protocol::AuthenticationStatus::InvalidProof,
        "server classifies a malformed proof");
}

void test_invalid_server_proof_is_rejected(TestRunner& runner) {
    syncwire::UniqueFd client;
    syncwire::UniqueFd server;
    runner.expect(open_socket_pair(client, server),
                  "socketpair opens for server-proof rejection");
    if (!client.valid() || !server.valid()) {
        return;
    }

    std::atomic_bool fake_server_completed{false};
    std::jthread server_thread([&] {
        const std::vector<std::byte> nonce(
            syncwire::protocol::kAuthenticationNonceSize,
            std::byte{0x5A});
        const syncwire::protocol::Frame challenge{
            .header = syncwire::protocol::FrameHeader{
                .message_type =
                    syncwire::protocol::MessageType::AuthChallenge,
                .payload_length =
                    static_cast<std::uint32_t>(nonce.size()),
            },
            .payload = nonce,
        };
        if (!syncwire::protocol::send_frame(server.get(), challenge).ok()) {
            return;
        }
        const auto proof_received =
            syncwire::protocol::receive_frame(server.get());
        if (!std::holds_alternative<syncwire::protocol::Frame>(
                proof_received)) {
            return;
        }

        std::vector<std::byte> payload(
            1U + syncwire::protocol::kAuthenticationProofSize,
            std::byte{0U});
        const syncwire::protocol::Frame false_acceptance{
            .header = syncwire::protocol::FrameHeader{
                .message_type =
                    syncwire::protocol::MessageType::AuthResult,
                .payload_length =
                    static_cast<std::uint32_t>(payload.size()),
            },
            .payload = std::move(payload),
        };
        if (syncwire::protocol::send_frame(server.get(), false_acceptance)
                .ok()) {
            fake_server_completed.store(true, std::memory_order_relaxed);
        }
    });

    const auto client_result =
        syncwire::protocol::authenticate_client(client.get(), kTestSecret);
    server_thread.join();
    runner.expect(fake_server_completed.load(std::memory_order_relaxed),
                  "fake server returns a forged acceptance proof");
    runner.expect(
        client_result.status ==
            syncwire::protocol::AuthenticationStatus::InvalidServerProof,
        "client rejects a server that cannot prove possession of the secret");
}

} // namespace

void run_authentication_tests(TestRunner& runner) {
    test_secret_validation(runner);
    test_successful_authentication(runner);
    test_wrong_secret_is_rejected(runner);
    test_malformed_proof_is_rejected(runner);
    test_invalid_server_proof_is_rejected(runner);
}

