#pragma once

#include "syncwire/common/frame_io.hpp"

#include <cstddef>
#include <string_view>

namespace syncwire::protocol {

inline constexpr std::size_t kAuthenticationNonceSize = 32U;
inline constexpr std::size_t kAuthenticationProofSize = 32U;
inline constexpr std::size_t kMinimumAuthenticationSecretSize = 16U;
inline constexpr std::size_t kMaximumAuthenticationSecretSize = 1'024U;

enum class AuthenticationStatus {
    Success,
    InvalidSecret,
    RandomGenerationError,
    CryptoError,
    FrameIoError,
    InvalidChallenge,
    InvalidProof,
    InvalidResult,
    InvalidServerProof,
    Rejected,
};

struct AuthenticationResult {
    AuthenticationStatus status{AuthenticationStatus::Success};
    FrameIoResult frame_io{};

    [[nodiscard]] bool ok() const noexcept {
        return status == AuthenticationStatus::Success;
    }
};

[[nodiscard]] bool
is_valid_authentication_secret(std::string_view secret) noexcept;

[[nodiscard]] AuthenticationResult
authenticate_client(int server_fd, std::string_view secret);

[[nodiscard]] AuthenticationResult
authenticate_server(int client_fd, std::string_view secret);

[[nodiscard]] std::string_view
authentication_status_message(AuthenticationStatus status) noexcept;

} // namespace syncwire::protocol

