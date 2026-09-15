#include "syncwire/common/transfer_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace syncwire::protocol {
namespace {

void append_u16(std::vector<std::byte>& output, const std::uint16_t value) {
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<std::byte>(value & 0xFFU));
}

void append_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    for (unsigned shift = 24U;; shift -= 8U) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        if (shift == 0U) {
            break;
        }
    }
}

void append_u64(std::vector<std::byte>& output, const std::uint64_t value) {
    for (unsigned shift = 56U;; shift -= 8U) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        if (shift == 0U) {
            break;
        }
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
    for (std::size_t index = 0U; index < 4U; ++index) {
        value = (value << 8U) | static_cast<std::uint32_t>(get_u8(bytes, offset + index));
    }
    return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::span<const std::byte> bytes,
                                    const std::size_t offset) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(get_u8(bytes, offset + index));
    }
    return value;
}

[[nodiscard]] bool known_result_code(const std::uint8_t raw) noexcept {
    switch (static_cast<TransferResultCode>(raw)) {
    case TransferResultCode::Success:
    case TransferResultCode::InvalidRequest:
    case TransferResultCode::InvalidPath:
    case TransferResultCode::FileTooLarge:
    case TransferResultCode::FileIoError:
    case TransferResultCode::UnexpectedFrame:
    case TransferResultCode::UnexpectedOffset:
    case TransferResultCode::SizeMismatch:
    case TransferResultCode::ChecksumMismatch:
        return true;
    }
    return false;
}

} // namespace

bool is_safe_remote_filename(const std::string_view filename) noexcept {
    return filename.size() <= kMaxRemoteFilenameLength &&
           is_safe_remote_path(filename) && filename.find('/') == std::string_view::npos;
}

