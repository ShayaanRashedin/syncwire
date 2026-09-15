#include "syncwire/common/directory_sync.hpp"

#include "syncwire/common/directory_manifest.hpp"
#include "syncwire/common/file_transfer.hpp"
#include "syncwire/common/frame_io.hpp"
#include "syncwire/common/frame_parser.hpp"
#include "syncwire/common/protocol.hpp"
#include "syncwire/common/sync_codec.hpp"
#include "syncwire/common/transfer_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace syncwire::protocol {
namespace {

[[nodiscard]] DirectorySyncResult frame_error(const FrameIoResult& error) noexcept {
    return DirectorySyncResult{
        .status = DirectorySyncStatus::FrameIoError,
        .frame_io = error,
    };
}

[[nodiscard]] DirectorySyncResult scan_error(const DirectoryScanResult& scan) noexcept {
    return DirectorySyncResult{
        .status = DirectorySyncStatus::ScanError,
        .scan_error = scan.error,
        .system_error = scan.system_error,
    };
}

[[nodiscard]] Frame make_frame(const MessageType type,
                               const std::uint64_t request_id,
                               std::vector<std::byte> payload = {}) {
    return Frame{
        .header = FrameHeader{
            .message_type = type,
            .payload_length = static_cast<std::uint32_t>(payload.size()),
            .request_id = request_id,
        },
        .payload = std::move(payload),
    };
}

[[nodiscard]] DirectorySyncResult receive_control_frame(const int fd,
                                                        const MessageType expected_type,
                                                        const std::uint64_t request_id,
                                                        Frame& output) {
    const auto received = receive_frame(fd);
    if (const auto* error = std::get_if<FrameIoResult>(&received); error != nullptr) {
        return frame_error(*error);
    }
    output = std::get<Frame>(received);
    if (output.header.request_id != request_id) {
        return DirectorySyncResult{
            .status = DirectorySyncStatus::MismatchedRequestId,
            .expected_id = request_id,
            .actual_id = output.header.request_id,
        };
    }
    if (output.header.transfer_id != 0U) {
        return DirectorySyncResult{
            .status = DirectorySyncStatus::UnexpectedTransferId,
            .actual_id = output.header.transfer_id,
        };
    }
    if (output.header.message_type != expected_type) {
        return DirectorySyncResult{.status = DirectorySyncStatus::UnexpectedMessageType};
    }
    return DirectorySyncResult{};
}

[[nodiscard]] const FileRecord*
find_file(const std::span<const FileRecord> files, const std::string_view path) {
    const auto found = std::ranges::lower_bound(files, path, {}, &FileRecord::path);
    if (found == files.end() || found->path != path) {
        return nullptr;
    }
    return &*found;
}

[[nodiscard]] FrameIoResult send_transfer_rejection(const int fd,
                                                    const std::uint64_t request_id) {
    return send_frame(
        fd,
        Frame{
            .header = FrameHeader{
                .message_type = MessageType::TransferResult,
                .payload_length = 1U,
                .request_id = request_id,
            },
            .payload = encode_transfer_result(TransferResultCode::UnexpectedFrame),
        });
}

} // namespace

