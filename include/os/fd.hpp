#pragma once
#include <unistd.h>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace os {
inline void system_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
class Fd {
    int value_ = -1;
public:
    explicit Fd(int value = -1) noexcept : value_(value) {}
    ~Fd() { if (value_ >= 0) ::close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    int get() const noexcept { return value_; }
};
}
