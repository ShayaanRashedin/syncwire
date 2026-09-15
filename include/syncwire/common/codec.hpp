#pragma once

#include "syncwire/common/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

namespace syncwire::protocol {

enum class CodecError {
    HeaderTooShort,
    InvalidMagic,
    UnsupportedVersion,
    InvalidHeaderSize,
    UnknownMessageType,
    UnsupportedFlags,
    PayloadTooLarge,
    InputBufferLimitExceeded,
};

using EncodedHeader = std::array<std::byte, kWireHeaderSize>;
using HeaderDecodeResult = std::variant<FrameHeader, CodecError>;

[[nodiscard]] EncodedHeader encode_header(const FrameHeader& header) noexcept;

[[nodiscard]] HeaderDecodeResult
decode_header(std::span<const std::byte> bytes,
              std::uint32_t max_payload = kDefaultMaxPayload) noexcept;

[[nodiscard]] std::string_view codec_error_message(CodecError error) noexcept;

} // namespace syncwire::protocol