DirectorySyncResult sync_directory(const int server_fd,
                                   const std::filesystem::path& source_root,
                                   const std::uint64_t request_id,
                                   const DirectorySyncLimits limits) {
    if (request_id == 0U) {
        return DirectorySyncResult{.status = DirectorySyncStatus::InvalidRequestId};
    }

    const auto source = scan_directory(source_root, limits.scan);
    if (!source.ok()) {
        return scan_error(source);
    }
    const auto manifest_payload = encode_manifest(source.files);
    if (manifest_payload.empty() || manifest_payload.size() > kDefaultMaxPayload) {
        return DirectorySyncResult{
            .status = DirectorySyncStatus::CodecError,
            .codec_error = SyncCodecError::PayloadSizeMismatch,
        };
    }
    const auto sent_manifest = send_frame(
        server_fd,
        make_frame(MessageType::SyncManifest, request_id, manifest_payload));
    if (!sent_manifest.ok()) {
        return frame_error(sent_manifest);
    }

    Frame plan_frame;
    const auto plan_frame_result = receive_control_frame(
        server_fd, MessageType::SyncPlan, request_id, plan_frame);
    if (!plan_frame_result.ok()) {
        return plan_frame_result;
    }
    const auto decoded_plan = decode_sync_plan(plan_frame.payload, limits.scan.max_entries);
    if (const auto* error = std::get_if<SyncCodecError>(&decoded_plan); error != nullptr) {
        return DirectorySyncResult{
            .status = DirectorySyncStatus::CodecError,
            .codec_error = *error,
        };
    }
    const auto& plan = std::get<SyncPlan>(decoded_plan);
    if (plan.upload_paths.size() + static_cast<std::size_t>(plan.unchanged_count) !=
        source.files.size()) {
        return DirectorySyncResult{.status = DirectorySyncStatus::PlanMismatch};
    }
    if (plan.upload_paths.size() >
        std::numeric_limits<std::uint64_t>::max() - request_id) {
        return DirectorySyncResult{.status = DirectorySyncStatus::InvalidRequestId};
    }

    DirectorySyncResult result;
    result.planned_uploads = plan.upload_paths.size();
    result.unchanged_files = plan.unchanged_count;
    result.server_only_files = plan.server_only_count;
    for (std::size_t index = 0U; index < plan.upload_paths.size(); ++index) {
        const auto& path = plan.upload_paths[index];
        if (find_file(source.files, path) == nullptr) {
            return DirectorySyncResult{.status = DirectorySyncStatus::PlanMismatch};
        }
        const auto file_request_id = request_id + static_cast<std::uint64_t>(index) + 1U;
        const auto transfer = send_file(
            server_fd, source_root / std::filesystem::path(path), path, file_request_id, limits.transfer);
        if (!transfer.ok()) {
            result.status = DirectorySyncStatus::FileTransferError;
            result.file_transfer = transfer;
            return result;
        }
        ++result.completed_uploads;
    }

    const auto sent_complete = send_frame(
        server_fd, make_frame(MessageType::SyncComplete, request_id));
    if (!sent_complete.ok()) {
        return frame_error(sent_complete);
    }

    Frame final_frame;
    const auto final_frame_result = receive_control_frame(
        server_fd, MessageType::SyncResult, request_id, final_frame);
    if (!final_frame_result.ok()) {
        return final_frame_result;
    }
    const auto decoded_result = decode_sync_result(final_frame.payload);
    if (const auto* error = std::get_if<SyncCodecError>(&decoded_result); error != nullptr) {
        return DirectorySyncResult{
            .status = DirectorySyncStatus::CodecError,
            .codec_error = *error,
        };
    }
    if (std::get<SyncResultCode>(decoded_result) != SyncResultCode::Success) {
        result.status = DirectorySyncStatus::VerificationFailed;
    }
    return result;
}

