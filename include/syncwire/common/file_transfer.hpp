#pragma once

#include "syncwire/common/frame_io.hpp"
#include "syncwire/common/transfer_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace syncwire::protocol {

struct TransferLimits {
    std::uint64_t max_file_size{kDefaultMaxFileSize};
    std::size_t chunk_size{kDefaultChunkSize};
};

enum class FileTransferStatus {
    Success,
    FrameIoError,
    InvalidRequest,
    InvalidPath,
    FileTooLarge,
    FileOpenError,
    FileReadError,
    FileWriteError,
    FileCommitError,
    UnexpectedMessageType,
    UnexpectedFrame,
    InvalidRequestId,
    InvalidTransferId,
    MismatchedRequestId,
    InvalidPayload,
    UnexpectedOffset,
    SizeMismatch,
    ChecksumMismatch,
    RemoteRejected,
};

struct FileTransferResult {
    FileTransferStatus status{FileTransferStatus::Success};
    FrameIoResult frame_io;
    std::optional<TransferCodecError> codec_error;
    std::optional<TransferResultCode> remote_code;
    std::uint64_t transferred{0U};
    std::uint32_t expected_checksum{0U};
    std::uint32_t actual_checksum{0U};
    std::uint64_t expected_id{0U};
    std::uint64_t actual_id{0U};
    int system_error{0};

    [[nodiscard]] bool ok() const noexcept {
        return status == FileTransferStatus::Success;
    }
};

[[nodiscard]] FileTransferResult
send_file(int server_fd,
          const std::filesystem::path& source_path,
          std::string_view remote_filename,
          std::uint64_t request_id,
          TransferLimits limits = {});

[[nodiscard]] FileTransferResult
receive_file(int client_fd,
             const Frame& upload_request,
             const std::filesystem::path& destination_root,
             TransferLimits limits = {});

[[nodiscard]] std::string_view file_transfer_status_message(FileTransferStatus status) noexcept;

} // namespace syncwire::protocol
