#include "syncwire/common/frame_io.hpp"

#include "syncwire/common/codec.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace syncwire::protocol {
namespace {

[[nodiscard]] FrameIoResult from_io_result(const net::IoResult& result,
                                           const std::size_t prefix = 0U) noexcept {
    FrameIoStatus status = FrameIoStatus::Complete;
    if (result.status == net::IoStatus::PeerClosed) {
        status = FrameIoStatus::PeerClosed;
    } else if (result.status == net::IoStatus::SystemError) {
        status = FrameIoStatus::SystemError;
    }

    return FrameIoResult{
        .status = status,
        .transferred = prefix + result.transferred,
        .system_error = result.system_error,
    };
}

} // namespace

FrameIoResult send_frame(const int fd, const Frame& frame) noexcept {
    if (frame.payload.size() != static_cast<std::size_t>(frame.header.payload_length)) {
        return FrameIoResult{.status = FrameIoStatus::InvalidFrame};
    }

    const auto encoded_header = encode_header(frame.header);
    const auto decoded_header = decode_header(encoded_header);
    if (const auto* error = std::get_if<CodecError>(&decoded_header); error != nullptr) {
        return FrameIoResult{
            .status = FrameIoStatus::InvalidFrame,
            .codec_error = *error,
        };
    }

    const auto header_result = net::send_all(fd, encoded_header);
    if (!header_result.ok()) {
        return from_io_result(header_result);
    }

    const auto payload_result = net::send_all(fd, frame.payload);
    if (!payload_result.ok()) {
        return from_io_result(payload_result, kWireHeaderSize);
    }

    return FrameIoResult{
        .status = FrameIoStatus::Complete,
        .transferred = kWireHeaderSize + frame.payload.size(),
    };
}

FrameReceiveResult receive_frame(const int fd, const std::uint32_t max_payload) {
    std::array<std::byte, kWireHeaderSize> header_bytes{};
    const auto header_result = net::recv_exact(fd, header_bytes);
    if (!header_result.ok()) {
        return from_io_result(header_result);
    }

    const auto decoded_header = decode_header(header_bytes, max_payload);
    if (const auto* error = std::get_if<CodecError>(&decoded_header); error != nullptr) {
        return FrameIoResult{
            .status = FrameIoStatus::InvalidHeader,
            .transferred = kWireHeaderSize,
            .codec_error = *error,
        };
    }

    const auto header = std::get<FrameHeader>(decoded_header);
    std::vector<std::byte> payload(static_cast<std::size_t>(header.payload_length));
    const auto payload_result = net::recv_exact(fd, payload);
    if (!payload_result.ok()) {
        return from_io_result(payload_result, kWireHeaderSize);
    }

    return Frame{.header = header, .payload = std::move(payload)};
}

std::string_view frame_io_status_message(const FrameIoStatus status) noexcept {
    switch (status) {
    case FrameIoStatus::Complete:
        return "frame I/O completed";
    case FrameIoStatus::PeerClosed:
        return "peer closed during a frame";
    case FrameIoStatus::SystemError:
        return "system error during frame I/O";
    case FrameIoStatus::InvalidHeader:
        return "received an invalid frame header";
    case FrameIoStatus::InvalidFrame:
        return "attempted to send an invalid frame";
    }
    return "unknown frame I/O status";
}

} // namespace syncwire::protocol

