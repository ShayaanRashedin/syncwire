#include "test_harness.hpp"

#include "syncwire/common/frame_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

namespace {

using syncwire::protocol::CodecError;
using syncwire::protocol::FrameParser;
using syncwire::protocol::MessageType;

[[nodiscard]] std::vector<std::byte> bytes_of(const std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    std::transform(text.begin(), text.end(), std::back_inserter(bytes),
                   [](const char value) { return static_cast<std::byte>(value); });
    return bytes;
}

[[nodiscard]] std::vector<std::byte>
make_frame(const MessageType type, const std::uint64_t request_id,
           const std::span<const std::byte> payload = {}) {
    const syncwire::protocol::FrameHeader header{
        .message_type = type,
        .payload_length = static_cast<std::uint32_t>(payload.size()),
        .request_id = request_id,
    };
    const auto encoded_header = syncwire::protocol::encode_header(header);

    std::vector<std::byte> encoded(encoded_header.begin(), encoded_header.end());
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    return encoded;
}

void test_every_split_point(TestRunner& runner) {
    const auto payload = bytes_of("fragment me");
    const auto encoded = make_frame(MessageType::Ping, 42U, payload);
    const auto all_bytes = std::span<const std::byte>(encoded);

    for (std::size_t split = 0U; split <= encoded.size(); ++split) {
        FrameParser parser;
        const auto first = parser.consume(all_bytes.first(split));
        const auto second = parser.consume(all_bytes.subspan(split));

        const auto frame_count = first.frames.size() + second.frames.size();
        runner.expect(!first.error.has_value() && !second.error.has_value(),
                      "valid fragmented frame has no parse error");
        runner.expect(frame_count == 1U, "every two-part split emits exactly one frame");

        const auto& frames = first.frames.empty() ? second.frames : first.frames;
        runner.expect(frames.size() == 1U && frames[0U].header.request_id == 42U &&
                          frames[0U].payload == payload,
                      "fragmented frame preserves header and payload");
    }
}

void test_byte_at_a_time(TestRunner& runner) {
    const auto payload = bytes_of("abc");
    const auto encoded = make_frame(MessageType::FileChunk, 99U, payload);
    FrameParser parser;
    std::size_t frames_seen = 0U;

    for (const auto value : encoded) {
        const auto result = parser.consume(std::span<const std::byte>(&value, 1U));
        runner.expect(!result.error.has_value(), "byte-at-a-time input remains valid");
        frames_seen += result.frames.size();
        if (!result.frames.empty()) {
            runner.expect(result.frames[0U].payload == payload,
                          "byte-at-a-time frame preserves payload");
        }
    }

    runner.expect(frames_seen == 1U, "byte-at-a-time input emits one frame");
    runner.expect(parser.buffered_bytes() == 0U, "complete frame leaves no buffered bytes");
}

void test_coalesced_frames(TestRunner& runner) {
    const auto first = make_frame(MessageType::Ping, 1U);
    const auto second_payload = bytes_of("payload");
    const auto second = make_frame(MessageType::Pong, 2U, second_payload);

    std::vector<std::byte> combined(first.begin(), first.end());
    combined.insert(combined.end(), second.begin(), second.end());

    FrameParser parser;
    const auto result = parser.consume(combined);
    runner.expect(!result.error.has_value(), "coalesced frames have no parse error");
    runner.expect(result.frames.size() == 2U, "two coalesced frames are both emitted");
    runner.expect(result.frames.size() == 2U && result.frames[0U].header.request_id == 1U &&
                      result.frames[1U].header.request_id == 2U &&
                      result.frames[1U].payload == second_payload,
                  "coalesced frame order and contents are preserved");
}

void test_incomplete_payload(TestRunner& runner) {
    const auto payload = bytes_of("not complete yet");
    const auto encoded = make_frame(MessageType::FileChunk, 3U, payload);
    const auto partial_size = encoded.size() - 1U;

    FrameParser parser;
    const auto partial = parser.consume(std::span<const std::byte>(encoded).first(partial_size));
    runner.expect(partial.frames.empty() && !partial.error.has_value(),
                  "incomplete payload is retained without emission");
    runner.expect(parser.buffered_bytes() == partial_size, "incomplete bytes remain buffered");

    const auto complete = parser.consume(std::span<const std::byte>(encoded).last(1U));
    runner.expect(complete.frames.size() == 1U && complete.frames[0U].payload == payload,
                  "final payload byte completes the frame");
}

void test_terminal_errors(TestRunner& runner) {
    auto invalid = make_frame(MessageType::Ping, 4U);
    invalid[0U] = std::byte{0x00};

    FrameParser parser;
    const auto bad = parser.consume(invalid);
    runner.expect(bad.error == CodecError::InvalidMagic && parser.failed(),
                  "malformed header makes the parser terminally failed");

    const auto valid = make_frame(MessageType::Ping, 5U);
    const auto after_failure = parser.consume(valid);
    runner.expect(after_failure.frames.empty() && after_failure.error == CodecError::InvalidMagic,
                  "failed parser does not accept later input");

    FrameParser bounded_parser(64U, syncwire::protocol::kWireHeaderSize);
    const std::vector<std::byte> too_many(syncwire::protocol::kWireHeaderSize + 1U);
    const auto overflow = bounded_parser.consume(too_many);
    runner.expect(overflow.error == CodecError::InputBufferLimitExceeded,
                  "bounded parser rejects input-buffer growth");
}

} // namespace

void run_frame_parser_tests(TestRunner& runner) {
    test_every_split_point(runner);
    test_byte_at_a_time(runner);
    test_coalesced_frames(runner);
    test_incomplete_payload(runner);
    test_terminal_errors(runner);
}
