#include "syncwire/server/client_session.hpp"

#include "syncwire/common/frame_parser.hpp"
#include "syncwire/common/protocol.hpp"

#include <mutex>
#include <utility>
#include <variant>

namespace syncwire::server {
namespace {

template <typename Operation>
[[nodiscard]] auto with_destination_lock(std::mutex* destination_mutex,
                                         Operation&& operation) {
    if (destination_mutex == nullptr) {
        return std::forward<Operation>(operation)();
    }
    const std::scoped_lock lock(*destination_mutex);
    return std::forward<Operation>(operation)();
}

} // namespace

ClientSessionResult serve_client_session(
    const int client_fd,
    const std::filesystem::path& destination_root,
    std::mutex* const destination_mutex) {
    const auto received = protocol::receive_frame(client_fd);
    if (const auto* error = std::get_if<protocol::FrameIoResult>(&received);
        error != nullptr) {
        return ClientSessionResult{
            .status = SessionStatus::FrameIoError,
            .frame_io = *error,
        };
    }

    const auto& first_frame = std::get<protocol::Frame>(received);
    ClientSessionResult result{
        .request_id = first_frame.header.request_id,
    };

    switch (first_frame.header.message_type) {
    case protocol::MessageType::Ping:
        result.operation = SessionOperation::Ping;
        result.ping = protocol::serve_ping_frame(client_fd, first_frame);
        result.status =
            result.ping.ok() ? SessionStatus::Success : SessionStatus::PingError;
        return result;

    case protocol::MessageType::UploadRequest:
        result.operation = SessionOperation::Upload;
        result.transfer = with_destination_lock(destination_mutex, [&] {
            return protocol::receive_file(client_fd, first_frame, destination_root);
        });
        result.status = result.transfer.ok() ? SessionStatus::Success
                                             : SessionStatus::FileTransferError;
        return result;

    case protocol::MessageType::SyncManifest:
        result.operation = SessionOperation::DirectorySync;
        result.sync = with_destination_lock(destination_mutex, [&] {
            return protocol::receive_directory_sync(
                client_fd, first_frame, destination_root);
        });
        result.status = result.sync.ok() ? SessionStatus::Success
                                         : SessionStatus::DirectorySyncError;
        return result;

    case protocol::MessageType::Pong:
    case protocol::MessageType::AuthChallenge:
    case protocol::MessageType::AuthProof:
    case protocol::MessageType::AuthResult:
    case protocol::MessageType::DownloadRequest:
    case protocol::MessageType::TransferReady:
    case protocol::MessageType::FileChunk:
    case protocol::MessageType::Acknowledgment:
    case protocol::MessageType::TransferComplete:
    case protocol::MessageType::TransferResult:
    case protocol::MessageType::SyncPlan:
    case protocol::MessageType::SyncComplete:
    case protocol::MessageType::SyncResult:
    case protocol::MessageType::Error:
        result.status = SessionStatus::UnsupportedMessage;
        return result;
    }

    result.status = SessionStatus::UnsupportedMessage;
    return result;
}

std::string_view session_operation_message(const SessionOperation operation) noexcept {
    switch (operation) {
    case SessionOperation::Unknown:
        return "unknown";
    case SessionOperation::Ping:
        return "ping";
    case SessionOperation::Upload:
        return "upload";
    case SessionOperation::DirectorySync:
        return "directory sync";
    }
    return "unknown";
}

std::string_view session_status_message(const SessionStatus status) noexcept {
    switch (status) {
    case SessionStatus::Success:
        return "session completed";
    case SessionStatus::FrameIoError:
        return "session frame I/O failed";
    case SessionStatus::PingError:
        return "PING/PONG exchange failed";
    case SessionStatus::FileTransferError:
        return "file transfer failed";
    case SessionStatus::DirectorySyncError:
        return "directory synchronization failed";
    case SessionStatus::UnsupportedMessage:
        return "unsupported first message";
    }
    return "unknown session status";
}

} // namespace syncwire::server