bool is_safe_remote_path(const std::string_view path) noexcept {
    if (path.empty() || path.size() > kMaxRemotePathLength || path.front() == '/' ||
        path.back() == '/') {
        return false;
    }
    if (std::ranges::any_of(path, [](const char character) {
            return character == '\\' || character == '\0';
        })) {
        return false;
    }

    std::size_t start = 0U;
    while (start < path.size()) {
        const auto separator = path.find('/', start);
        const auto length = separator == std::string_view::npos
                                ? path.size() - start
                                : separator - start;
        const auto component = path.substr(start, length);
        if (component.empty() || component == "." || component == ".." ||
            component == kPartialDirectory || component.size() > kMaxRemoteFilenameLength) {
            return false;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1U;
    }
    return true;
}

std::vector<std::byte> encode_upload_metadata(const UploadMetadata& metadata) {
    std::vector<std::byte> payload;
    if (!is_safe_remote_path(metadata.filename)) {
        return payload;
    }
    payload.reserve(kUploadMetadataPrefixSize + metadata.filename.size());
    append_u16(payload, static_cast<std::uint16_t>(metadata.filename.size()));
    append_u64(payload, metadata.file_size);
    append_u32(payload, metadata.checksum);
    for (const char character : metadata.filename) {
        payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return payload;
}

UploadMetadataResult decode_upload_metadata(const std::span<const std::byte> payload) {
    if (payload.size() < kUploadMetadataPrefixSize) {
        return TransferCodecError::PayloadTooShort;
    }
    const auto filename_length = static_cast<std::size_t>(get_u16(payload, 0U));
    if (filename_length == 0U) {
        return TransferCodecError::EmptyFilename;
    }
    if (filename_length > kMaxRemotePathLength) {
        return TransferCodecError::FilenameTooLong;
    }
    if (payload.size() != kUploadMetadataPrefixSize + filename_length) {
        return TransferCodecError::PayloadSizeMismatch;
    }

    std::string filename;
    filename.reserve(filename_length);
    for (std::size_t index = 0U; index < filename_length; ++index) {
        filename.push_back(
            static_cast<char>(get_u8(payload, kUploadMetadataPrefixSize + index)));
    }
    if (!is_safe_remote_path(filename)) {
        return TransferCodecError::UnsafeFilename;
    }

    return UploadMetadata{
        .filename = std::move(filename),
        .file_size = get_u64(payload, 2U),
        .checksum = get_u32(payload, 10U),
    };
}

std::vector<std::byte> encode_file_chunk(const std::uint64_t offset,
                                         const std::span<const std::byte> data) {
    std::vector<std::byte> payload;
    payload.reserve(kChunkOffsetSize + data.size());
    append_u64(payload, offset);
    payload.insert(payload.end(), data.begin(), data.end());
    return payload;
}

FileChunkResult decode_file_chunk(const std::span<const std::byte> payload) {
    if (payload.size() < kChunkOffsetSize) {
        return TransferCodecError::PayloadTooShort;
    }
    if (payload.size() == kChunkOffsetSize) {
        return TransferCodecError::ChunkHasNoData;
    }
    return FileChunk{
        .offset = get_u64(payload, 0U),
        .data = std::vector<std::byte>(payload.begin() +
                                          static_cast<std::ptrdiff_t>(kChunkOffsetSize),
                                      payload.end()),
    };
}

std::vector<std::byte> encode_offset(const std::uint64_t offset) {
    std::vector<std::byte> payload;
    payload.reserve(kAcknowledgmentPayloadSize);
    append_u64(payload, offset);
    return payload;
}

OffsetResult decode_offset(const std::span<const std::byte> payload) {
    if (payload.size() != kAcknowledgmentPayloadSize) {
        return TransferCodecError::PayloadSizeMismatch;
    }
    return get_u64(payload, 0U);
}

std::vector<std::byte> encode_transfer_ready(const TransferReady& ready) {
    auto payload = encode_offset(ready.offset);
    append_u32(payload, ready.prefix_checksum);
    return payload;
}

TransferReadyResult decode_transfer_ready(const std::span<const std::byte> payload) {
    if (payload.size() != kTransferReadyPayloadSize) {
        return TransferCodecError::PayloadSizeMismatch;
    }
    return TransferReady{.offset = get_u64(payload, 0U), .prefix_checksum = get_u32(payload, 8U)};
}

std::vector<std::byte> encode_transfer_result(const TransferResultCode code) {
    return {static_cast<std::byte>(static_cast<std::uint8_t>(code))};
}

TransferCodeResult decode_transfer_result(const std::span<const std::byte> payload) {
    if (payload.size() != kTransferResultPayloadSize) {
        return TransferCodecError::PayloadSizeMismatch;
    }
    const auto raw = get_u8(payload, 0U);
    if (!known_result_code(raw)) {
        return TransferCodecError::UnknownResultCode;
    }
    return static_cast<TransferResultCode>(raw);
}

std::string_view transfer_codec_error_message(const TransferCodecError error) noexcept {
    switch (error) {
    case TransferCodecError::PayloadTooShort:
        return "transfer payload is too short";
    case TransferCodecError::PayloadSizeMismatch:
        return "transfer payload size does not match its schema";
    case TransferCodecError::EmptyFilename:
        return "remote filename is empty";
    case TransferCodecError::FilenameTooLong:
        return "remote filename exceeds the configured limit";
    case TransferCodecError::UnsafeFilename:
        return "remote filename is unsafe";
    case TransferCodecError::ChunkHasNoData:
        return "file chunk contains no file data";
    case TransferCodecError::UnknownResultCode:
        return "transfer result code is unknown";
    }
    return "unknown transfer codec error";
}

std::string_view transfer_result_code_message(const TransferResultCode code) noexcept {
    switch (code) {
    case TransferResultCode::Success:
        return "transfer completed";
    case TransferResultCode::InvalidRequest:
        return "upload request is invalid";
    case TransferResultCode::InvalidPath:
        return "remote filename is invalid";
    case TransferResultCode::FileTooLarge:
        return "file exceeds the configured limit";
    case TransferResultCode::FileIoError:
        return "file I/O failed";
    case TransferResultCode::UnexpectedFrame:
        return "peer sent an unexpected transfer frame";
    case TransferResultCode::UnexpectedOffset:
        return "file chunk offset is not contiguous";
    case TransferResultCode::SizeMismatch:
        return "received file size does not match metadata";
    case TransferResultCode::ChecksumMismatch:
        return "received file checksum does not match metadata";
    }
    return "unknown transfer result";
}

} // namespace syncwire::protocol
