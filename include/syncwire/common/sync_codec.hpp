#pragma once

#include "syncwire/common/directory_manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace syncwire::protocol {

inline constexpr std::size_t kManifestCountSize = 4U;
inline constexpr std::size_t kManifestRecordPrefixSize = 14U;
inline constexpr std::size_t kSyncPlanPrefixSize = 12U;

enum class SyncCodecError {
    PayloadTooShort,
    PayloadSizeMismatch,
    TooManyEntries,
    PathTooLong,
    UnsafePath,
    PathsNotStrictlySorted,
    UnknownResultCode,
};

enum class SyncResultCode : std::uint8_t {
    Success = 0U,
    DestinationChanged = 1U,
};

struct SyncPlan {
    std::vector<std::string> upload_paths;
    std::uint32_t unchanged_count{0U};
    std::uint32_t server_only_count{0U};

    [[nodiscard]] friend bool operator==(const SyncPlan&, const SyncPlan&) = default;
};

using ManifestDecodeResult = std::variant<FileManifest, SyncCodecError>;
using SyncPlanDecodeResult = std::variant<SyncPlan, SyncCodecError>;
using SyncResultDecodeResult = std::variant<SyncResultCode, SyncCodecError>;

[[nodiscard]] std::vector<std::byte> encode_manifest(std::span<const FileRecord> files);
[[nodiscard]] ManifestDecodeResult
decode_manifest(std::span<const std::byte> payload, std::size_t max_entries = 4'096U);

[[nodiscard]] SyncPlan build_sync_plan(std::span<const FileRecord> source,
                                       std::span<const FileRecord> destination);
[[nodiscard]] std::vector<std::byte> encode_sync_plan(const SyncPlan& plan);
[[nodiscard]] SyncPlanDecodeResult
decode_sync_plan(std::span<const std::byte> payload, std::size_t max_entries = 4'096U);

[[nodiscard]] std::vector<std::byte> encode_sync_result(SyncResultCode code);
[[nodiscard]] SyncResultDecodeResult decode_sync_result(std::span<const std::byte> payload);
[[nodiscard]] std::string_view sync_codec_error_message(SyncCodecError error) noexcept;

} // namespace syncwire::protocol
