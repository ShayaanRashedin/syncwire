#include "test_harness.hpp"

#include "syncwire/common/crc32.hpp"
#include "syncwire/common/file_transfer.hpp"
#include "syncwire/common/frame_io.hpp"
#include "syncwire/common/frame_parser.hpp"
#include "syncwire/common/protocol.hpp"
#include "syncwire/common/transfer_codec.hpp"
#include "syncwire/common/unique_fd.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::array<char, 32U> pattern{};
        constexpr std::string_view value = "/tmp/syncwire-test-XXXXXX";
        std::copy(value.begin(), value.end(), pattern.begin());
        if (char* created = ::mkdtemp(pattern.data()); created != nullptr) {
            path_ = created;
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return !path_.empty();
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] bool open_socket_pair(syncwire::UniqueFd& first, syncwire::UniqueFd& second) {
    int sockets[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) {
        return false;
    }
    first.reset(sockets[0]);
    second.reset(sockets[1]);
    return true;
}

[[nodiscard]] bool write_file(const std::filesystem::path& path,
                              const std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {};
    }
    const auto raw_size = input.tellg();
    if (raw_size < 0) {
        return {};
    }
    const auto size = static_cast<std::size_t>(raw_size);
    input.seekg(0);
    std::vector<std::byte> bytes(size);
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return {};
    }
    return bytes;
}

[[nodiscard]] syncwire::protocol::Frame make_frame(
    const syncwire::protocol::MessageType type,
    const std::uint64_t request_id,
    const std::uint64_t transfer_id,
    std::vector<std::byte> payload = {}) {
    return syncwire::protocol::Frame{
        .header = syncwire::protocol::FrameHeader{
            .message_type = type,
            .payload_length = static_cast<std::uint32_t>(payload.size()),
            .request_id = request_id,
            .transfer_id = transfer_id,
        },
        .payload = std::move(payload),
    };
}

[[nodiscard]] bool no_part_files(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.path().extension() == ".part") {
            return false;
        }
    }
    return true;
}

