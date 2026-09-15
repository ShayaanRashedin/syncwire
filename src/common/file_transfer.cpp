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
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/stat.h>
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

struct PreparedDestination {
    std::filesystem::path final_path;
    std::filesystem::path part_path;
    UniqueFd directory;
    int system_error{0};
    bool invalid_path{false};

    [[nodiscard]] bool ok() const noexcept {
        return !final_path.empty() && !part_path.empty();
    }
};

[[nodiscard]] PreparedDestination
prepare_destination(const std::filesystem::path& root,
                    const UploadMetadata& metadata) {
    PreparedDestination prepared;
    std::error_code error;

    const auto root_status = std::filesystem::symlink_status(root, error);
    const bool root_missing =
        error == std::make_error_code(std::errc::no_such_file_or_directory);
    if (error && !root_missing) {
        prepared.system_error = error.value();
        return prepared;
    }
    error.clear();
    if (!root_missing && std::filesystem::exists(root_status)) {
        if (std::filesystem::is_symlink(root_status) ||
            !std::filesystem::is_directory(root_status)) {
            prepared.invalid_path = true;
            return prepared;
        }
    } else if (!std::filesystem::create_directories(root, error) && error) {
        prepared.system_error = error.value();
        return prepared;
    }

    const std::filesystem::path relative(metadata.filename);
    auto parent = root;
    for (const auto& component : relative.parent_path()) {
        parent /= component;
        const auto status = std::filesystem::symlink_status(parent, error);
        const bool parent_missing =
            error == std::make_error_code(std::errc::no_such_file_or_directory);
        if (error && !parent_missing) {
            prepared.system_error = error.value();
            return prepared;
        }
        error.clear();
        if (!parent_missing && std::filesystem::exists(status)) {
            if (std::filesystem::is_symlink(status) ||
                !std::filesystem::is_directory(status)) {
                prepared.invalid_path = true;
                return prepared;
            }
        } else if (!std::filesystem::create_directory(parent, error) && error) {
            prepared.system_error = error.value();
            return prepared;
        }
    }

    const auto filename = relative.filename().string();
    prepared.final_path = parent / filename;
    // The key is independent of request IDs and binds the path, size, and whole-file CRC.
    // SHA-256 makes a bounded filesystem name; CRC remains accidental-corruption detection.
    const auto encoded = encode_upload_metadata(metadata);
    std::array<unsigned char, 32U> digest{};
    unsigned int digest_size = 0U;
    if (::EVP_Digest(encoded.data(), encoded.size(), digest.data(), &digest_size,
                     ::EVP_sha256(), nullptr) != 1 || digest_size != digest.size()) {
        prepared.system_error = EIO;
        return prepared;
    }
    std::string key;
    constexpr std::string_view hex = "0123456789abcdef";
    for (const auto byte : digest) {
        key.push_back(hex[byte >> 4U]);
        key.push_back(hex[byte & 0x0FU]);
    }
    const auto state_root = root / kPartialDirectory;
    if (::mkdir(state_root.c_str(), 0700) < 0 && errno != EEXIST) {
        prepared.system_error = errno;
        return prepared;
    }
    prepared.directory.reset(::open(state_root.c_str(), O_RDONLY | O_DIRECTORY |
                                                        O_NOFOLLOW | O_CLOEXEC));
    if (!prepared.directory) {
        prepared.system_error = errno;
        return prepared;
    }
    prepared.part_path = state_root / (key + ".part");
    return prepared;
}

[[nodiscard]] bool within_partial_budget(const PreparedDestination& prepared,
                                         const UploadMetadata& metadata,
                                         const TransferLimits limits) {
    // Reserve this upload's full declared size; other retained files count actual bytes.
    // The production dispatcher serializes destination mutations across this check/open.
    if (metadata.file_size > limits.max_partial_bytes) {
        return false;
    }
    std::uint64_t bytes = metadata.file_size;
    std::size_t count = 1U;
    std::error_code error;
    std::filesystem::directory_iterator it(prepared.part_path.parent_path(), error);
    const std::filesystem::directory_iterator end;
    while (!error && it != end) {
        if (it->path() != prepared.part_path) {
            const auto status = it->symlink_status(error);
            if (error || !std::filesystem::is_regular_file(status)) {
                return false;
            }
            const auto size = it->file_size(error);
            if (error || size > limits.max_partial_bytes - bytes) {
                return false;
            }
            bytes += size;
            if (++count > limits.max_partial_files) {
                return false;
            }
        }
        it.increment(error);
    }
    return !error && count <= limits.max_partial_files;
}

