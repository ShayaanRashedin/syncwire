#include "syncwire/common/authentication.hpp"

#include "syncwire/common/frame_io.hpp"
#include "syncwire/common/frame_parser.hpp"
#include "syncwire/common/protocol.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>

namespace syncwire::protocol {
namespace {

using Nonce = std::array<std::byte, kAuthenticationNonceSize>;
using Proof = std::array<std::byte, kAuthenticationProofSize>;

inline constexpr std::string_view kClientProofDomain =
    "SyncWire-v2-client-proof";
inline constexpr std::string_view kServerProofDomain =
    "SyncWire-v2-server-proof";
inline constexpr std::byte kAuthenticationAccepted{0x00};
inline constexpr std::byte kAuthenticationRejected{0x01};

struct EvpMacDeleter {
    void operator()(EVP_MAC* value) const noexcept {
        EVP_MAC_free(value);
    }
};

struct EvpMacContextDeleter {
    void operator()(EVP_MAC_CTX* value) const noexcept {
        EVP_MAC_CTX_free(value);
    }
};

using EvpMac = std::unique_ptr<EVP_MAC, EvpMacDeleter>;
using EvpMacContext = std::unique_ptr<EVP_MAC_CTX, EvpMacContextDeleter>;

[[nodiscard]] AuthenticationResult frame_error(const FrameIoResult& error) {
    return AuthenticationResult{
        .status = AuthenticationStatus::FrameIoError,
        .frame_io = error,
    };
}

[[nodiscard]] bool is_auth_frame(const Frame& frame,
                                 const MessageType expected_type,
                                 const std::size_t expected_payload_size) {
    return frame.header.message_type == expected_type &&
           frame.header.request_id == 0U && frame.header.transfer_id == 0U &&
           frame.payload.size() == expected_payload_size;
}

template <std::size_t Size>
[[nodiscard]] std::array<std::byte, Size>
copy_payload(const std::span<const std::byte> payload) {
    std::array<std::byte, Size> output{};
    std::copy(payload.begin(), payload.end(), output.begin());
    return output;
}

[[nodiscard]] bool calculate_proof(const std::string_view secret,
                                   const std::string_view domain,
                                   const Nonce& server_nonce,
                                   const Nonce& client_nonce,
                                   const std::span<const std::byte> binding,
                                   Proof& proof) {
    EvpMac mac(EVP_MAC_fetch(nullptr, "HMAC", nullptr));
    if (!mac) {
        return false;
    }
    EvpMacContext context(EVP_MAC_CTX_new(mac.get()));
    if (!context) {
        return false;
    }

    char digest_name[] = "SHA256";
    std::array parameters{
        OSSL_PARAM_construct_utf8_string(
            OSSL_MAC_PARAM_DIGEST, digest_name, 0U),
        OSSL_PARAM_construct_end(),
    };
    const auto* key =
        reinterpret_cast<const unsigned char*>(secret.data());
    if (EVP_MAC_init(
            context.get(), key, secret.size(), parameters.data()) != 1) {
        return false;
    }

    const auto* domain_bytes =
        reinterpret_cast<const unsigned char*>(domain.data());
    if (EVP_MAC_update(
            context.get(), domain_bytes, domain.size()) != 1) {
        return false;
    }
    if (EVP_MAC_update(
            context.get(),
            reinterpret_cast<const unsigned char*>(server_nonce.data()),
            server_nonce.size()) != 1) {
        return false;
    }
    if (EVP_MAC_update(
            context.get(),
            reinterpret_cast<const unsigned char*>(client_nonce.data()),
            client_nonce.size()) != 1) {
        return false;
    }
    if (!binding.empty() &&
        EVP_MAC_update(
            context.get(),
            reinterpret_cast<const unsigned char*>(binding.data()),
            binding.size()) != 1) {
        return false;
    }

    std::size_t produced = 0U;
    if (EVP_MAC_final(
            context.get(),
            reinterpret_cast<unsigned char*>(proof.data()),
            &produced,
            proof.size()) != 1) {
        return false;
    }
    return produced == proof.size();
}

[[nodiscard]] Frame make_auth_frame(const MessageType type,
                                    const std::span<const std::byte> payload) {
    return Frame{
        .header = FrameHeader{
            .message_type = type,
            .payload_length = static_cast<std::uint32_t>(payload.size()),
        },
        .payload = std::vector<std::byte>(payload.begin(), payload.end()),
    };
}

[[nodiscard]] FrameIoResult
send_auth_result(const int fd,
                 const bool accepted,
                 const std::span<const std::byte> server_proof = {}) {
    std::vector<std::byte> payload{
        accepted ? kAuthenticationAccepted : kAuthenticationRejected,
    };
    payload.insert(payload.end(), server_proof.begin(), server_proof.end());
    return send_frame(fd, make_auth_frame(MessageType::AuthResult, payload));
}

} // namespace

bool is_valid_authentication_secret(const std::string_view secret) noexcept {
    return secret.size() >= kMinimumAuthenticationSecretSize &&
           secret.size() <= kMaximumAuthenticationSecretSize;
}

AuthenticationResult authenticate_client(const int server_fd,
                                         const std::string_view secret) {
    if (!is_valid_authentication_secret(secret)) {
        return AuthenticationResult{
            .status = AuthenticationStatus::InvalidSecret,
        };
    }

    const auto challenge_received = receive_frame(server_fd);
    if (const auto* error = std::get_if<FrameIoResult>(&challenge_received);
        error != nullptr) {
        return frame_error(*error);
    }
    const auto& challenge = std::get<Frame>(challenge_received);
    if (!is_auth_frame(
            challenge, MessageType::AuthChallenge, kAuthenticationNonceSize)) {
        return AuthenticationResult{
            .status = AuthenticationStatus::InvalidChallenge,
        };
    }

    const auto server_nonce =
        copy_payload<kAuthenticationNonceSize>(challenge.payload);
    Nonce client_nonce{};
    if (RAND_bytes(
            reinterpret_cast<unsigned char*>(client_nonce.data()),
            static_cast<int>(client_nonce.size())) != 1) {
        return AuthenticationResult{
            .status = AuthenticationStatus::RandomGenerationError,
        };
    }
    Proof client_proof{};
    if (!calculate_proof(
            secret,
            kClientProofDomain,
            server_nonce,
            client_nonce,
            {},
            client_proof)) {
        return AuthenticationResult{
            .status = AuthenticationStatus::CryptoError,
        };
    }
    std::vector<std::byte> proof_payload;
    proof_payload.reserve(client_nonce.size() + client_proof.size());
    proof_payload.insert(
        proof_payload.end(), client_nonce.begin(), client_nonce.end());
    proof_payload.insert(
        proof_payload.end(), client_proof.begin(), client_proof.end());
    const auto proof_sent = send_frame(
        server_fd, make_auth_frame(MessageType::AuthProof, proof_payload));
    if (!proof_sent.ok()) {
        return frame_error(proof_sent);
    }

    const auto result_received = receive_frame(server_fd);
    if (const auto* error = std::get_if<FrameIoResult>(&result_received);
        error != nullptr) {
        return frame_error(*error);
    }
    const auto& result = std::get<Frame>(result_received);
    if (result.header.message_type != MessageType::AuthResult ||
        result.header.request_id != 0U || result.header.transfer_id != 0U ||
        result.payload.empty()) {
        return AuthenticationResult{
            .status = AuthenticationStatus::InvalidResult,
        };
    }
    if (result.payload.front() == kAuthenticationRejected) {
        if (result.payload.size() != 1U) {
            return AuthenticationResult{
                .status = AuthenticationStatus::InvalidResult,
            };
        }
        return AuthenticationResult{
            .status = AuthenticationStatus::Rejected,
        };
    }
    if (result.payload.front() != kAuthenticationAccepted ||
        result.payload.size() != 1U + kAuthenticationProofSize) {
        return AuthenticationResult{
            .status = AuthenticationStatus::InvalidResult,
        };
    }

    Proof expected_server_proof{};
    if (!calculate_proof(
            secret,
            kServerProofDomain,
            server_nonce,
            client_nonce,
            client_proof,
            expected_server_proof)) {
        return AuthenticationResult{
            .status = AuthenticationStatus::CryptoError,
        };
    }
    const bool server_accepted =
        CRYPTO_memcmp(expected_server_proof.data(),
                      result.payload.data() + 1U,
                      expected_server_proof.size()) == 0;
    return server_accepted
               ? AuthenticationResult{}
               : AuthenticationResult{
                     .status = AuthenticationStatus::InvalidServerProof,
                 };
}

AuthenticationResult authenticate_server(const int client_fd,
                                         const std::string_view secret) {
    if (!is_valid_authentication_secret(secret)) {
        return AuthenticationResult{
            .status = AuthenticationStatus::InvalidSecret,
        };
    }

    Nonce server_nonce{};
    if (RAND_bytes(
            reinterpret_cast<unsigned char*>(server_nonce.data()),
            static_cast<int>(server_nonce.size())) != 1) {
        return AuthenticationResult{
            .status = AuthenticationStatus::RandomGenerationError,
        };
    }
    const auto challenge_sent = send_frame(
        client_fd,
        make_auth_frame(MessageType::AuthChallenge, server_nonce));
    if (!challenge_sent.ok()) {
        return frame_error(challenge_sent);
    }

    const auto proof_received = receive_frame(client_fd);
    if (const auto* error = std::get_if<FrameIoResult>(&proof_received);
        error != nullptr) {
        return frame_error(*error);
    }
    const auto& proof_frame = std::get<Frame>(proof_received);
    if (!is_auth_frame(
            proof_frame,
            MessageType::AuthProof,
            kAuthenticationNonceSize + kAuthenticationProofSize)) {
        const auto rejected = send_auth_result(client_fd, false);
        if (!rejected.ok()) {
            return frame_error(rejected);
        }
        return AuthenticationResult{
            .status = AuthenticationStatus::InvalidProof,
        };
    }

    const auto client_nonce = copy_payload<kAuthenticationNonceSize>(
        std::span<const std::byte>(proof_frame.payload).first(
            kAuthenticationNonceSize));
    const auto received_client_proof =
        std::span<const std::byte>(proof_frame.payload).subspan(
            kAuthenticationNonceSize);
    Proof expected{};
    if (!calculate_proof(
            secret,
            kClientProofDomain,
            server_nonce,
            client_nonce,
            {},
            expected)) {
        return AuthenticationResult{
            .status = AuthenticationStatus::CryptoError,
        };
    }
    const bool accepted =
        CRYPTO_memcmp(expected.data(),
                      received_client_proof.data(),
                      expected.size()) == 0;
    Proof server_proof{};
    if (accepted &&
        !calculate_proof(secret,
                         kServerProofDomain,
                         server_nonce,
                         client_nonce,
                         received_client_proof,
                         server_proof)) {
        return AuthenticationResult{
            .status = AuthenticationStatus::CryptoError,
        };
    }
    const std::span<const std::byte> server_proof_payload =
        accepted ? std::span<const std::byte>(server_proof)
                 : std::span<const std::byte>{};
    const auto result_sent =
        send_auth_result(client_fd, accepted, server_proof_payload);
    if (!result_sent.ok()) {
        return frame_error(result_sent);
    }
    return accepted
               ? AuthenticationResult{}
               : AuthenticationResult{
                     .status = AuthenticationStatus::Rejected,
                 };
}

std::string_view
authentication_status_message(const AuthenticationStatus status) noexcept {
    switch (status) {
    case AuthenticationStatus::Success:
        return "authentication succeeded";
    case AuthenticationStatus::InvalidSecret:
        return "authentication secret must contain 16-1024 bytes";
    case AuthenticationStatus::RandomGenerationError:
        return "secure nonce generation failed";
    case AuthenticationStatus::CryptoError:
        return "HMAC-SHA256 calculation failed";
    case AuthenticationStatus::FrameIoError:
        return "authentication frame I/O failed";
    case AuthenticationStatus::InvalidChallenge:
        return "authentication challenge is malformed";
    case AuthenticationStatus::InvalidProof:
        return "authentication proof is malformed";
    case AuthenticationStatus::InvalidResult:
        return "authentication result is malformed";
    case AuthenticationStatus::InvalidServerProof:
        return "server authentication proof is invalid";
    case AuthenticationStatus::Rejected:
        return "authentication was rejected";
    }
    return "unknown authentication status";
}

} // namespace syncwire::protocol
