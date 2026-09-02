#include "syncwire/common/file_transfer.hpp"

#include "syncwire/common/crc32.hpp"
#include "syncwire/common/frame_parser.hpp"
#include "syncwire/common/protocol.hpp"
#include "syncwire/common/unique_fd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace syncwire::protocol {
namespace {

[[nodiscard]] FileTransferResult frame_error(const FrameIoResult& result) noexcept {
    return FileTransferResult{
        .status = FileTransferStatus::FrameIoError,
        .frame_io = result,
    };
}

[[nodiscard]] Frame make_frame(const MessageType type,
                               const std::uint64_t request_id,
                               const std::uint64_t transfer_id,
                               std::vector<std::byte> payload = {}) {
    return Frame{
        .header = FrameHeader{
            .message_type = type,
            .payload_length = static_cast<std::uint32_t>(payload.size()),
            .request_id = request_id,
            .transfer_id = transfer_id,
        },
        .payload = std::move(payload),
    };
}

[[nodiscard]] FileTransferResult receive_expected_frame(const int fd,
                                                        const MessageType type,
                                                        const std::uint64_t request_id,
                                                        const std::uint64_t transfer_id,
                                                        Frame& output) {
    const auto received = receive_frame(fd);
    if (const auto* error = std::get_if<FrameIoResult>(&received); error != nullptr) {
        return frame_error(*error);
    }

    output = std::get<Frame>(received);
    if (output.header.request_id != request_id) {
        return FileTransferResult{
            .status = FileTransferStatus::MismatchedRequestId,
            .expected_id = request_id,
            .actual_id = output.header.request_id,
        };
    }
    if (output.header.transfer_id != transfer_id) {
        return FileTransferResult{
            .status = FileTransferStatus::InvalidTransferId,
            .expected_id = transfer_id,
            .actual_id = output.header.transfer_id,
        };
    }
    if (output.header.message_type != type) {
        return FileTransferResult{.status = FileTransferStatus::UnexpectedMessageType};
    }
    return FileTransferResult{};
}

[[nodiscard]] FileTransferResult decode_remote_result(const Frame& frame) {
    const auto decoded = decode_transfer_result(frame.payload);
    if (const auto* error = std::get_if<TransferCodecError>(&decoded); error != nullptr) {
        return FileTransferResult{
            .status = FileTransferStatus::InvalidPayload,
            .codec_error = *error,
        };
    }
    const auto code = std::get<TransferResultCode>(decoded);
    if (code == TransferResultCode::Success) {
        return FileTransferResult{};
    }
    return FileTransferResult{
        .status = FileTransferStatus::RemoteRejected,
        .remote_code = code,
    };
}

[[nodiscard]] FileTransferResult read_source_metadata(const std::filesystem::path& source_path,
                                                      const TransferLimits limits,
                                                      UploadMetadata& metadata) {
    std::error_code error;
    const auto raw_size = std::filesystem::file_size(source_path, error);
    if (error) {
        return FileTransferResult{
            .status = FileTransferStatus::FileOpenError,
            .system_error = error.value(),
        };
    }
    if (raw_size > limits.max_file_size) {
        return FileTransferResult{.status = FileTransferStatus::FileTooLarge};
    }

    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        return FileTransferResult{
            .status = FileTransferStatus::FileOpenError,
            .system_error = errno,
        };
    }

    Crc32 checksum;
    std::array<std::byte, kDefaultChunkSize> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            checksum.update(std::span<const std::byte>(
                buffer.data(), static_cast<std::size_t>(count)));
        }
    }
    if (!input.eof()) {
        return FileTransferResult{
            .status = FileTransferStatus::FileReadError,
            .system_error = errno,
        };
    }

    metadata.file_size = static_cast<std::uint64_t>(raw_size);
    metadata.checksum = checksum.value();
    return FileTransferResult{};
}

[[nodiscard]] bool valid_limits(const TransferLimits limits) noexcept {
    return limits.chunk_size > 0U &&
           limits.chunk_size <= static_cast<std::size_t>(kDefaultMaxPayload) - kChunkOffsetSize;
}

