#include "test_harness.hpp"

#include "syncwire/common/frame_io.hpp"
#include "syncwire/common/socket_io.hpp"
#include "syncwire/common/unique_fd.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <variant>
#include <vector>

namespace {

[[nodiscard]] std::vector<std::byte> bytes_of(const std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(begin, begin + text.size());
}

[[nodiscard]] bool open_socket_pair(syncwire::UniqueFd& first, syncwire::UniqueFd& second) {
    int sockets[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) {
        return false;
    }
    first.reset(sockets[0]);
    second.reset(sockets[1]);
    return true;
}

void test_send_and_receive(TestRunner& runner) {
    syncwire::UniqueFd sender;
    syncwire::UniqueFd receiver;
    runner.expect(open_socket_pair(sender, receiver), "socketpair opens for I/O test");

    const auto payload = bytes_of("partial I/O safe");
    std::vector<std::byte> received(payload.size());
    const auto sent = syncwire::net::send_all(sender.get(), payload);
    const auto read = syncwire::net::recv_exact(receiver.get(), received);

    runner.expect(sent.ok() && sent.transferred == payload.size(), "send_all sends every byte");
    runner.expect(read.ok() && read.transferred == payload.size(), "recv_exact reads every byte");
    runner.expect(received == payload, "socket I/O preserves bytes");
}

void test_peer_closes_mid_read(TestRunner& runner) {
    syncwire::UniqueFd sender;
    syncwire::UniqueFd receiver;
    runner.expect(open_socket_pair(sender, receiver), "socketpair opens for close test");

    const auto partial = bytes_of("half");
    runner.expect(syncwire::net::send_all(sender.get(), partial).ok(), "partial data is sent");
    sender.reset();

    std::vector<std::byte> destination(8U);
    const auto read = syncwire::net::recv_exact(receiver.get(), destination);
    runner.expect(read.status == syncwire::net::IoStatus::PeerClosed,
                  "recv_exact reports early peer closure");
    runner.expect(read.transferred == partial.size(), "recv_exact reports partial byte count");
}

void test_send_suppresses_sigpipe(TestRunner& runner) {
    syncwire::UniqueFd sender;
    syncwire::UniqueFd receiver;
    runner.expect(open_socket_pair(sender, receiver), "socketpair opens for SIGPIPE test");
    receiver.reset();

    const auto payload = bytes_of("x");
    const auto sent = syncwire::net::send_all(sender.get(), payload);
    runner.expect(sent.status == syncwire::net::IoStatus::SystemError,
                  "send_all reports a closed peer without terminating the process");
}

void test_frame_validation(TestRunner& runner) {
    syncwire::UniqueFd sender;
    syncwire::UniqueFd receiver;
    runner.expect(open_socket_pair(sender, receiver), "socketpair opens for frame test");

    const syncwire::protocol::Frame invalid{
        .header = syncwire::protocol::FrameHeader{
            .message_type = syncwire::protocol::MessageType::Ping,
            .payload_length = 1U,
            .request_id = 9U,
        },
        .payload = {},
    };
    const auto sent = syncwire::protocol::send_frame(sender.get(), invalid);
    runner.expect(sent.status == syncwire::protocol::FrameIoStatus::InvalidFrame,
                  "send_frame rejects mismatched payload length");

    auto encoded = syncwire::protocol::encode_header(syncwire::protocol::FrameHeader{
        .message_type = syncwire::protocol::MessageType::Ping,
        .request_id = 10U,
    });
    encoded[0U] = std::byte{0x00};
    runner.expect(syncwire::net::send_all(sender.get(), encoded).ok(),
                  "malformed header bytes are sent to receiver");

    const auto received = syncwire::protocol::receive_frame(receiver.get());
    const auto* error = std::get_if<syncwire::protocol::FrameIoResult>(&received);
    runner.expect(error != nullptr &&
                      error->status == syncwire::protocol::FrameIoStatus::InvalidHeader &&
                      error->codec_error == syncwire::protocol::CodecError::InvalidMagic,
                  "receive_frame rejects malformed header before payload allocation");
}

} // namespace

void run_socket_io_tests(TestRunner& runner) {
    test_send_and_receive(runner);
    test_peer_closes_mid_read(runner);
    test_send_suppresses_sigpipe(runner);
    test_frame_validation(runner);
}

