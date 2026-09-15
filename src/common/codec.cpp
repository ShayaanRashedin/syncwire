#include "syncwire/common/codec.hpp"

#include <cstddef>
#include <cstdint>

namespace syncwire::protocol {
namespace {

void put_u16(EncodedHeader& out, const std::size_t offset, const std::uint16_t value) noexcept {
    out[offset] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    out[offset + 1U] = static_cast<std::byte>(value & 0xFFU);
}

void put_u32(EncodedHeader& out, const std::size_t offset, const std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4U; ++index) {
        const auto shift = static_cast<unsigned>((3U - index) * 8U);
        out[offset + index] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

void put_u64(EncodedHeader& out, const std::size_t offset, const std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8U; ++index) {
        const auto shift = static_cast<unsigned>((7U - index) * 8U);
        out[offset + index] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

[[nodiscard]] std::uint8_t get_u8(const std::span<const std::byte> bytes,
                                  const std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] std::uint16_t get_u16(const std::span<const std::byte> bytes,
                                    const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(get_u8(bytes, offset)) << 8U) |
                                      static_cast<std::uint16_t>(get_u8(bytes, offset + 1U)));
}

[[nodiscard]] std::uint32_t get_u32(const std::span<const std::byte> bytes,
                                    const std::size_t offset) noexcept {
    std::uint32_t value = 0U;
    for (std::size_t index = 0; index < 4U; ++index) {
        value = (value << 8U) | static_cast<std::uint32_t>(get_u8(bytes, offset + index));
    }
    return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::span<const std::byte> bytes,
                                    const std::size_t offset) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(get_u8(bytes, offset + index));
    }
    return value;
}

} // namespace

EncodedHeader encode_header(const FrameHeader& header) noexcept {
    EncodedHeader bytes{};
    put_u32(bytes, 0U, header.magic);
    bytes[4U] = static_cast<std::byte>(header.version);
    bytes[5U] = static_cast<std::byte>(static_cast<std::uint8_t>(header.message_type));
    put_u16(bytes, 6U, header.header_size);
    put_u32(bytes, 8U, header.payload_length);
    put_u32(bytes, 12U, header.flags);
    put_u64(bytes, 16U, header.request_id);
    put_u64(bytes, 24U, header.transfer_id);
    return bytes;
}

HeaderDecodeResult decode_header(const std::span<const std::byte> bytes,
                                 const std::uint32_t max_payload) noexcept {
    if (bytes.size() < kWireHeaderSize) {
        return CodecError::HeaderTooShort;
    }

    const auto magic = get_u32(bytes, 0U);
    if (magic != kMagic) {
        return CodecError::InvalidMagic;
    }

    const auto version = get_u8(bytes, 4U);
    if (version != kVersion) {
        return CodecError::UnsupportedVersion;
    }

    const auto raw_message_type = get_u8(bytes, 5U);
    if (!is_known_message_type(raw_message_type)) {
        return CodecError::UnknownMessageType;
    }

    const auto header_size = get_u16(bytes, 6U);
    if (header_size != static_cast<std::uint16_t>(kWireHeaderSize)) {
        return CodecError::InvalidHeaderSize;
    }

    const auto payload_length = get_u32(bytes, 8U);
    if (payload_length > max_payload) {
        return CodecError::PayloadTooLarge;
    }

    const auto flags = get_u32(bytes, 12U);
    if (flags != 0U) {
        return CodecError::UnsupportedFlags;
    }

    return FrameHeader{
        .magic = magic,
        .version = version,
        .message_type = static_cast<MessageType>(raw_message_type),
        .header_size = header_size,
        .payload_length = payload_length,
        .flags = flags,
        .request_id = get_u64(bytes, 16U),
        .transfer_id = get_u64(bytes, 24U),
    };
}

std::string_view codec_error_message(const CodecError error) noexcept {
    switch (error) {
    case CodecError::HeaderTooShort:
        return "frame header is incomplete";
    case CodecError::InvalidMagic:
        return "frame magic is invalid";
    case CodecError::UnsupportedVersion:
        return "protocol version is unsupported";
    case CodecError::InvalidHeaderSize:
        return "frame header size is invalid";
    case CodecError::UnknownMessageType:
        return "message type is unknown";
    case CodecError::UnsupportedFlags:
        return "frame uses unsupported flags";
    case CodecError::PayloadTooLarge:
        return "frame payload exceeds the configured limit";
    case CodecError::InputBufferLimitExceeded:
        return "connection input buffer exceeds the configured limit";
    }
    return "unknown codec error";
}

} // namespace syncwire::protocol
