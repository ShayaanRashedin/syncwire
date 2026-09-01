#pragma once

#include <unistd.h>

#include <utility>

namespace syncwire {

class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(const int fd) noexcept : fd_(fd) {}

    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return fd_ >= 0;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }

    void reset(const int new_fd = -1) noexcept {
        if (fd_ == new_fd) {
            return;
        }

        const int old_fd = std::exchange(fd_, new_fd);
        if (old_fd >= 0) {
            // Do not retry close() after EINTR on Linux: the descriptor may already
            // have been released and reused by another thread.
            static_cast<void>(::close(old_fd));
        }
    }

private:
    int fd_{-1};
};

} // namespace syncwire