[[nodiscard]] FileTransferResult write_all(const int fd,
                                           const std::span<const std::byte> data) noexcept {
    std::size_t written = 0U;
    while (written < data.size()) {
        const auto result = ::write(fd, data.data() + written, data.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return FileTransferResult{
            .status = FileTransferStatus::FileWriteError,
            .system_error = result < 0 ? errno : EIO,
        };
    }
    return FileTransferResult{};
}

[[nodiscard]] FrameIoResult send_result(const int fd,
                                        const std::uint64_t request_id,
                                        const std::uint64_t transfer_id,
                                        const TransferResultCode code) {
    return send_frame(fd,
                      make_frame(MessageType::TransferResult,
                                 request_id,
                                 transfer_id,
                                 encode_transfer_result(code)));
}

[[nodiscard]] TransferResultCode result_code_for_status(const FileTransferStatus status) noexcept {
    switch (status) {
    case FileTransferStatus::InvalidPath:
        return TransferResultCode::InvalidPath;
    case FileTransferStatus::FileTooLarge:
        return TransferResultCode::FileTooLarge;
    case FileTransferStatus::UnexpectedOffset:
        return TransferResultCode::UnexpectedOffset;
    case FileTransferStatus::SizeMismatch:
        return TransferResultCode::SizeMismatch;
    case FileTransferStatus::ChecksumMismatch:
        return TransferResultCode::ChecksumMismatch;
    case FileTransferStatus::FileOpenError:
    case FileTransferStatus::FileReadError:
    case FileTransferStatus::FileWriteError:
    case FileTransferStatus::FileCommitError:
        return TransferResultCode::FileIoError;
    case FileTransferStatus::UnexpectedMessageType:
    case FileTransferStatus::InvalidTransferId:
    case FileTransferStatus::MismatchedRequestId:
    case FileTransferStatus::UnexpectedFrame:
        return TransferResultCode::UnexpectedFrame;
    case FileTransferStatus::InvalidRequest:
    case FileTransferStatus::InvalidRequestId:
    case FileTransferStatus::InvalidPayload:
        return TransferResultCode::InvalidRequest;
    case FileTransferStatus::Success:
    case FileTransferStatus::FrameIoError:
    case FileTransferStatus::RemoteRejected:
        return TransferResultCode::InvalidRequest;
    }
    return TransferResultCode::InvalidRequest;
}

} // namespace

