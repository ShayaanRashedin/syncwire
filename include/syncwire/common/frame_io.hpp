#pragma once

#include "syncwire/common/frame_parser.hpp"
#include "syncwire/common/socket_io.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace syncwire::protocol {

enum class FrameIoStatus {
    Complete,
    PeerClosed,
    SystemError,
    InvalidHeader,
    InvalidFrame,
};

struct FrameIoResult {
    FrameIoStatus status{FrameIoStatus::Complete};
    std::size_t transferred{0U};
    int system_error{0};
    std::optional<CodecError> codec_error{};

    [[nodiscard]] bool ok() const noexcept {
        return status == FrameIoStatus::Complete;
    }
};

using FrameReceiveResult = std::variant<Frame, FrameIoResult>;

[[nodiscard]] FrameIoResult send_frame(int fd, const Frame& frame) noexcept;

[[nodiscard]] FrameReceiveResult
receive_frame(int fd, std::uint32_t max_payload = kDefaultMaxPayload);

[[nodiscard]] std::string_view frame_io_status_message(FrameIoStatus status) noexcept;

} // namespace syncwire::protocol