[[nodiscard]] bool read_partial_prefix(const int fd, const std::uint64_t size,
                                       Crc32& checksum) {
    std::array<std::byte, kDefaultChunkSize> buffer{};
    std::uint64_t remaining = size;
    while (remaining != 0U) {
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        const auto count = ::read(fd, buffer.data(), requested);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        checksum.update(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(count)));
        remaining -= static_cast<std::uint64_t>(count);
    }
    return true;
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
    if (!is_safe_remote_path(remote_filename)) {
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
        auto rejection = decode_remote_result(ready);
        if (rejection.ok()) {
            rejection.status = FileTransferStatus::UnexpectedFrame;
        }
        return rejection;
    }
    if (ready.header.message_type != MessageType::TransferReady) {
        return FileTransferResult{.status = FileTransferStatus::UnexpectedMessageType};
    }
    const auto decoded_ready = decode_transfer_ready(ready.payload);
    const auto* checkpoint = std::get_if<TransferReady>(&decoded_ready);
    if (checkpoint == nullptr) {
        return FileTransferResult{.status = FileTransferStatus::InvalidPayload};
    }
    if (checkpoint->offset > metadata.file_size ||
        (checkpoint->offset == 0U && checkpoint->prefix_checksum != 0U)) {
        return FileTransferResult{.status = FileTransferStatus::UnexpectedOffset};
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
    Crc32 prefix_checksum;
    std::uint64_t prefix_read = 0U;
    while (prefix_read < checkpoint->offset) {
        const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
            checkpoint->offset - prefix_read, buffer.size()));
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(requested));
        if (input.gcount() != static_cast<std::streamsize>(requested)) {
            return FileTransferResult{.status = FileTransferStatus::FileReadError};
        }
        prefix_checksum.update(std::span<const std::byte>(buffer.data(), requested));
        prefix_read += requested;
    }
    const auto resumed_from = prefix_checksum.value() == checkpoint->prefix_checksum
                                  ? checkpoint->offset : 0U;
    if (resumed_from == 0U) {
        input.clear();
        input.seekg(0);
        if (!input) {
            return FileTransferResult{.status = FileTransferStatus::FileReadError};
        }
    }
    std::uint64_t offset = resumed_from;
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
    result.resumed_from = resumed_from;
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

    const auto transfer_id = request_id;
    const auto prepared = prepare_destination(destination_root, metadata);
    if (!prepared.ok()) {
        return reject_before_ready(FileTransferResult{
            .status = prepared.invalid_path ? FileTransferStatus::InvalidPath
                                            : FileTransferStatus::FileOpenError,
            .system_error = prepared.system_error,
        });
    }
    const auto& final_path = prepared.final_path;
    const auto& part_path = prepared.part_path;
    if (!within_partial_budget(prepared, metadata, limits)) {
        return reject_before_ready(FileTransferResult{
            .status = FileTransferStatus::FileOpenError, .system_error = ENOSPC});
    }
    std::error_code filesystem_error;
    const auto part_name = part_path.filename().string();
    const int raw_fd = ::openat(prepared.directory.get(), part_name.c_str(),
                               O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (raw_fd < 0) {
        return reject_before_ready(FileTransferResult{
            .status = FileTransferStatus::FileOpenError,
            .system_error = errno,
        });
    }
    UniqueFd part_file(raw_fd);
    struct stat part_status {};
    if (::fstat(part_file.get(), &part_status) < 0 || !S_ISREG(part_status.st_mode) ||
        part_status.st_nlink != 1 || part_status.st_uid != ::geteuid() ||
        ::flock(part_file.get(), LOCK_EX | LOCK_NB) < 0) {
        return reject_before_ready(FileTransferResult{
            .status = FileTransferStatus::FileOpenError, .system_error = EACCES});
    }
    if (part_status.st_size < 0 ||
        static_cast<std::uint64_t>(part_status.st_size) > metadata.file_size) {
        if (::ftruncate(part_file.get(), 0) < 0) {
            return reject_before_ready(FileTransferResult{
                .status = FileTransferStatus::FileWriteError, .system_error = errno});
        }
        part_status.st_size = 0;
    }
    Crc32 checksum;
    std::uint64_t offset = static_cast<std::uint64_t>(part_status.st_size);
    if (!read_partial_prefix(part_file.get(), offset, checksum)) {
        return reject_before_ready(FileTransferResult{.status = FileTransferStatus::FileReadError});
    }
    std::uint64_t resumed_from = offset;
    bool committed = false;
    auto cleanup = [&] {
        part_file.reset();
        if (!committed) {
            static_cast<void>(::unlinkat(prepared.directory.get(), part_name.c_str(), 0));
        }
    };
    auto interrupted = [&](const FrameIoResult& io) {
        // Transport interruption keeps complete bytes for a fresh authenticated connection.
        // Malformed wire input is terminal and discards the partial instead.
        if (io.status == FrameIoStatus::PeerClosed || io.status == FrameIoStatus::SystemError) {
            if (::fsync(part_file.get()) < 0) {
                const int saved_error = errno;
                cleanup();
                return FileTransferResult{
                    .status = FileTransferStatus::FileWriteError, .system_error = saved_error};
            }
        } else {
            cleanup();
        }
        auto result = frame_error(io);
        result.transferred = offset;
        result.resumed_from = resumed_from;
        return result;
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
        make_frame(MessageType::TransferReady, request_id, transfer_id,
                   encode_transfer_ready(TransferReady{offset, checksum.value()})));
    if (!sent_ready.ok()) {
        return interrupted(sent_ready);
    }

    bool first_chunk = true;
    while (true) {
        const auto received = receive_frame(client_fd);
        if (const auto* error = std::get_if<FrameIoResult>(&received); error != nullptr) {
            return interrupted(*error);
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
        // A client whose prefix verification failed explicitly restarts with offset zero.
        if (first_chunk && offset != 0U && chunk.offset == 0U) {
            if (::ftruncate(part_file.get(), 0) < 0 || ::lseek(part_file.get(), 0, SEEK_SET) < 0) {
                return reject_transfer(FileTransferResult{
                    .status = FileTransferStatus::FileWriteError, .system_error = errno});
            }
            checksum = Crc32{};
            offset = 0U;
            resumed_from = 0U;
        }
        first_chunk = false;
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
        if (::fsync(part_file.get()) < 0 || ::fsync(prepared.directory.get()) < 0) {
            return reject_transfer(FileTransferResult{
                .status = FileTransferStatus::FileWriteError, .system_error = errno});
        }

        const auto sent_ack = send_frame(
            client_fd,
            make_frame(MessageType::Acknowledgment,
                       request_id,
                       transfer_id,
                       encode_offset(offset)));
        if (!sent_ack.ok()) {
            return interrupted(sent_ack);
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
    std::filesystem::rename(part_path, final_path, filesystem_error);
    if (filesystem_error) {
        return reject_transfer(FileTransferResult{
            .status = FileTransferStatus::FileCommitError,
            .transferred = offset,
            .system_error = filesystem_error.value(),
        });
    }
    committed = true;
    // Keep the lock through rename. Synchronize both directory entries before reporting success.
    UniqueFd final_directory(::open(final_path.parent_path().c_str(), O_RDONLY | O_DIRECTORY |
                                                                    O_CLOEXEC | O_NOFOLLOW));
    if (!final_directory || ::fsync(final_directory.get()) < 0 ||
        ::fsync(prepared.directory.get()) < 0) {
        return reject_transfer(FileTransferResult{
            .status = FileTransferStatus::FileCommitError, .system_error = errno});
    }

    const auto sent_result = send_result(
        client_fd, request_id, transfer_id, TransferResultCode::Success);
    if (!sent_result.ok()) {
        return frame_error(sent_result);
    }
    FileTransferResult result;
    result.transferred = offset;
    result.expected_checksum = metadata.checksum;
    result.actual_checksum = actual_checksum;
    result.resumed_from = resumed_from;
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
