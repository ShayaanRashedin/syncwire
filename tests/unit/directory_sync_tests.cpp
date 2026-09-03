#include "test_harness.hpp"

#include "syncwire/common/crc32.hpp"
#include "syncwire/common/directory_manifest.hpp"
#include "syncwire/common/directory_sync.hpp"
#include "syncwire/common/file_transfer.hpp"
#include "syncwire/common/frame_io.hpp"
#include "syncwire/common/frame_parser.hpp"
#include "syncwire/common/sync_codec.hpp"
#include "syncwire/common/unique_fd.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::array<char, 32U> pattern{};
        constexpr std::string_view value = "/tmp/syncwire-sync-XXXXXX";
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

[[nodiscard]] std::vector<std::byte> bytes_of(const std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(first, first + text.size());
}

[[nodiscard]] bool write_file(const std::filesystem::path& path,
                              const std::string_view content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream output(path, std::ios::binary);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

[[nodiscard]] const syncwire::FileRecord*
find_record(const syncwire::FileManifest& files, const std::string_view path) {
    const auto found = std::ranges::lower_bound(files, path, {}, &syncwire::FileRecord::path);
    return found != files.end() && found->path == path ? &*found : nullptr;
}

struct SyncPairResult {
    syncwire::protocol::DirectorySyncResult client;
    std::optional<syncwire::protocol::DirectorySyncResult> server{};
};

[[nodiscard]] SyncPairResult run_sync_pair(const std::filesystem::path& source,
                                           const std::filesystem::path& destination,
                                           const std::uint64_t request_id,
                                           const syncwire::protocol::DirectorySyncLimits limits) {
    syncwire::UniqueFd client;
    syncwire::UniqueFd server;
    if (!open_socket_pair(client, server)) {
        return SyncPairResult{
            .client = syncwire::protocol::DirectorySyncResult{
                .status = syncwire::protocol::DirectorySyncStatus::FrameIoError,
            },
        };
    }

    std::optional<syncwire::protocol::DirectorySyncResult> server_result;
    std::jthread server_thread([&] {
        const auto received = syncwire::protocol::receive_frame(server.get());
        if (const auto* frame = std::get_if<syncwire::protocol::Frame>(&received);
            frame != nullptr) {
            server_result = syncwire::protocol::receive_directory_sync(
                server.get(), *frame, destination, limits);
        }
    });
    auto client_result = syncwire::protocol::sync_directory(
        client.get(), source, request_id, limits);
    server_thread.join();
    return SyncPairResult{
        .client = std::move(client_result),
        .server = std::move(server_result),
    };
}

void test_recursive_manifest_scan(TestRunner& runner) {
    TemporaryDirectory temporary;
    runner.expect(temporary.valid(), "temporary manifest directory opens");
    if (!temporary.valid()) {
        return;
    }
    const auto root = temporary.path() / "source";
    runner.expect(write_file(root / "z.txt", "last"), "top-level manifest fixture is written");
    runner.expect(write_file(root / "nested" / "a.txt", "first"),
                  "nested manifest fixture is written");

    std::error_code symlink_error;
    std::filesystem::create_symlink(root / "z.txt", root / "alias.txt", symlink_error);
    runner.expect(!symlink_error, "manifest symlink fixture is created");

    const auto scan = syncwire::scan_directory(root);
    runner.expect(scan.ok(), "recursive manifest scan succeeds");
    runner.expect(scan.files.size() == 2U, "manifest includes regular files but skips symlinks");
    runner.expect(scan.skipped_symlinks == 1U, "manifest reports skipped symlink count");
    runner.expect(scan.files.size() == 2U && scan.files[0].path == "nested/a.txt" &&
                      scan.files[1].path == "z.txt",
                  "manifest paths are normalized and sorted");

    const auto* nested = find_record(scan.files, "nested/a.txt");
    const auto nested_bytes = bytes_of("first");
    runner.expect(nested != nullptr && nested->size == nested_bytes.size() &&
                      nested->checksum == syncwire::crc32(nested_bytes),
                  "manifest records file size and CRC-32");
}

void test_manifest_and_plan_codecs(TestRunner& runner) {
    const syncwire::FileManifest source{
        syncwire::FileRecord{.path = "a.txt", .size = 1U, .checksum = 11U},
        syncwire::FileRecord{.path = "nested/b.txt", .size = 2U, .checksum = 22U},
        syncwire::FileRecord{.path = "same.txt", .size = 3U, .checksum = 33U},
    };
    const syncwire::FileManifest destination{
        syncwire::FileRecord{.path = "nested/b.txt", .size = 9U, .checksum = 99U},
        syncwire::FileRecord{.path = "same.txt", .size = 3U, .checksum = 33U},
        syncwire::FileRecord{.path = "server-only.txt", .size = 4U, .checksum = 44U},
    };

    const auto encoded_manifest = syncwire::protocol::encode_manifest(source);
    const auto decoded_manifest = syncwire::protocol::decode_manifest(encoded_manifest);
    runner.expect(std::holds_alternative<syncwire::FileManifest>(decoded_manifest) &&
                      std::get<syncwire::FileManifest>(decoded_manifest) == source,
                  "directory manifest round-trips through the wire codec");

    const auto plan = syncwire::protocol::build_sync_plan(source, destination);
    runner.expect(plan.upload_paths == std::vector<std::string>{"a.txt", "nested/b.txt"},
                  "sync plan requests missing and changed files");
    runner.expect(plan.unchanged_count == 1U, "sync plan counts unchanged files");
    runner.expect(plan.server_only_count == 1U, "sync plan counts server-only files");

    const auto encoded_plan = syncwire::protocol::encode_sync_plan(plan);
    const auto decoded_plan = syncwire::protocol::decode_sync_plan(encoded_plan);
    runner.expect(std::holds_alternative<syncwire::protocol::SyncPlan>(decoded_plan) &&
                      std::get<syncwire::protocol::SyncPlan>(decoded_plan) == plan,
                  "sync plan round-trips through the wire codec");
}

void test_incremental_directory_sync(TestRunner& runner) {
    TemporaryDirectory temporary;
    runner.expect(temporary.valid(), "temporary end-to-end sync directory opens");
    if (!temporary.valid()) {
        return;
    }
    const auto source = temporary.path() / "source";
    const auto destination = temporary.path() / "destination";
    runner.expect(write_file(source / "same.txt", "same"), "source unchanged fixture is written");
    runner.expect(write_file(source / "changed.txt", "new-value"),
                  "source changed fixture is written");
    runner.expect(write_file(source / "nested" / "new.txt", "nested-value"),
                  "source nested fixture is written");
    runner.expect(write_file(destination / "same.txt", "same"),
                  "destination unchanged fixture is written");
    runner.expect(write_file(destination / "changed.txt", "old"),
                  "destination stale fixture is written");
    runner.expect(write_file(destination / "server-only.txt", "preserve-me"),
                  "destination-only fixture is written");

    const syncwire::protocol::DirectorySyncLimits limits{
        .scan = syncwire::DirectoryScanLimits{},
        .transfer = syncwire::protocol::TransferLimits{
            .max_file_size = 1'024U,
            .chunk_size = 3U,
        },
    };
    const auto first = run_sync_pair(source, destination, 500U, limits);
    runner.expect(first.client.ok(), "client completes recursive directory synchronization");
    runner.expect(first.server.has_value() && first.server->ok(),
                  "server verifies recursive directory synchronization");
    runner.expect(first.client.planned_uploads == 2U &&
                      first.client.completed_uploads == 2U,
                  "first sync uploads only missing and changed files");
    runner.expect(first.client.unchanged_files == 1U && first.client.server_only_files == 1U,
                  "first sync reports unchanged and preserved server-only files");
    runner.expect(read_file(destination / "changed.txt") == "new-value",
                  "changed destination file is replaced");
    runner.expect(read_file(destination / "nested" / "new.txt") == "nested-value",
                  "nested destination file is created");
    runner.expect(read_file(destination / "server-only.txt") == "preserve-me",
                  "server-only file is preserved");

    const auto second = run_sync_pair(source, destination, 700U, limits);
    runner.expect(second.client.ok() && second.server.has_value() && second.server->ok(),
                  "second synchronization succeeds");
    runner.expect(second.client.planned_uploads == 0U &&
                      second.client.completed_uploads == 0U &&
                      second.client.unchanged_files == 3U &&
                      second.client.server_only_files == 1U,
                  "second sync transfers nothing when source files are unchanged");
}

void test_symlink_parent_cannot_escape_root(TestRunner& runner) {
    TemporaryDirectory temporary;
    runner.expect(temporary.valid(), "temporary symlink-containment directory opens");
    if (!temporary.valid()) {
        return;
    }
    const auto source = temporary.path() / "source";
    const auto destination = temporary.path() / "destination";
    const auto outside = temporary.path() / "outside";
    runner.expect(write_file(source / "nested" / "blocked.txt", "blocked"),
                  "symlink-containment source is written");
    std::error_code error;
    std::filesystem::create_directories(destination, error);
    std::filesystem::create_directories(outside, error);
    std::filesystem::create_directory_symlink(outside, destination / "nested", error);
    runner.expect(!error, "destination symlink fixture is created");

    const auto sync = run_sync_pair(
        source, destination, 900U, syncwire::protocol::DirectorySyncLimits{});
    runner.expect(sync.client.status ==
                      syncwire::protocol::DirectorySyncStatus::FileTransferError,
                  "client observes server rejection for symlinked parent");
    runner.expect(sync.server.has_value() &&
                      sync.server->status ==
                          syncwire::protocol::DirectorySyncStatus::FileTransferError &&
                      sync.server->file_transfer.status ==
                          syncwire::protocol::FileTransferStatus::InvalidPath,
                  "server rejects destination path through a symlink");
    runner.expect(!std::filesystem::exists(outside / "blocked.txt"),
                  "symlinked directory cannot redirect an upload outside the root");
}

void test_invalid_manifest_order(TestRunner& runner) {
    const syncwire::FileManifest unsorted{
        syncwire::FileRecord{.path = "z.txt", .size = 1U},
        syncwire::FileRecord{.path = "a.txt", .size = 1U},
    };
    runner.expect(syncwire::protocol::encode_manifest(unsorted).empty(),
                  "manifest encoder rejects unsorted paths");

    const auto missing = syncwire::scan_directory("/path/that/does/not/exist");
    runner.expect(missing.error == syncwire::DirectoryScanError::RootNotDirectory,
                  "scanner rejects a missing root directory");
}

} // namespace

void run_directory_sync_tests(TestRunner& runner) {
    test_recursive_manifest_scan(runner);
    test_manifest_and_plan_codecs(runner);
    test_incremental_directory_sync(runner);
    test_symlink_parent_cannot_escape_root(runner);
    test_invalid_manifest_order(runner);
}