FileTransferResult send_file(const int server_fd,
                             const std::filesystem::path& source_path,
                             const std::string_view remote_filename,
                             const std::uint64_t request_id,
                             const TransferLimits limits) {
    if (request_id == 0U) {
        return FileTransferResult{.status = FileTransferStatus::InvalidRequestId};
    }
    if (!is_safe_remote_filename(remote_filename)) {
        return FileTransferResult{.status = FileTransferStatus::InvalidPath};
    }
    if (!valid_limits(limits)) {
        return FileTransferResult{.status = FileTransferStatus::InvalidRequest};
    }

    UploadMetadata metadata{.filename = std::string(remote_filename)};
    const auto metadata_result = read_source_metadata(source_path, limits, metadata);
    if (!metadata_result.ok()) {
        return metadata_result;
    }

    const auto request_payload = encode_upload_metadata(metadata);
    const auto sent_request = send_frame(
        server_fd,
        make_frame(MessageType::UploadRequest, request_id, 0U, request_payload));
    if (!sent_request.ok()) {
        return frame_error(sent_request);
    }

    const auto first_response = receive_frame(server_fd);
    if (const auto* error = std::get_if<FrameIoResult>(&first_response); error != nullptr) {
        return frame_error(*error);
    }
    const auto& ready = std::get<Frame>(first_response);
    if (ready.header.request_id != request_id) {
        return FileTransferResult{
            .status = FileTransferStatus::MismatchedRequestId,
            .expected_id = request_id,
            .actual_id = ready.header.request_id,
        };
    }
    if (ready.header.message_type == MessageType::TransferResult) {
        if (ready.header.transfer_id != 0U) {
            return FileTransferResult{
                .status = FileTransferStatus::InvalidTransferId,
                .actual_id = ready.header.transfer_id,
            };
        }
        return decode_remote_result(ready);
    }
    if (ready.header.message_type != MessageType::TransferReady || !ready.payload.empty()) {
        return FileTransferResult{.status = FileTransferStatus::UnexpectedMessageType};
    }
    const auto transfer_id = ready.header.transfer_id;
    if (transfer_id == 0U) {
        return FileTransferResult{.status = FileTransferStatus::InvalidTransferId};
    }

    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        return FileTransferResult{
            .status = FileTransferStatus::FileOpenError,
            .system_error = errno,
        };
    }

    std::vector<std::byte> buffer(limits.chunk_size);
    std::uint64_t offset = 0U;
    while (offset < metadata.file_size) {
        const auto remaining = metadata.file_size - offset;
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(requested));
        const auto count = input.gcount();
        if (count <= 0 || static_cast<std::size_t>(count) != requested) {
            return FileTransferResult{
                .status = FileTransferStatus::FileReadError,
                .transferred = offset,
                .system_error = errno,
            };
        }

        const auto chunk_size = static_cast<std::size_t>(count);
        const auto chunk_payload = encode_file_chunk(
            offset, std::span<const std::byte>(buffer.data(), chunk_size));
        const auto sent_chunk = send_frame(
            server_fd,
            make_frame(MessageType::FileChunk, request_id, transfer_id, chunk_payload));
        if (!sent_chunk.ok()) {
            return frame_error(sent_chunk);
        }

        offset += static_cast<std::uint64_t>(chunk_size);
        Frame acknowledgment;
        const auto ack_result = receive_expected_frame(server_fd,
                                                       MessageType::Acknowledgment,
                                                       request_id,
                                                       transfer_id,
                                                       acknowledgment);
        if (!ack_result.ok()) {
            return ack_result;
        }
        const auto decoded_offset = decode_offset(acknowledgment.payload);
        if (const auto* error = std::get_if<TransferCodecError>(&decoded_offset);
            error != nullptr) {
            return FileTransferResult{
                .status = FileTransferStatus::InvalidPayload,
                .codec_error = *error,
                .transferred = offset,
            };
        }
        const auto acknowledged = std::get<std::uint64_t>(decoded_offset);
        if (acknowledged != offset) {
            return FileTransferResult{
                .status = FileTransferStatus::UnexpectedOffset,
                .transferred = offset,
                .expected_id = offset,
                .actual_id = acknowledged,
            };
        }
    }

    const auto sent_complete = send_frame(
        server_fd,
        make_frame(MessageType::TransferComplete, request_id, transfer_id));
    if (!sent_complete.ok()) {
        return frame_error(sent_complete);
    }

    Frame final_response;
    const auto response_result = receive_expected_frame(server_fd,
                                                        MessageType::TransferResult,
                                                        request_id,
                                                        transfer_id,
                                                        final_response);
    if (!response_result.ok()) {
        return response_result;
    }
    auto result = decode_remote_result(final_response);
    result.transferred = offset;
    result.expected_checksum = metadata.checksum;
    return result;
}

