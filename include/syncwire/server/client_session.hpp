#pragma once

#include "syncwire/common/authentication.hpp"
#include "syncwire/common/directory_sync.hpp"
#include "syncwire/common/file_transfer.hpp"
#include "syncwire/common/frame_io.hpp"
#include "syncwire/common/ping_pong.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string_view>

namespace syncwire::server {

enum class SessionOperation {
    Unknown,
    Ping,
    Upload,
    DirectorySync,
};

enum class SessionStatus {
    Success,
    AuthenticationError,
    FrameIoError,
    PingError,
    FileTransferError,
    DirectorySyncError,
    UnsupportedMessage,
};

struct ClientSessionResult {
    SessionStatus status{SessionStatus::Success};
    SessionOperation operation{SessionOperation::Unknown};
    std::uint64_t request_id{0U};
    protocol::AuthenticationResult authentication{};
    protocol::FrameIoResult frame_io{};
    protocol::PingPongResult ping{};
    protocol::FileTransferResult transfer{};
    protocol::DirectorySyncResult sync{};

    [[nodiscard]] bool ok() const noexcept {
        return status == SessionStatus::Success;
    }
};

[[nodiscard]] ClientSessionResult
serve_client_session(int client_fd,
                     const std::filesystem::path& destination_root,
                     std::string_view authentication_secret,
                     std::mutex* destination_mutex = nullptr);

[[nodiscard]] std::string_view session_operation_message(SessionOperation operation) noexcept;
[[nodiscard]] std::string_view session_status_message(SessionStatus status) noexcept;

} // namespace syncwire::server

