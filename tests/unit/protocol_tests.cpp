#include "test_harness.hpp"

#include "syncwire/common/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace {

using syncwire::protocol::CodecError;
using syncwire::protocol::FrameHeader;
using syncwire::protocol::HeaderDecodeResult;
using syncwire::protocol::MessageType;

[[nodiscard]] bool has_error(const HeaderDecodeResult& result, const CodecError expected) {
    const auto* error = std::get_if<CodecError>(&result);
    return error != nullptr && *error == expected;
}

void test_header_round_trip(TestRunner& runner) {
    const FrameHeader original{
        .magic = syncwire::protocol::kMagic,
        .version = syncwire::protocol::kVersion,
        .message_type = MessageType::FileChunk,
        .header_size = static_cast<std::uint16_t>(syncwire::protocol::kWireHeaderSize),
        .payload_length = 65'536U,
        .flags = 0U,
        .request_id = 0x0102'0304'0506'0708ULL,
        .transfer_id = 0x1112'1314'1516'1718ULL,
    };

    const auto encoded = syncwire::protocol::encode_header(original);
    const auto decoded = syncwire::protocol::decode_header(encoded);
    const auto* header = std::get_if<FrameHeader>(&decoded);

    runner.expect(header != nullptr, "valid header decodes");
    runner.expect(header != nullptr && *header == original, "header survives a round trip");
    runner.expect(std::to_integer<std::uint8_t>(encoded[0U]) == 0x53U &&
                      std::to_integer<std::uint8_t>(encoded[1U]) == 0x57U &&
                      std::to_integer<std::uint8_t>(encoded[2U]) == 0x49U &&
                      std::to_integer<std::uint8_t>(encoded[3U]) == 0x52U,
                  "magic is encoded in network byte order");
    runner.expect(std::to_integer<std::uint8_t>(encoded[16U]) == 0x01U &&
                      std::to_integer<std::uint8_t>(encoded[23U]) == 0x08U,
                  "64-bit request ID is encoded in network byte order");
}

void test_header_validation(TestRunner& runner) {
    const FrameHeader valid{
        .message_type = MessageType::Ping,
        .request_id = 7U,
    };

    auto encoded = syncwire::protocol::encode_header(valid);
    runner.expect(has_error(syncwire::protocol::decode_header(
                                std::span<const std::byte>(encoded).first(31U)),
                            CodecError::HeaderTooShort),
                  "short header is rejected");

    encoded = syncwire::protocol::encode_header(valid);
    encoded[0U] = std::byte{0x00};
    runner.expect(has_error(syncwire::protocol::decode_header(encoded), CodecError::InvalidMagic),
                  "invalid magic is rejected");

    encoded = syncwire::protocol::encode_header(valid);
    encoded[4U] = std::byte{0x01};
    runner.expect(has_error(syncwire::protocol::decode_header(encoded),
                            CodecError::UnsupportedVersion),
                  "legacy v1 is rejected instead of misreading resume metadata");

    encoded = syncwire::protocol::encode_header(valid);
    encoded[5U] = std::byte{0x55};
    runner.expect(has_error(syncwire::protocol::decode_header(encoded),
                            CodecError::UnknownMessageType),
                  "unknown message type is rejected");

    encoded = syncwire::protocol::encode_header(valid);
    encoded[7U] = std::byte{0x1F};
    runner.expect(has_error(syncwire::protocol::decode_header(encoded),
                            CodecError::InvalidHeaderSize),
                  "wrong header size is rejected");

    encoded = syncwire::protocol::encode_header(valid);
    encoded[15U] = std::byte{0x01};
    runner.expect(has_error(syncwire::protocol::decode_header(encoded),
                            CodecError::UnsupportedFlags),
                  "unsupported flags are rejected");

    auto oversized = valid;
    oversized.payload_length = 1025U;
    encoded = syncwire::protocol::encode_header(oversized);
    runner.expect(has_error(syncwire::protocol::decode_header(encoded, 1024U),
                            CodecError::PayloadTooLarge),
                  "oversized payload is rejected before allocation");
}

} // namespace

void run_protocol_tests(TestRunner& runner) {
    test_header_round_trip(runner);
    test_header_validation(runner);
}
