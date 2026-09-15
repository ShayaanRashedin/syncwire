#include "syncwire/common/ping_pong.hpp"

#include "syncwire/common/frame_parser.hpp"
#include "syncwire/common/protocol.hpp"

#include <cstdint>
#include <variant>

namespace syncwire::protocol {
namespace {

[[nodiscard]] PingPongResult validate_common(const Frame& frame,
                                             const MessageType expected_type) noexcept {
    if (frame.header.message_type != expected_type) {
        return PingPongResult{.status = PingPongStatus::UnexpectedMessageType};
    }
    if (!frame.payload.empty()) {
        return PingPongResult{.status = PingPongStatus::UnexpectedPayload};
    }
    if (frame.header.request_id == 0U) {
        return PingPongResult{.status = PingPongStatus::InvalidRequestId};
    }
    if (frame.header.transfer_id != 0U) {
        return PingPongResult{.status = PingPongStatus::UnexpectedTransferId};
    }
    return PingPongResult{};
}

[[nodiscard]] PingPongResult frame_error(const FrameIoResult& result) noexcept {
    return PingPongResult{
        .status = PingPongStatus::FrameIoError,
        .frame_io = result,
    };
}

} // namespace

PingPongResult serve_ping_once(const int client_fd) {
    const auto received = receive_frame(client_fd);
    if (const auto* error = std::get_if<FrameIoResult>(&received); error != nullptr) {
        return frame_error(*error);
    }

    return serve_ping_frame(client_fd, std::get<Frame>(received));
}

PingPongResult serve_ping_frame(const int client_fd, const Frame& ping) {
    const auto validation = validate_common(ping, MessageType::Ping);
    if (!validation.ok()) {
        return validation;
    }

    const Frame pong{
        .header = FrameHeader{
            .message_type = MessageType::Pong,
            .request_id = ping.header.request_id,
        },
        .payload = {},
    };
    const auto sent = send_frame(client_fd, pong);
    if (!sent.ok()) {
        return frame_error(sent);
    }

    return PingPongResult{};
}

PingPongResult perform_ping(const int server_fd, const std::uint64_t request_id) {
    if (request_id == 0U) {
        return PingPongResult{.status = PingPongStatus::InvalidRequestId};
    }

    const Frame ping{
        .header = FrameHeader{
            .message_type = MessageType::Ping,
            .request_id = request_id,
        },
        .payload = {},
    };
    const auto sent = send_frame(server_fd, ping);
    if (!sent.ok()) {
        return frame_error(sent);
    }

    const auto received = receive_frame(server_fd);
    if (const auto* error = std::get_if<FrameIoResult>(&received); error != nullptr) {
        return frame_error(*error);
    }

    const auto& pong = std::get<Frame>(received);
    const auto validation = validate_common(pong, MessageType::Pong);
    if (!validation.ok()) {
        return validation;
    }
    if (pong.header.request_id != request_id) {
        return PingPongResult{
            .status = PingPongStatus::MismatchedRequestId,
            .expected_request_id = request_id,
            .actual_request_id = pong.header.request_id,
        };
    }

    return PingPongResult{};
}

std::string_view ping_pong_status_message(const PingPongStatus status) noexcept {
    switch (status) {
    case PingPongStatus::Success:
        return "PING/PONG exchange succeeded";
    case PingPongStatus::FrameIoError:
        return "frame transport failed";
    case PingPongStatus::UnexpectedMessageType:
        return "peer sent an unexpected message type";
    case PingPongStatus::UnexpectedPayload:
        return "PING and PONG must have empty payloads";
    case PingPongStatus::InvalidRequestId:
        return "request ID must be nonzero";
    case PingPongStatus::UnexpectedTransferId:
        return "PING and PONG must not carry a transfer ID";
    case PingPongStatus::MismatchedRequestId:
        return "PONG request ID does not match PING";
    }
    return "unknown PING/PONG status";
}

} // namespace syncwire::protocol
