#pragma once

#include "syncwire/common/frame_io.hpp"

#include <cstdint>
#include <string_view>

namespace syncwire::protocol {

enum class PingPongStatus {
    Success,
    FrameIoError,
    UnexpectedMessageType,
    UnexpectedPayload,
    InvalidRequestId,
    UnexpectedTransferId,
    MismatchedRequestId,
};

struct PingPongResult {
    PingPongStatus status{PingPongStatus::Success};
    FrameIoResult frame_io;
    std::uint64_t expected_request_id{0U};
    std::uint64_t actual_request_id{0U};

    [[nodiscard]] bool ok() const noexcept {
        return status == PingPongStatus::Success;
    }
};

[[nodiscard]] PingPongResult serve_ping_once(int client_fd);
[[nodiscard]] PingPongResult perform_ping(int server_fd, std::uint64_t request_id);
[[nodiscard]] std::string_view ping_pong_status_message(PingPongStatus status) noexcept;

} // namespace syncwire::protocol