FileTransferResult receive_file(const int client_fd,
                                const Frame& upload_request,
                                const std::filesystem::path& destination_root,
                                const TransferLimits limits) {
    const auto request_id = upload_request.header.request_id;
    auto reject_before_ready = [&](FileTransferResult result) {
        const auto sent = send_result(client_fd,
                                      request_id,
                                      0U,
                                      result_code_for_status(result.status));
        if (!sent.ok()) {
            return frame_error(sent);
        }
        return result;
    };

    if (upload_request.header.message_type != MessageType::UploadRequest ||
        upload_request.header.transfer_id != 0U || request_id == 0U || !valid_limits(limits)) {
        return reject_before_ready(
            FileTransferResult{.status = FileTransferStatus::InvalidRequest});
    }

    const auto decoded_metadata = decode_upload_metadata(upload_request.payload);
    if (const auto* error = std::get_if<TransferCodecError>(&decoded_metadata);
        error != nullptr) {
        const auto status = *error == TransferCodecError::UnsafeFilename ||
                                    *error == TransferCodecError::EmptyFilename ||
                                    *error == TransferCodecError::FilenameTooLong
                                ? FileTransferStatus::InvalidPath
                                : FileTransferStatus::InvalidPayload;
        return reject_before_ready(FileTransferResult{
            .status = status,
            .codec_error = *error,
        });
    }
    const auto& metadata = std::get<UploadMetadata>(decoded_metadata);
    if (metadata.file_size > limits.max_file_size) {
        return reject_before_ready(FileTransferResult{.status = FileTransferStatus::FileTooLarge});
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(destination_root, filesystem_error);
    if (filesystem_error) {
        return reject_before_ready(FileTransferResult{
            .status = FileTransferStatus::FileOpenError,
            .system_error = filesystem_error.value(),
        });
    }

    const auto transfer_id = request_id;
    const auto final_path = destination_root / metadata.filename;
    const auto part_path = destination_root /
                           ("." + metadata.filename + "." + std::to_string(transfer_id) + ".part");
    const int raw_fd = ::open(part_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (raw_fd < 0) {
        return reject_before_ready(FileTransferResult{
            .status = FileTransferStatus::FileOpenError,
            .system_error = errno,
        });
    }
    UniqueFd part_file(raw_fd);
    bool committed = false;
    auto cleanup = [&] {
        part_file.reset();
        if (!committed) {
            std::error_code ignored;
            std::filesystem::remove(part_path, ignored);
        }
    };
    auto reject_transfer = [&](FileTransferResult result) {
        cleanup();
        const auto sent = send_result(client_fd,
                                      request_id,
                                      transfer_id,
                                      result_code_for_status(result.status));
        if (!sent.ok()) {
            return frame_error(sent);
        }
        return result;
    };

    const auto sent_ready = send_frame(
        client_fd,
        make_frame(MessageType::TransferReady, request_id, transfer_id));
    if (!sent_ready.ok()) {
        cleanup();
        return frame_error(sent_ready);
    }

    Crc32 checksum;
    std::uint64_t offset = 0U;
    while (true) {
        const auto received = receive_frame(client_fd);
        if (const auto* error = std::get_if<FrameIoResult>(&received); error != nullptr) {
            cleanup();
            return frame_error(*error);
        }
        const auto& frame = std::get<Frame>(received);
        if (frame.header.request_id != request_id || frame.header.transfer_id != transfer_id) {
            return reject_transfer(FileTransferResult{
                .status = FileTransferStatus::UnexpectedFrame,
                .expected_id = request_id,
                .actual_id = frame.header.request_id,
            });
        }

        if (frame.header.message_type == MessageType::TransferComplete) {
            if (!frame.payload.empty()) {
                return reject_transfer(
                    FileTransferResult{.status = FileTransferStatus::InvalidPayload});
            }
            break;
        }
        if (frame.header.message_type != MessageType::FileChunk) {
            return reject_transfer(
                FileTransferResult{.status = FileTransferStatus::UnexpectedMessageType});
        }

        const auto decoded_chunk = decode_file_chunk(frame.payload);
        if (const auto* error = std::get_if<TransferCodecError>(&decoded_chunk);
            error != nullptr) {
            return reject_transfer(FileTransferResult{
                .status = FileTransferStatus::InvalidPayload,
                .codec_error = *error,
            });
        }
        const auto& chunk = std::get<FileChunk>(decoded_chunk);
        if (chunk.offset != offset) {
            return reject_transfer(FileTransferResult{
                .status = FileTransferStatus::UnexpectedOffset,
                .transferred = offset,
                .expected_id = offset,
                .actual_id = chunk.offset,
            });
        }
        if (chunk.data.size() > limits.chunk_size || offset > metadata.file_size ||
            static_cast<std::uint64_t>(chunk.data.size()) > metadata.file_size - offset) {
            return reject_transfer(FileTransferResult{
                .status = FileTransferStatus::SizeMismatch,
                .transferred = offset,
            });
        }

        const auto write_result = write_all(part_file.get(), chunk.data);
        if (!write_result.ok()) {
            return reject_transfer(write_result);
        }
        checksum.update(chunk.data);
        offset += static_cast<std::uint64_t>(chunk.data.size());

        const auto sent_ack = send_frame(
            client_fd,
            make_frame(MessageType::Acknowledgment,
                       request_id,
                       transfer_id,
                       encode_offset(offset)));
        if (!sent_ack.ok()) {
            cleanup();
            return frame_error(sent_ack);
        }
    }

    if (offset != metadata.file_size) {
        return reject_transfer(FileTransferResult{
            .status = FileTransferStatus::SizeMismatch,
            .transferred = offset,
        });
    }
    const auto actual_checksum = checksum.value();
    if (actual_checksum != metadata.checksum) {
        return reject_transfer(FileTransferResult{
            .status = FileTransferStatus::ChecksumMismatch,
            .transferred = offset,
            .expected_checksum = metadata.checksum,
            .actual_checksum = actual_checksum,
        });
    }
    if (::fsync(part_file.get()) < 0) {
        return reject_transfer(FileTransferResult{
            .status = FileTransferStatus::FileCommitError,
            .transferred = offset,
            .system_error = errno,
        });
    }
    part_file.reset();
    std::filesystem::rename(part_path, final_path, filesystem_error);
    if (filesystem_error) {
        return reject_transfer(FileTransferResult{
            .status = FileTransferStatus::FileCommitError,
            .transferred = offset,
            .system_error = filesystem_error.value(),
        });
    }
    committed = true;

    const auto sent_result = send_result(
        client_fd, request_id, transfer_id, TransferResultCode::Success);
    if (!sent_result.ok()) {
        return frame_error(sent_result);
    }
    FileTransferResult result;
    result.transferred = offset;
    result.expected_checksum = metadata.checksum;
    result.actual_checksum = actual_checksum;
    return result;
}

std::string_view file_transfer_status_message(const FileTransferStatus status) noexcept {
    switch (status) {
    case FileTransferStatus::Success:
        return "file transfer completed";
    case FileTransferStatus::FrameIoError:
        return "frame transport failed";
    case FileTransferStatus::InvalidRequest:
        return "file transfer request is invalid";
    case FileTransferStatus::InvalidPath:
        return "remote filename is invalid";
    case FileTransferStatus::FileTooLarge:
        return "file exceeds the configured transfer limit";
    case FileTransferStatus::FileOpenError:
        return "file could not be opened";
    case FileTransferStatus::FileReadError:
        return "source file could not be read";
    case FileTransferStatus::FileWriteError:
        return "destination file could not be written";
    case FileTransferStatus::FileCommitError:
        return "temporary file could not be committed";
    case FileTransferStatus::UnexpectedMessageType:
        return "peer sent an unexpected transfer message";
    case FileTransferStatus::UnexpectedFrame:
        return "peer sent a frame outside the active transfer";
    case FileTransferStatus::InvalidRequestId:
        return "request ID must be nonzero";
    case FileTransferStatus::InvalidTransferId:
        return "transfer ID is invalid";
    case FileTransferStatus::MismatchedRequestId:
        return "response request ID does not match the upload";
    case FileTransferStatus::InvalidPayload:
        return "transfer payload is invalid";
    case FileTransferStatus::UnexpectedOffset:
        return "file chunk acknowledgment or offset is unexpected";
    case FileTransferStatus::SizeMismatch:
        return "transferred byte count does not match metadata";
    case FileTransferStatus::ChecksumMismatch:
        return "transferred file failed checksum verification";
    case FileTransferStatus::RemoteRejected:
        return "server rejected the transfer";
    }
    return "unknown file transfer status";
}

} // namespace syncwire::protocol

