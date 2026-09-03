#include "test_harness.hpp"

#include "syncwire/common/authentication.hpp"
#include "syncwire/common/codec.hpp"
#include "syncwire/common/crc32.hpp"
#include "syncwire/common/directory_manifest.hpp"
#include "syncwire/common/file_transfer.hpp"
#include "syncwire/common/unique_fd.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace syncwire;
using namespace syncwire::protocol;
constexpr std::string_view secret = "resume-test-only-shared-secret";

class Fixture {
public:
    Fixture() {
        std::array<char, 40U> pattern{};
        const std::string value = "/tmp/syncwire-resume-XXXXXX";
        std::copy(value.begin(), value.end(), pattern.begin());
        if (const auto* created = ::mkdtemp(pattern.data())) {
            root = created;
            destination = root / "destination";
            std::filesystem::create_directory(destination);
        }
    }
    ~Fixture() {
        if (!root.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    }
    std::filesystem::path root;
    std::filesystem::path destination;
};

void write_bytes(const std::filesystem::path& path, const std::span<const std::byte> data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

bool matches(const std::filesystem::path& path, const std::vector<std::byte>& data) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::byte> actual(data.size());
    in.read(reinterpret_cast<char*>(actual.data()), static_cast<std::streamsize>(actual.size()));
    return in.gcount() == static_cast<std::streamsize>(actual.size()) && actual == data &&
           in.peek() == std::char_traits<char>::eof();
}

std::vector<std::filesystem::path> partials(const Fixture& fixture) {
    std::vector<std::filesystem::path> result;
    const auto directory = fixture.destination / kPartialDirectory;
    if (std::filesystem::exists(directory)) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            result.push_back(entry.path());
        }
    }
    return result;
}

Frame frame(const MessageType type, const std::uint64_t request, const std::uint64_t transfer,
            std::vector<std::byte> payload = {}) {
    return Frame{.header = FrameHeader{.message_type = type,
                                     .payload_length = static_cast<std::uint32_t>(payload.size()),
                                     .request_id = request, .transfer_id = transfer},
                 .payload = std::move(payload)};
}

// Each instance is a NEW authenticated connection with no shared in-memory transfer state.
class Session {
public:
    Session(const std::filesystem::path& destination, const TransferLimits limits = {}) {
        int fds[2]{};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
            return;
        }
        client.reset(fds[0]);
        server.reset(fds[1]);
        const timeval timeout{.tv_sec = 3, .tv_usec = 0};
        for (const int fd : fds) {
            static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
            static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
        }
        worker = std::jthread([this, destination, limits] {
            if (authenticate_server(server.get(), secret).ok()) {
                const auto request = receive_frame(server.get());
                if (const auto* value = std::get_if<Frame>(&request)) {
                    result = receive_file(server.get(), *value, destination, limits);
                }
            }
            static_cast<void>(::shutdown(server.get(), SHUT_RDWR));
        });
    }
    ~Session() {
        static_cast<void>(::shutdown(client.get(), SHUT_RDWR));
        static_cast<void>(::shutdown(server.get(), SHUT_RDWR));
        join();
    }
    bool authenticate() { return client && authenticate_client(client.get(), secret).ok(); }
    void join() { if (worker.joinable()) { worker.join(); } }
    UniqueFd client;
    UniqueFd server;
    std::optional<FileTransferResult> result;
    std::jthread worker;
};

bool seed_partial(TestRunner& runner, const Fixture& fixture, const std::vector<std::byte>& data,
                  const std::size_t prefix, const std::string& name = "result.bin") {
    Session session(fixture.destination);
    if (!session.authenticate()) {
        runner.expect(false, "interrupted connection authenticates");
        return false;
    }
    const UploadMetadata metadata{.filename = name, .file_size = data.size(), .checksum = crc32(data)};
    if (!send_frame(session.client.get(), frame(MessageType::UploadRequest, 11U, 0U,
                                               encode_upload_metadata(metadata))).ok()) {
        return false;
    }
    const auto response = receive_frame(session.client.get());
    const auto* ready = std::get_if<Frame>(&response);
    if (ready == nullptr || ready->header.message_type != MessageType::TransferReady) {
        runner.expect(false, "interrupted connection receives ready");
        return false;
    }
    runner.expect(decode_transfer_ready(ready->payload) == TransferReadyResult(TransferReady{}),
                  "new transfer offers zero offset and empty-prefix CRC");
    if (prefix > 0U) {
        runner.expect(send_frame(session.client.get(), frame(MessageType::FileChunk, 11U, 11U,
                      encode_file_chunk(0U, std::span<const std::byte>(data).first(prefix)))).ok(),
                      "seed writes one complete chunk");
        const auto ack = receive_frame(session.client.get());
        const auto* value = std::get_if<Frame>(&ack);
        runner.expect(value && value->header.message_type == MessageType::Acknowledgment &&
                          decode_offset(value->payload) == OffsetResult(prefix),
                      "complete chunk is acknowledged at its durable offset");
    }
    // EOF inside the next header must NOT append incomplete bytes to the stored prefix.
    const auto header = encode_header(FrameHeader{.message_type = MessageType::FileChunk,
                                                  .payload_length = 20U,
                                                  .request_id = 11U, .transfer_id = 11U});
    static_cast<void>(::send(session.client.get(), header.data(), 9U, MSG_NOSIGNAL));
    session.client.reset();
    session.join();
    runner.expect(session.result && session.result->status == FileTransferStatus::FrameIoError,
                  "disconnect is reported as transport failure");
    const auto saved = partials(fixture);
    const bool retained = saved.size() == 1U && std::filesystem::file_size(saved[0]) == prefix;
    runner.expect(retained, "only complete bytes survive an incomplete wire frame");
    return retained;
}

