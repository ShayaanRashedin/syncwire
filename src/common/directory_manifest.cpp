#include "syncwire/common/directory_manifest.hpp"

#include "syncwire/common/crc32.hpp"
#include "syncwire/common/transfer_codec.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <system_error>

namespace syncwire {
namespace {

[[nodiscard]] DirectoryScanResult failure(const DirectoryScanError error,
                                          const int system_error = 0) {
    return DirectoryScanResult{
        .files = {},
        .error = error,
        .skipped_symlinks = 0U,
        .system_error = system_error,
    };
}

[[nodiscard]] bool checksum_file(const std::filesystem::path& path,
                                 std::uint32_t& checksum,
                                 int& system_error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        system_error = errno;
        return false;
    }

    Crc32 running;
    std::array<std::byte, protocol::kDefaultChunkSize> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            running.update(std::span<const std::byte>(
                buffer.data(), static_cast<std::size_t>(count)));
        }
    }
    if (!input.eof()) {
        system_error = errno;
        return false;
    }
    checksum = running.value();
    return true;
}

} // namespace

DirectoryScanResult scan_directory(const std::filesystem::path& root,
                                   const DirectoryScanLimits limits) {
    std::error_code filesystem_error;
    const auto root_status = std::filesystem::symlink_status(root, filesystem_error);
    if (filesystem_error ==
        std::make_error_code(std::errc::no_such_file_or_directory)) {
        return failure(DirectoryScanError::RootNotDirectory);
    }
    if (filesystem_error) {
        return failure(DirectoryScanError::FileIoError, filesystem_error.value());
    }
    if (!std::filesystem::exists(root_status) || std::filesystem::is_symlink(root_status) ||
        !std::filesystem::is_directory(root_status)) {
        return failure(DirectoryScanError::RootNotDirectory);
    }

    DirectoryScanResult result;
    std::size_t encoded_size = sizeof(std::uint32_t);
    std::filesystem::recursive_directory_iterator iterator(root, filesystem_error);
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        if (filesystem_error) {
            return failure(DirectoryScanError::FileIoError, filesystem_error.value());
        }

        const auto status = iterator->symlink_status(filesystem_error);
        if (filesystem_error) {
            return failure(DirectoryScanError::FileIoError, filesystem_error.value());
        }
        if (std::filesystem::is_symlink(status)) {
            iterator.disable_recursion_pending();
            ++result.skipped_symlinks;
            iterator.increment(filesystem_error);
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            iterator.increment(filesystem_error);
            continue;
        }
        if (result.files.size() >= limits.max_entries) {
            return failure(DirectoryScanError::TooManyEntries);
        }

        const auto relative = std::filesystem::relative(iterator->path(), root, filesystem_error);
        if (filesystem_error) {
            return failure(DirectoryScanError::FileIoError, filesystem_error.value());
        }
        const auto relative_path = relative.generic_string();
        if (!protocol::is_safe_remote_path(relative_path)) {
            return failure(DirectoryScanError::UnsafeRelativePath);
        }

        const auto raw_size = iterator->file_size(filesystem_error);
        if (filesystem_error) {
            return failure(DirectoryScanError::FileIoError, filesystem_error.value());
        }
        if (raw_size > limits.max_file_size) {
            return failure(DirectoryScanError::FileTooLarge);
        }
        const std::size_t record_size = sizeof(std::uint16_t) + sizeof(std::uint64_t) +
                                        sizeof(std::uint32_t) + relative_path.size();
        if (record_size > limits.max_encoded_size ||
            encoded_size > limits.max_encoded_size - record_size) {
            return failure(DirectoryScanError::ManifestTooLarge);
        }

        std::uint32_t checksum = 0U;
        int system_error = 0;
        if (!checksum_file(iterator->path(), checksum, system_error)) {
            return failure(DirectoryScanError::FileIoError, system_error);
        }
        result.files.push_back(FileRecord{
            .path = relative_path,
            .size = static_cast<std::uint64_t>(raw_size),
            .checksum = checksum,
        });
        encoded_size += record_size;
        iterator.increment(filesystem_error);
    }
    if (filesystem_error) {
        return failure(DirectoryScanError::FileIoError, filesystem_error.value());
    }

    std::ranges::sort(result.files, {}, &FileRecord::path);
    return result;
}

std::string_view directory_scan_error_message(const DirectoryScanError error) noexcept {
    switch (error) {
    case DirectoryScanError::RootNotDirectory:
        return "scan root is not a real directory";
    case DirectoryScanError::FileIoError:
        return "directory scan encountered a file I/O error";
    case DirectoryScanError::UnsafeRelativePath:
        return "directory contains an unsafe relative path";
    case DirectoryScanError::TooManyEntries:
        return "directory contains too many files";
    case DirectoryScanError::FileTooLarge:
        return "directory contains a file larger than the configured limit";
    case DirectoryScanError::ManifestTooLarge:
        return "encoded directory manifest exceeds the frame limit";
    }
    return "unknown directory scan error";
}

} // namespace syncwire

