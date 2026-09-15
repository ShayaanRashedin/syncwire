#include "syncwire/common/sync_codec.hpp"

#include "syncwire/common/directory_manifest.hpp"
#include "syncwire/common/transfer_codec.hpp"

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

[[nodiscard]] bool append_path(std::vector<std::byte>& output, const std::string_view path) {
    if (!is_safe_remote_path(path) || path.size() > kMaxRemotePathLength ||
        path.size() > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    append_u16(output, static_cast<std::uint16_t>(path.size()));
    for (const char character : path) {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return true;
}

[[nodiscard]] std::variant<std::string, SyncCodecError>
decode_path(const std::span<const std::byte> payload, std::size_t& offset) {
    if (offset > payload.size() || payload.size() - offset < sizeof(std::uint16_t)) {
        return SyncCodecError::PayloadTooShort;
    }
    const auto path_size = static_cast<std::size_t>(get_u16(payload, offset));
    offset += sizeof(std::uint16_t);
    if (path_size == 0U || path_size > kMaxRemotePathLength) {
        return SyncCodecError::PathTooLong;
    }
    if (offset > payload.size() || path_size > payload.size() - offset) {
        return SyncCodecError::PayloadTooShort;
    }

    std::string path;
    path.reserve(path_size);
    for (std::size_t index = 0U; index < path_size; ++index) {
        path.push_back(static_cast<char>(get_u8(payload, offset + index)));
    }
    offset += path_size;
    if (!is_safe_remote_path(path)) {
        return SyncCodecError::UnsafePath;
    }
    return path;
}

[[nodiscard]] bool strictly_after(const std::string_view previous,
                                  const std::string_view current) noexcept {
    return previous.empty() || previous < current;
}

} // namespace

std::vector<std::byte> encode_manifest(const std::span<const FileRecord> files) {
    if (files.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }
    std::vector<std::byte> payload;
    append_u32(payload, static_cast<std::uint32_t>(files.size()));
    std::string_view previous;
    for (const auto& file : files) {
        if (!strictly_after(previous, file.path)) {
            return {};
        }
        append_u64(payload, file.size);
        append_u32(payload, file.checksum);
        if (!append_path(payload, file.path)) {
            return {};
        }
        previous = file.path;
    }
    return payload;
}

ManifestDecodeResult decode_manifest(const std::span<const std::byte> payload,
                                     const std::size_t max_entries) {
    if (payload.size() < kManifestCountSize) {
        return SyncCodecError::PayloadTooShort;
    }
    const auto count = static_cast<std::size_t>(get_u32(payload, 0U));
    if (count > max_entries) {
        return SyncCodecError::TooManyEntries;
    }

    FileManifest files;
    files.reserve(count);
    std::size_t offset = kManifestCountSize;
    std::string previous;
    for (std::size_t index = 0U; index < count; ++index) {
        if (offset > payload.size() ||
            payload.size() - offset < sizeof(std::uint64_t) + sizeof(std::uint32_t)) {
            return SyncCodecError::PayloadTooShort;
        }
        const auto size = get_u64(payload, offset);
        const auto checksum = get_u32(payload, offset + sizeof(std::uint64_t));
        offset += sizeof(std::uint64_t) + sizeof(std::uint32_t);
        auto decoded_path = decode_path(payload, offset);
        if (const auto* error = std::get_if<SyncCodecError>(&decoded_path); error != nullptr) {
            return *error;
        }
        auto path = std::get<std::string>(std::move(decoded_path));
        if (!strictly_after(previous, path)) {
            return SyncCodecError::PathsNotStrictlySorted;
        }
        previous = path;
        files.push_back(FileRecord{
            .path = std::move(path),
            .size = size,
            .checksum = checksum,
        });
    }
    if (offset != payload.size()) {
        return SyncCodecError::PayloadSizeMismatch;
    }
    return files;
}

SyncPlan build_sync_plan(const std::span<const FileRecord> source,
                         const std::span<const FileRecord> destination) {
    SyncPlan plan;
    std::size_t source_index = 0U;
    std::size_t destination_index = 0U;
    while (source_index < source.size() && destination_index < destination.size()) {
        const auto& source_file = source[source_index];
        const auto& destination_file = destination[destination_index];
        if (source_file.path < destination_file.path) {
            plan.upload_paths.push_back(source_file.path);
            ++source_index;
        } else if (destination_file.path < source_file.path) {
            ++plan.server_only_count;
            ++destination_index;
        } else {
            if (source_file.size == destination_file.size &&
                source_file.checksum == destination_file.checksum) {
                ++plan.unchanged_count;
            } else {
                plan.upload_paths.push_back(source_file.path);
            }
            ++source_index;
            ++destination_index;
        }
    }
    while (source_index < source.size()) {
        plan.upload_paths.push_back(source[source_index].path);
        ++source_index;
    }
    const auto remaining_server_files = destination.size() - destination_index;
    plan.server_only_count += static_cast<std::uint32_t>(remaining_server_files);
    return plan;
}

std::vector<std::byte> encode_sync_plan(const SyncPlan& plan) {
    if (plan.upload_paths.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }
    std::vector<std::byte> payload;
    append_u32(payload, static_cast<std::uint32_t>(plan.upload_paths.size()));
    append_u32(payload, plan.unchanged_count);
    append_u32(payload, plan.server_only_count);
    std::string_view previous;
    for (const auto& path : plan.upload_paths) {
        if (!strictly_after(previous, path) || !append_path(payload, path)) {
            return {};
        }
        previous = path;
    }
    return payload;
}

SyncPlanDecodeResult decode_sync_plan(const std::span<const std::byte> payload,
                                      const std::size_t max_entries) {
    if (payload.size() < kSyncPlanPrefixSize) {
        return SyncCodecError::PayloadTooShort;
    }
    const auto count = static_cast<std::size_t>(get_u32(payload, 0U));
    if (count > max_entries) {
        return SyncCodecError::TooManyEntries;
    }

    SyncPlan plan{
        .upload_paths = {},
        .unchanged_count = get_u32(payload, 4U),
        .server_only_count = get_u32(payload, 8U),
    };
    plan.upload_paths.reserve(count);
    std::size_t offset = kSyncPlanPrefixSize;
    std::string previous;
    for (std::size_t index = 0U; index < count; ++index) {
        auto decoded_path = decode_path(payload, offset);
        if (const auto* error = std::get_if<SyncCodecError>(&decoded_path); error != nullptr) {
            return *error;
        }
        auto path = std::get<std::string>(std::move(decoded_path));
        if (!strictly_after(previous, path)) {
            return SyncCodecError::PathsNotStrictlySorted;
        }
        previous = path;
        plan.upload_paths.push_back(std::move(path));
    }
    if (offset != payload.size()) {
        return SyncCodecError::PayloadSizeMismatch;
    }
    return plan;
}

std::vector<std::byte> encode_sync_result(const SyncResultCode code) {
    return {static_cast<std::byte>(static_cast<std::uint8_t>(code))};
}

SyncResultDecodeResult decode_sync_result(const std::span<const std::byte> payload) {
    if (payload.size() != 1U) {
        return SyncCodecError::PayloadSizeMismatch;
    }
    const auto raw = get_u8(payload, 0U);
    if (raw > static_cast<std::uint8_t>(SyncResultCode::DestinationChanged)) {
        return SyncCodecError::UnknownResultCode;
    }
    return static_cast<SyncResultCode>(raw);
}

std::string_view sync_codec_error_message(const SyncCodecError error) noexcept {
    switch (error) {
    case SyncCodecError::PayloadTooShort:
        return "sync payload is too short";
    case SyncCodecError::PayloadSizeMismatch:
        return "sync payload size does not match its schema";
    case SyncCodecError::TooManyEntries:
        return "sync payload contains too many entries";
    case SyncCodecError::PathTooLong:
        return "sync path is empty or too long";
    case SyncCodecError::UnsafePath:
        return "sync path is unsafe";
    case SyncCodecError::PathsNotStrictlySorted:
        return "sync paths are duplicated or not sorted";
    case SyncCodecError::UnknownResultCode:
        return "sync result code is unknown";
    }
    return "unknown sync codec error";
}

} // namespace syncwire::protocol
