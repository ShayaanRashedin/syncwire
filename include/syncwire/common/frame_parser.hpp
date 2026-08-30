#pragma once

#include "syncwire/common/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace syncwire::protocol {

struct Frame {
    FrameHeader header;
    std::vector<std::byte> payload;
};

struct ParseBatch {
    std::vector<Frame> frames;
    std::optional<CodecError> error;
};

class FrameParser {
public:
    explicit FrameParser(
        std::uint32_t max_payload = kDefaultMaxPayload,
        std::size_t max_buffer = 2U * (kWireHeaderSize + kDefaultMaxPayload));

    [[nodiscard]] ParseBatch consume(std::span<const std::byte> bytes);

    [[nodiscard]] std::size_t buffered_bytes() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::optional<CodecError> terminal_error() const noexcept;

private:
    void compact();
    [[nodiscard]] ParseBatch fail(CodecError error, std::vector<Frame> completed = {});

    std::uint32_t max_payload_;
    std::size_t max_buffer_;
    std::vector<std::byte> buffer_;
    std::size_t read_offset_{0U};
    std::optional<CodecError> terminal_error_;
};

} // namespace syncwire::protocol

