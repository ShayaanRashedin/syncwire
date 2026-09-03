#pragma once

#include "syncwire/common/directory_manifest.hpp"
#include "syncwire/common/file_transfer.hpp"
#include "syncwire/common/frame_io.hpp"
#include "syncwire/common/sync_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace syncwire::protocol {

struct DirectorySyncLimits {
    DirectoryScanLimits scan;
    TransferLimits transfer;
};

enum class DirectorySyncStatus {
    Success,
    FrameIoError,
    ScanError,
    CodecError,
    FileTransferError,
    InvalidRequest,
    InvalidRequestId,
    MismatchedRequestId,
    UnexpectedMessageType,
    UnexpectedTransferId,
    PlanMismatch,
    VerificationFailed,
    FileIoError,
};

struct DirectorySyncResult {
    DirectorySyncStatus status{DirectorySyncStatus::Success};
    FrameIoResult frame_io{};
    FileTransferResult file_transfer{};
    std::optional<DirectoryScanError> scan_error{};
    std::optional<SyncCodecError> codec_error{};
    std::size_t planned_uploads{0U};
    std::size_t completed_uploads{0U};
    std::size_t unchanged_files{0U};
    std::size_t server_only_files{0U};
    std::uint64_t expected_id{0U};
    std::uint64_t actual_id{0U};
    int system_error{0};

    [[nodiscard]] bool ok() const noexcept {
        return status == DirectorySyncStatus::Success;
    }
};

[[nodiscard]] DirectorySyncResult
sync_directory(int server_fd,
               const std::filesystem::path& source_root,
               std::uint64_t request_id,
               DirectorySyncLimits limits = {});

[[nodiscard]] DirectorySyncResult
receive_directory_sync(int client_fd,
                       const Frame& manifest_frame,
                       const std::filesystem::path& destination_root,
                       DirectorySyncLimits limits = {});

[[nodiscard]] std::string_view directory_sync_status_message(DirectorySyncStatus status) noexcept;

} // namespace syncwire::protocol

