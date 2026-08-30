#pragma once

#include <cstddef>
#include <cstdint>

namespace syncwire::protocol {

inline constexpr std::uint32_t kMagic = 0x53574952U; // ASCII: SWIR
inline constexpr std::uint8_t kVersion = 1U;
inline constexpr std::size_t kWireHeaderSize = 32U;
inline constexpr std::uint32_t kDefaultMaxPayload = 1024U * 1024U;

enum class MessageType : std::uint8_t {
    Ping = 0x01,
    Pong = 0x02,

    AuthChallenge = 0x10,
    AuthProof = 0x11,
    AuthResult = 0x12,

    UploadRequest = 0x20,
    DownloadRequest = 0x21,
    TransferReady = 0x22,
    FileChunk = 0x23,
    Acknowledgment = 0x24,
    TransferComplete = 0x25,
    TransferResult = 0x26,

    Error = 0x7F,
};

[[nodiscard]] constexpr bool is_known_message_type(const std::uint8_t value) noexcept {
    switch (static_cast<MessageType>(value)) {
    case MessageType::Ping:
    case MessageType::Pong:
    case MessageType::AuthChallenge:
    case MessageType::AuthProof:
    case MessageType::AuthResult:
    case MessageType::UploadRequest:
    case MessageType::DownloadRequest:
    case MessageType::TransferReady:
    case MessageType::FileChunk:
    case MessageType::Acknowledgment:
    case MessageType::TransferComplete:
    case MessageType::TransferResult:
    case MessageType::Error:
        return true;
    }
    return false;
}

struct FrameHeader {
    std::uint32_t magic{kMagic};
    std::uint8_t version{kVersion};
    MessageType message_type{MessageType::Ping};
    std::uint16_t header_size{static_cast<std::uint16_t>(kWireHeaderSize)};
    std::uint32_t payload_length{0U};
    std::uint32_t flags{0U};
    std::uint64_t request_id{0U};
    std::uint64_t transfer_id{0U};

    [[nodiscard]] friend constexpr bool operator==(const FrameHeader&, const FrameHeader&) = default;
};

} // namespace syncwire::protocol

