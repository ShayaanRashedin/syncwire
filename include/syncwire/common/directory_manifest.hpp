#pragma once

#include "syncwire/common/transfer_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace syncwire {

struct FileRecord {
    std::string path;
    std::uint64_t size{0U};
    std::uint32_t checksum{0U};

    [[nodiscard]] friend bool operator==(const FileRecord&, const FileRecord&) = default;
};

using FileManifest = std::vector<FileRecord>;

struct DirectoryScanLimits {
    std::size_t max_entries{4'096U};
    std::uint64_t max_file_size{protocol::kDefaultMaxFileSize};
    std::size_t max_encoded_size{protocol::kDefaultMaxPayload};
};

enum class DirectoryScanError {
    RootNotDirectory,
    FileIoError,
    UnsafeRelativePath,
    TooManyEntries,
    FileTooLarge,
    ManifestTooLarge,
};

struct DirectoryScanResult {
    FileManifest files;
    std::optional<DirectoryScanError> error{};
    std::size_t skipped_symlinks{0U};
    int system_error{0};

    [[nodiscard]] bool ok() const noexcept {
        return !error.has_value();
    }
};

[[nodiscard]] DirectoryScanResult
scan_directory(const std::filesystem::path& root, DirectoryScanLimits limits = {});

[[nodiscard]] std::string_view directory_scan_error_message(DirectoryScanError error) noexcept;

} // namespace syncwire
