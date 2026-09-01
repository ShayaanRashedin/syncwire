#include "test_harness.hpp"

#include "syncwire/common/unique_fd.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <utility>

namespace {

[[nodiscard]] bool is_closed(const int fd) {
    errno = 0;
    return ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

void test_default_and_scope_ownership(TestRunner& runner) {
    syncwire::UniqueFd empty;
    runner.expect(!empty.valid() && empty.get() == -1, "default UniqueFd is invalid");

    int descriptors[2]{};
    runner.expect(::pipe(descriptors) == 0, "pipe opens descriptors for ownership test");
    const int owned_fd = descriptors[0];
    {
        syncwire::UniqueFd owned(owned_fd);
        runner.expect(owned.valid() && owned.get() == owned_fd, "UniqueFd owns a descriptor");
    }
    runner.expect(is_closed(owned_fd), "UniqueFd destructor closes its descriptor");
    static_cast<void>(::close(descriptors[1]));
}

void test_move_and_reset(TestRunner& runner) {
    int first[2]{};
    int second[2]{};
    runner.expect(::pipe(first) == 0 && ::pipe(second) == 0,
                  "pipes open descriptors for move test");

    syncwire::UniqueFd source(first[0]);
    syncwire::UniqueFd moved(std::move(source));
    runner.expect(!source.valid() && moved.get() == first[0],
                  "move construction transfers ownership");

    const int replaced_fd = second[0];
    syncwire::UniqueFd destination(replaced_fd);
    destination = std::move(moved);
    runner.expect(!moved.valid() && destination.get() == first[0],
                  "move assignment transfers ownership");
    runner.expect(is_closed(replaced_fd), "move assignment closes previous destination");

    destination.reset();
    runner.expect(!destination.valid() && is_closed(first[0]), "reset closes owned descriptor");
    static_cast<void>(::close(first[1]));
    static_cast<void>(::close(second[1]));
}

void test_release(TestRunner& runner) {
    int descriptors[2]{};
    runner.expect(::pipe(descriptors) == 0, "pipe opens descriptors for release test");

    syncwire::UniqueFd owned(descriptors[0]);
    const int released = owned.release();
    runner.expect(!owned.valid() && released == descriptors[0], "release relinquishes ownership");
    runner.expect(::fcntl(released, F_GETFD) != -1, "released descriptor remains open");

    static_cast<void>(::close(released));
    static_cast<void>(::close(descriptors[1]));
}

} // namespace

void run_unique_fd_tests(TestRunner& runner) {
    test_default_and_scope_ownership(runner);
    test_move_and_reset(runner);
    test_release(runner);
}