void test_resume_case(TestRunner& runner, const std::size_t prefix, const int mutation) {
    Fixture fixture;
    runner.expect(!fixture.root.empty(), "resume fixture opens");
    if (fixture.root.empty()) { return; }
    std::vector<std::byte> data(4097U);
    for (std::size_t i = 0; i < data.size(); ++i) { data[i] = static_cast<std::byte>(i % 251U); }
    const std::vector<std::byte> old{std::byte{0x5A}};
    write_bytes(fixture.destination / "result.bin", old);
    if (!seed_partial(runner, fixture, data, prefix)) { return; }
    runner.expect(matches(fixture.destination / "result.bin", old),
                  "interruption preserves the previous committed destination");
    if (mutation == 1) {
        std::fstream corrupt(partials(fixture)[0], std::ios::binary | std::ios::in | std::ios::out);
        corrupt.put('X');
    } else if (mutation == 2) {
        data[0] = std::byte{0x77};
    } else if (mutation == 3) {
        std::filesystem::resize_file(partials(fixture)[0], data.size() + 1U);
    }
    const auto source = fixture.root / "source.bin";
    write_bytes(source, data);
    Session session(fixture.destination);
    runner.expect(session.authenticate(), "reconnected upload authenticates again");
    const auto result = send_file(session.client.get(), source, "result.bin", 999U);
    session.join();
    runner.expect(result.ok() && session.result && session.result->ok(),
                  "reconnected upload completes and commits");
    runner.expect(result.resumed_from == (mutation == 0 ? prefix : 0U),
                  "only a matching prefix is reused; corruption and changed sources restart");
    runner.expect(matches(fixture.destination / "result.bin", data),
                  "resumed destination matches the complete source byte-for-byte");
    runner.expect(partials(fixture).size() == (mutation == 2 ? 1U : 0U),
                  "success removes its state without deleting other transfer identities");
    const auto scan = scan_directory(fixture.destination);
    runner.expect(scan.ok() && scan.files.size() == 1U && scan.files[0].path == "result.bin",
                  "directory manifests never include internal resume state");
}

void test_storage_guards(TestRunner& runner) {
    Fixture fixture;
    if (fixture.root.empty()) { runner.expect(false, "storage fixture opens"); return; }
    const std::vector<std::byte> data(20U, std::byte{0x33});
    if (!seed_partial(runner, fixture, data, 5U)) { return; }
    const auto source = fixture.root / "source.bin";
    write_bytes(source, data);
    for (const auto limits : {TransferLimits{.max_partial_files = 1U},
                              TransferLimits{.max_partial_bytes = 24U}}) {
        Session session(fixture.destination, limits);
        runner.expect(session.authenticate(), "budget test authenticates");
        const auto result = send_file(session.client.get(), source, "different.bin", 123U);
        session.join();
        runner.expect(result.status == FileTransferStatus::RemoteRejected && partials(fixture).size() == 1U,
                      "file-count and byte budgets reject new state without deleting old state");
    }
    const auto saved = partials(fixture)[0];
    const auto outside = fixture.root / "outside.bin";
    write_bytes(outside, data);
    std::filesystem::remove(saved);
    for (const bool hard_link : {false, true}) {
        if (hard_link) { std::filesystem::create_hard_link(outside, saved); }
        else { std::filesystem::create_symlink(outside, saved); }
        Session session(fixture.destination);
        runner.expect(session.authenticate(), "link test authenticates");
        const auto result = send_file(session.client.get(), source, "result.bin", 124U);
        session.join();
        runner.expect(!result.ok() && matches(outside, data),
                      "resume storage refuses symlinks and multiply linked files without modifying targets");
        std::filesystem::remove(saved);
    }
    // Internal directories cannot be selected as a destination by an authenticated client.
    runner.expect(!is_safe_remote_path(".syncwire-partials/injected.part") &&
                      !is_safe_remote_path("nested/.syncwire-partials/injected.part"),
                  "reserved namespace is rejected at any path depth");
}

void test_ready_codec(TestRunner& runner) {
    const TransferReady ready{0x0102030405060708ULL, 0xAABBCCDDU};
    const auto encoded = encode_transfer_ready(ready);
    runner.expect(encoded.size() == 12U && encoded[0] == std::byte{1} &&
                      encoded[7] == std::byte{8} && encoded[8] == std::byte{0xAA} &&
                      encoded[11] == std::byte{0xDD}, "resume metadata uses network byte order");
    runner.expect(decode_transfer_ready(encoded) == TransferReadyResult(ready),
                  "resume metadata round-trips");
    for (const std::size_t size : {0U, 8U, 11U, 13U}) {
        runner.expect(std::holds_alternative<TransferCodecError>(
                          decode_transfer_ready(std::vector<std::byte>(size))),
                      "resume metadata rejects legacy, truncated, and overlong payloads");
    }
}
} // namespace

void run_resume_tests(TestRunner& runner) {
    test_ready_codec(runner);
    test_resume_case(runner, 257U, 0);
    test_resume_case(runner, 4097U, 0); // Disconnect after final ACK but before commit.
    test_resume_case(runner, 0U, 0);
    test_resume_case(runner, 257U, 1); // Corrupted stored prefix.
    test_resume_case(runner, 257U, 2); // Source changed between attempts.
    test_resume_case(runner, 257U, 3); // Oversized partial is restarted safely.
    test_storage_guards(runner);
}