DirectorySyncResult receive_directory_sync(const int client_fd,
                                           const Frame& manifest_frame,
                                           const std::filesystem::path& destination_root,
                                           const DirectorySyncLimits limits) {
    const auto request_id = manifest_frame.header.request_id;
    if (manifest_frame.header.message_type != MessageType::SyncManifest || request_id == 0U ||
        manifest_frame.header.transfer_id != 0U) {
        return DirectorySyncResult{.status = DirectorySyncStatus::InvalidRequest};
    }

    const auto decoded_manifest = decode_manifest(
        manifest_frame.payload, limits.scan.max_entries);
    if (const auto* error = std::get_if<SyncCodecError>(&decoded_manifest); error != nullptr) {
        return DirectorySyncResult{
            .status = DirectorySyncStatus::CodecError,
            .codec_error = *error,
        };
    }
    const auto& source = std::get<FileManifest>(decoded_manifest);
    for (const auto& file : source) {
        if (file.size > limits.scan.max_file_size) {
            return DirectorySyncResult{.status = DirectorySyncStatus::InvalidRequest};
        }
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(destination_root, filesystem_error);
    if (filesystem_error) {
        return DirectorySyncResult{
            .status = DirectorySyncStatus::FileIoError,
            .system_error = filesystem_error.value(),
        };
    }
    const auto destination = scan_directory(destination_root, limits.scan);
    if (!destination.ok()) {
        return scan_error(destination);
    }
    const auto plan = build_sync_plan(source, destination.files);
    if (plan.upload_paths.size() >
        std::numeric_limits<std::uint64_t>::max() - request_id) {
        return DirectorySyncResult{.status = DirectorySyncStatus::InvalidRequestId};
    }
    const auto plan_payload = encode_sync_plan(plan);
    if (plan_payload.empty() || plan_payload.size() > kDefaultMaxPayload) {
        return DirectorySyncResult{
            .status = DirectorySyncStatus::CodecError,
            .codec_error = SyncCodecError::PayloadSizeMismatch,
        };
    }
    const auto sent_plan = send_frame(
        client_fd, make_frame(MessageType::SyncPlan, request_id, plan_payload));
    if (!sent_plan.ok()) {
        return frame_error(sent_plan);
    }

    DirectorySyncResult result;
    result.planned_uploads = plan.upload_paths.size();
    result.unchanged_files = plan.unchanged_count;
    result.server_only_files = plan.server_only_count;
    for (std::size_t index = 0U; index < plan.upload_paths.size(); ++index) {
        const auto received = receive_frame(client_fd);
        if (const auto* error = std::get_if<FrameIoResult>(&received); error != nullptr) {
            return frame_error(*error);
        }
        const auto& request = std::get<Frame>(received);
        const auto expected_request_id = request_id + static_cast<std::uint64_t>(index) + 1U;
        const auto decoded_metadata = decode_upload_metadata(request.payload);
        const auto* metadata = std::get_if<UploadMetadata>(&decoded_metadata);
        if (request.header.message_type != MessageType::UploadRequest ||
            request.header.request_id != expected_request_id ||
            request.header.transfer_id != 0U || metadata == nullptr ||
            metadata->filename != plan.upload_paths[index]) {
            const auto rejected = send_transfer_rejection(client_fd, request.header.request_id);
            if (!rejected.ok()) {
                return frame_error(rejected);
            }
            return DirectorySyncResult{.status = DirectorySyncStatus::PlanMismatch};
        }

        const auto transfer = receive_file(
            client_fd, request, destination_root, limits.transfer);
        if (!transfer.ok()) {
            result.status = DirectorySyncStatus::FileTransferError;
            result.file_transfer = transfer;
            return result;
        }
        ++result.completed_uploads;
    }

    Frame complete_frame;
    const auto complete_result = receive_control_frame(
        client_fd, MessageType::SyncComplete, request_id, complete_frame);
    if (!complete_result.ok()) {
        return complete_result;
    }
    if (!complete_frame.payload.empty()) {
        return DirectorySyncResult{.status = DirectorySyncStatus::InvalidRequest};
    }

    const auto verified_destination = scan_directory(destination_root, limits.scan);
    if (!verified_destination.ok()) {
        return scan_error(verified_destination);
    }
    const auto verification = build_sync_plan(source, verified_destination.files);
    const auto result_code = verification.upload_paths.empty()
                                 ? SyncResultCode::Success
                                 : SyncResultCode::DestinationChanged;
    const auto sent_result = send_frame(
        client_fd,
        make_frame(MessageType::SyncResult, request_id, encode_sync_result(result_code)));
    if (!sent_result.ok()) {
        return frame_error(sent_result);
    }
    if (result_code != SyncResultCode::Success) {
        result.status = DirectorySyncStatus::VerificationFailed;
    }
    return result;
}

std::string_view directory_sync_status_message(const DirectorySyncStatus status) noexcept {
    switch (status) {
    case DirectorySyncStatus::Success:
        return "directory synchronization completed";
    case DirectorySyncStatus::FrameIoError:
        return "directory synchronization transport failed";
    case DirectorySyncStatus::ScanError:
        return "directory scan failed";
    case DirectorySyncStatus::CodecError:
        return "directory synchronization payload is invalid";
    case DirectorySyncStatus::FileTransferError:
        return "a planned file upload failed";
    case DirectorySyncStatus::InvalidRequest:
        return "directory synchronization request is invalid";
    case DirectorySyncStatus::InvalidRequestId:
        return "directory synchronization request ID is invalid";
    case DirectorySyncStatus::MismatchedRequestId:
        return "directory synchronization response ID does not match";
    case DirectorySyncStatus::UnexpectedMessageType:
        return "peer sent an unexpected synchronization message";
    case DirectorySyncStatus::UnexpectedTransferId:
        return "synchronization control frame has a transfer ID";
    case DirectorySyncStatus::PlanMismatch:
        return "server synchronization plan does not match the source manifest";
    case DirectorySyncStatus::VerificationFailed:
        return "destination verification failed after synchronization";
    case DirectorySyncStatus::FileIoError:
        return "destination directory could not be prepared";
    }
    return "unknown directory synchronization status";
}

} // namespace syncwire::protocol
