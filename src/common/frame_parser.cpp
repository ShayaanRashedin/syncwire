#include "syncwire/common/frame_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <variant>

namespace syncwire::protocol {

FrameParser::FrameParser(const std::uint32_t max_payload, const std::size_t max_buffer)
    : max_payload_(max_payload), max_buffer_(max_buffer) {
    if (max_buffer_ < kWireHeaderSize) {
        throw std::invalid_argument("max_buffer must hold at least one frame header");
    }
}

ParseBatch FrameParser::consume(const std::span<const std::byte> bytes) {
    if (terminal_error_.has_value()) {
        return ParseBatch{.frames = {}, .error = terminal_error_};
    }

    compact();
    const auto currently_buffered = buffered_bytes();
    if (bytes.size() > max_buffer_ - currently_buffered) {
        return fail(CodecError::InputBufferLimitExceeded);
    }

    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    std::vector<Frame> completed;
    while (buffered_bytes() >= kWireHeaderSize) {
        const auto available = std::span<const std::byte>(buffer_).subspan(read_offset_);
        const auto decoded = decode_header(available.first(kWireHeaderSize), max_payload_);
        if (const auto* error = std::get_if<CodecError>(&decoded); error != nullptr) {
            return fail(*error, std::move(completed));
        }

        const auto header = std::get<FrameHeader>(decoded);
        const auto frame_size = kWireHeaderSize + static_cast<std::size_t>(header.payload_length);
        if (available.size() < frame_size) {
            break;
        }

        const auto payload_begin = available.begin() + static_cast<std::ptrdiff_t>(kWireHeaderSize);
        const auto payload_end = available.begin() + static_cast<std::ptrdiff_t>(frame_size);
        completed.push_back(Frame{
            .header = header,
            .payload = std::vector<std::byte>(payload_begin, payload_end),
        });
        read_offset_ += frame_size;
    }

    compact();
    return ParseBatch{.frames = std::move(completed), .error = std::nullopt};
}

std::size_t FrameParser::buffered_bytes() const noexcept {
    return buffer_.size() - read_offset_;
}

bool FrameParser::failed() const noexcept {
    return terminal_error_.has_value();
}

std::optional<CodecError> FrameParser::terminal_error() const noexcept {
    return terminal_error_;
}

void FrameParser::compact() {
    if (read_offset_ == 0U) {
        return;
    }

    if (read_offset_ == buffer_.size()) {
        buffer_.clear();
        read_offset_ = 0U;
        return;
    }

    if (read_offset_ >= buffer_.size() / 2U) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(read_offset_));
        read_offset_ = 0U;
    }
}

ParseBatch FrameParser::fail(const CodecError error, std::vector<Frame> completed) {
    terminal_error_ = error;
    return ParseBatch{.frames = std::move(completed), .error = error};
}

} // namespace syncwire::protocol