void test_successful_multi_chunk_upload(TestRunner& runner) {
    TemporaryDirectory temporary;
    runner.expect(temporary.valid(), "temporary transfer directory opens");
    if (!temporary.valid()) {
        return;
    }

    std::vector<std::byte> source_bytes(4'097U);
    for (std::size_t index = 0U; index < source_bytes.size(); ++index) {
        source_bytes[index] = static_cast<std::byte>(index % 251U);
    }
    const auto source = temporary.path() / "source.bin";
    const auto destination = temporary.path() / "received";
    runner.expect(write_file(source, source_bytes), "source fixture is written");

    syncwire::UniqueFd client;
    syncwire::UniqueFd server;
    runner.expect(open_socket_pair(client, server), "socketpair opens for file upload");

    const syncwire::protocol::TransferLimits limits{.max_file_size = 10'000U, .chunk_size = 257U};
    std::optional<syncwire::protocol::FileTransferResult> server_result;
    std::jthread server_thread([&] {
        const auto received = syncwire::protocol::receive_frame(server.get());
        if (const auto* frame = std::get_if<syncwire::protocol::Frame>(&received);
            frame != nullptr) {
            server_result = syncwire::protocol::receive_file(server.get(), *frame, destination, limits);
        }
    });

    const auto client_result = syncwire::protocol::send_file(
        client.get(), source, "uploaded.bin", 77U, limits);
    server_thread.join();

    runner.expect(client_result.ok(), "client completes a multi-chunk upload");
    runner.expect(server_result.has_value() && server_result->ok(),
                  "server verifies and commits a multi-chunk upload");
    runner.expect(client_result.transferred == source_bytes.size(),
                  "client reports the transferred byte count");
    runner.expect(read_file(destination / "uploaded.bin") == source_bytes,
                  "committed file matches the source bytes");
    runner.expect(no_part_files(destination), "successful upload leaves no part file");
}

void test_checksum_mismatch_is_not_committed(TestRunner& runner) {
    TemporaryDirectory temporary;
    runner.expect(temporary.valid(), "temporary checksum directory opens");
    if (!temporary.valid()) {
        return;
    }
    const auto destination = temporary.path() / "received";
    const std::vector<std::byte> data{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

    syncwire::UniqueFd client;
    syncwire::UniqueFd server;
    runner.expect(open_socket_pair(client, server), "socketpair opens for checksum rejection");

    std::optional<syncwire::protocol::FileTransferResult> server_result;
    std::jthread server_thread([&] {
        const auto received = syncwire::protocol::receive_frame(server.get());
        if (const auto* frame = std::get_if<syncwire::protocol::Frame>(&received);
            frame != nullptr) {
            server_result = syncwire::protocol::receive_file(server.get(), *frame, destination);
        }
    });

    const std::uint64_t request_id = 88U;
    const syncwire::protocol::UploadMetadata metadata{
        .filename = "corrupt.bin",
        .file_size = data.size(),
        .checksum = syncwire::crc32(data) ^ 1U,
    };
    runner.expect(syncwire::protocol::send_frame(
                      client.get(),
                      make_frame(syncwire::protocol::MessageType::UploadRequest,
                                 request_id,
                                 0U,
                                 syncwire::protocol::encode_upload_metadata(metadata)))
                      .ok(),
                  "checksum test sends upload request");
    const auto ready = syncwire::protocol::receive_frame(client.get());
    runner.expect(std::holds_alternative<syncwire::protocol::Frame>(ready),
                  "checksum test receives transfer ready");
    runner.expect(syncwire::protocol::send_frame(
                      client.get(),
                      make_frame(syncwire::protocol::MessageType::FileChunk,
                                 request_id,
                                 request_id,
                                 syncwire::protocol::encode_file_chunk(0U, data)))
                      .ok(),
                  "checksum test sends file bytes");
    const auto acknowledgment = syncwire::protocol::receive_frame(client.get());
    runner.expect(std::holds_alternative<syncwire::protocol::Frame>(acknowledgment),
                  "checksum test receives acknowledgment");
    runner.expect(syncwire::protocol::send_frame(
                      client.get(),
                      make_frame(syncwire::protocol::MessageType::TransferComplete,
                                 request_id,
                                 request_id))
                      .ok(),
                  "checksum test completes the byte stream");
    const auto response = syncwire::protocol::receive_frame(client.get());
    server_thread.join();

    runner.expect(server_result.has_value() &&
                      server_result->status ==
                          syncwire::protocol::FileTransferStatus::ChecksumMismatch,
                  "server reports checksum mismatch");
    if (const auto* frame = std::get_if<syncwire::protocol::Frame>(&response); frame != nullptr) {
        const auto code = syncwire::protocol::decode_transfer_result(frame->payload);
        runner.expect(std::holds_alternative<syncwire::protocol::TransferResultCode>(code) &&
                          std::get<syncwire::protocol::TransferResultCode>(code) ==
                              syncwire::protocol::TransferResultCode::ChecksumMismatch,
                      "client receives checksum mismatch result");
    } else {
        runner.expect(false, "client receives checksum mismatch result");
    }
    runner.expect(!std::filesystem::exists(destination / "corrupt.bin"),
                  "checksum mismatch is not committed");
    runner.expect(no_part_files(destination), "checksum mismatch cleans up part file");
}

void test_noncontiguous_chunk_is_rejected(TestRunner& runner) {
    TemporaryDirectory temporary;
    runner.expect(temporary.valid(), "temporary offset directory opens");
    if (!temporary.valid()) {
        return;
    }
    const auto destination = temporary.path() / "received";
    const std::vector<std::byte> data{std::byte{0xAA}};

    syncwire::UniqueFd client;
    syncwire::UniqueFd server;
    runner.expect(open_socket_pair(client, server), "socketpair opens for offset rejection");

    std::optional<syncwire::protocol::FileTransferResult> server_result;
    std::jthread server_thread([&] {
        const auto received = syncwire::protocol::receive_frame(server.get());
        if (const auto* frame = std::get_if<syncwire::protocol::Frame>(&received);
            frame != nullptr) {
            server_result = syncwire::protocol::receive_file(server.get(), *frame, destination);
        }
    });

    const std::uint64_t request_id = 99U;
    const syncwire::protocol::UploadMetadata metadata{
        .filename = "offset.bin",
        .file_size = data.size(),
        .checksum = syncwire::crc32(data),
    };
    static_cast<void>(syncwire::protocol::send_frame(
        client.get(),
        make_frame(syncwire::protocol::MessageType::UploadRequest,
                   request_id,
                   0U,
                   syncwire::protocol::encode_upload_metadata(metadata))));
    static_cast<void>(syncwire::protocol::receive_frame(client.get()));
    static_cast<void>(syncwire::protocol::send_frame(
        client.get(),
        make_frame(syncwire::protocol::MessageType::FileChunk,
                   request_id,
                   request_id,
                   syncwire::protocol::encode_file_chunk(1U, data))));
    const auto response = syncwire::protocol::receive_frame(client.get());
    server_thread.join();

    runner.expect(server_result.has_value() &&
                      server_result->status ==
                          syncwire::protocol::FileTransferStatus::UnexpectedOffset,
                  "server rejects noncontiguous file chunk");
    if (const auto* frame = std::get_if<syncwire::protocol::Frame>(&response); frame != nullptr) {
        const auto code = syncwire::protocol::decode_transfer_result(frame->payload);
        runner.expect(std::holds_alternative<syncwire::protocol::TransferResultCode>(code) &&
                          std::get<syncwire::protocol::TransferResultCode>(code) ==
                              syncwire::protocol::TransferResultCode::UnexpectedOffset,
                      "client receives unexpected-offset result");
    } else {
        runner.expect(false, "client receives unexpected-offset result");
    }
    runner.expect(!std::filesystem::exists(destination / "offset.bin"),
                  "noncontiguous upload is not committed");
    runner.expect(no_part_files(destination), "offset rejection cleans up part file");
}

void test_client_preflight_validation(TestRunner& runner) {
    TemporaryDirectory temporary;
    runner.expect(temporary.valid(), "temporary preflight directory opens");
    if (!temporary.valid()) {
        return;
    }
    const std::vector<std::byte> data{std::byte{0x01}, std::byte{0x02}};
    const auto source = temporary.path() / "large.bin";
    runner.expect(write_file(source, data), "preflight source fixture is written");

    const auto unsafe = syncwire::protocol::send_file(-1, source, "../escape.bin", 1U);
    runner.expect(unsafe.status == syncwire::protocol::FileTransferStatus::InvalidPath,
                  "client rejects unsafe remote name before network I/O");

    const auto too_large = syncwire::protocol::send_file(
        -1,
        source,
        "large.bin",
        1U,
        syncwire::protocol::TransferLimits{.max_file_size = 1U, .chunk_size = 1U});
    runner.expect(too_large.status == syncwire::protocol::FileTransferStatus::FileTooLarge,
                  "client rejects oversized source before network I/O");
}

} // namespace

void run_file_transfer_tests(TestRunner& runner) {
    test_successful_multi_chunk_upload(runner);
    test_checksum_mismatch_is_not_committed(runner);
    test_noncontiguous_chunk_is_rejected(runner);
    test_client_preflight_validation(runner);
}
