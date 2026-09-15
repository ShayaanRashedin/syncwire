#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace syncwire::protocol {

inline constexpr std::size_t kUploadMetadataPrefixSize = 14U;
inline constexpr std::size_t kChunkOffsetSize = 8U;
inline constexpr std::size_t kAcknowledgmentPayloadSize = 8U;
inline constexpr std::size_t kTransferReadyPayloadSize = 12U;
inline constexpr std::string_view kPartialDirectory = ".syncwire-partials";
inline constexpr std::size_t kTransferResultPayloadSize = 1U;
inline constexpr std::size_t kMaxRemoteFilenameLength = 255U;
inline constexpr std::size_t kMaxRemotePathLength = 1024U;
inline constexpr std::size_t kDefaultChunkSize = 64U * 1024U;
inline constexpr std::uint64_t kDefaultMaxFileSize = 1024ULL * 1024ULL * 1024ULL;

enum class TransferResultCode : std::uint8_t {
    Success = 0U,
    InvalidRequest = 1U,
    InvalidPath = 2U,
    FileTooLarge = 3U,
    FileIoError = 4U,
    UnexpectedFrame = 5U,
    UnexpectedOffset = 6U,
    SizeMismatch = 7U,
    ChecksumMismatch = 8U,
};

enum class TransferCodecError {
    PayloadTooShort,
    PayloadSizeMismatch,
    EmptyFilename,
    FilenameTooLong,
    UnsafeFilename,
    ChunkHasNoData,
    UnknownResultCode,
};

struct UploadMetadata {
    std::string filename;
    std::uint64_t file_size{0U};
    std::uint32_t checksum{0U};

    [[nodiscard]] friend bool operator==(const UploadMetadata&, const UploadMetadata&) = default;
};

struct FileChunk {
    std::uint64_t offset{0U};
    std::vector<std::byte> data;

    [[nodiscard]] friend bool operator==(const FileChunk&, const FileChunk&) = default;
};

struct TransferReady {
    std::uint64_t offset{0U};
    std::uint32_t prefix_checksum{0U};

    [[nodiscard]] friend bool operator==(const TransferReady&, const TransferReady&) = default;
};

using UploadMetadataResult = std::variant<UploadMetadata, TransferCodecError>;
using FileChunkResult = std::variant<FileChunk, TransferCodecError>;
using OffsetResult = std::variant<std::uint64_t, TransferCodecError>;
using TransferCodeResult = std::variant<TransferResultCode, TransferCodecError>;
using TransferReadyResult = std::variant<TransferReady, TransferCodecError>;

[[nodiscard]] bool is_safe_remote_filename(std::string_view filename) noexcept;
[[nodiscard]] bool is_safe_remote_path(std::string_view path) noexcept;
[[nodiscard]] std::vector<std::byte> encode_upload_metadata(const UploadMetadata& metadata);
[[nodiscard]] UploadMetadataResult decode_upload_metadata(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte>
encode_file_chunk(std::uint64_t offset, std::span<const std::byte> data);
[[nodiscard]] FileChunkResult decode_file_chunk(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode_offset(std::uint64_t offset);
[[nodiscard]] OffsetResult decode_offset(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode_transfer_ready(const TransferReady& ready);
[[nodiscard]] TransferReadyResult decode_transfer_ready(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode_transfer_result(TransferResultCode code);
[[nodiscard]] TransferCodeResult decode_transfer_result(std::span<const std::byte> payload);
[[nodiscard]] std::string_view transfer_codec_error_message(TransferCodecError error) noexcept;
[[nodiscard]] std::string_view transfer_result_code_message(TransferResultCode code) noexcept;

} // namespace syncwire::protocol
