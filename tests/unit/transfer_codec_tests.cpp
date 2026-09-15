#include "test_harness.hpp"

#include "syncwire/common/crc32.hpp"
#include "syncwire/common/transfer_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace {

void test_crc32_known_vector(TestRunner& runner) {
    constexpr std::string_view input = "123456789";
    const auto bytes = std::as_bytes(std::span(input.data(), input.size()));
    runner.expect(syncwire::crc32(bytes) == 0xCBF4'3926U,
                  "CRC-32 matches the standard check vector");
    runner.expect(syncwire::crc32({}) == 0U, "CRC-32 of empty input is zero");
}

void test_upload_metadata_round_trip(TestRunner& runner) {
    const syncwire::protocol::UploadMetadata metadata{
        .filename = "report.bin",
        .file_size = 0x0102'0304'0506'0708ULL,
        .checksum = 0x89AB'CDEFU,
    };
    const auto payload = syncwire::protocol::encode_upload_metadata(metadata);
    const auto decoded = syncwire::protocol::decode_upload_metadata(payload);
    runner.expect(std::holds_alternative<syncwire::protocol::UploadMetadata>(decoded),
                  "upload metadata decodes");
    if (const auto* value = std::get_if<syncwire::protocol::UploadMetadata>(&decoded);
        value != nullptr) {
        runner.expect(*value == metadata, "upload metadata preserves filename, size, and checksum");
    }
}

void test_transfer_payloads_round_trip(TestRunner& runner) {
    const std::vector<std::byte> data{
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
    };
    const auto chunk_payload = syncwire::protocol::encode_file_chunk(42U, data);
    const auto decoded_chunk = syncwire::protocol::decode_file_chunk(chunk_payload);
    runner.expect(std::holds_alternative<syncwire::protocol::FileChunk>(decoded_chunk),
                  "file chunk decodes");
    if (const auto* chunk = std::get_if<syncwire::protocol::FileChunk>(&decoded_chunk);
        chunk != nullptr) {
        runner.expect(chunk->offset == 42U && chunk->data == data,
                      "file chunk preserves offset and bytes");
    }

    const auto offset_payload = syncwire::protocol::encode_offset(9'876U);
    const auto decoded_offset = syncwire::protocol::decode_offset(offset_payload);
    runner.expect(std::holds_alternative<std::uint64_t>(decoded_offset) &&
                      std::get<std::uint64_t>(decoded_offset) == 9'876U,
                  "acknowledgment offset round-trips");

    const auto result_payload = syncwire::protocol::encode_transfer_result(
        syncwire::protocol::TransferResultCode::ChecksumMismatch);
    const auto decoded_result = syncwire::protocol::decode_transfer_result(result_payload);
    runner.expect(std::holds_alternative<syncwire::protocol::TransferResultCode>(decoded_result) &&
                      std::get<syncwire::protocol::TransferResultCode>(decoded_result) ==
                          syncwire::protocol::TransferResultCode::ChecksumMismatch,
                  "transfer result code round-trips");
}

void test_invalid_transfer_payloads(TestRunner& runner) {
    runner.expect(syncwire::protocol::is_safe_remote_path("nested/report.bin"),
                  "normalized nested remote path is accepted");
    runner.expect(!syncwire::protocol::is_safe_remote_filename("nested/report.bin"),
                  "single-file CLI name still rejects directory components");
    auto unsafe_payload = syncwire::protocol::encode_upload_metadata(
        syncwire::protocol::UploadMetadata{.filename = "safe", .file_size = 1U});
    unsafe_payload[syncwire::protocol::kUploadMetadataPrefixSize] = std::byte{0x2E};
    unsafe_payload[syncwire::protocol::kUploadMetadataPrefixSize + 1U] = std::byte{0x2E};
    unsafe_payload[syncwire::protocol::kUploadMetadataPrefixSize + 2U] = std::byte{0x2F};
    const auto unsafe = syncwire::protocol::decode_upload_metadata(unsafe_payload);
    runner.expect(std::holds_alternative<syncwire::protocol::TransferCodecError>(unsafe) &&
                      std::get<syncwire::protocol::TransferCodecError>(unsafe) ==
                          syncwire::protocol::TransferCodecError::UnsafeFilename,
                  "path traversal metadata is rejected");

    const std::vector<std::byte> empty_chunk(syncwire::protocol::kChunkOffsetSize);
    const auto chunk = syncwire::protocol::decode_file_chunk(empty_chunk);
    runner.expect(std::holds_alternative<syncwire::protocol::TransferCodecError>(chunk) &&
                      std::get<syncwire::protocol::TransferCodecError>(chunk) ==
                          syncwire::protocol::TransferCodecError::ChunkHasNoData,
                  "chunk without data is rejected");

    const std::vector<std::byte> unknown_result{std::byte{0xFF}};
    const auto result = syncwire::protocol::decode_transfer_result(unknown_result);
    runner.expect(std::holds_alternative<syncwire::protocol::TransferCodecError>(result) &&
                      std::get<syncwire::protocol::TransferCodecError>(result) ==
                          syncwire::protocol::TransferCodecError::UnknownResultCode,
                  "unknown transfer result is rejected");
}

} // namespace

void run_transfer_codec_tests(TestRunner& runner) {
    test_crc32_known_vector(runner);
    test_upload_metadata_round_trip(runner);
    test_transfer_payloads_round_trip(runner);
    test_invalid_transfer_payloads(runner);
}
