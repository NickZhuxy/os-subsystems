#include "os/fd.hpp"
#include "os/test.hpp"
#include <fcntl.h>
#include <type_traits>
int main() {
    static_assert(!std::is_copy_constructible_v<os::Fd>);
    int raw = -1;
    {
        os::Fd fd(open("/dev/null", O_RDONLY | O_CLOEXEC));
        CHECK(fd.get() >= 0);
        raw = fd.get();
        os::Fd moved(std::move(fd));
        CHECK(fd.get() == -1);
        CHECK(moved.get() == raw);
        os::Fd assigned;
        assigned = std::move(moved);
        CHECK(moved.get() == -1);
        CHECK(fcntl(assigned.get(), F_GETFD) >= 0);
    }
    CHECK(fcntl(raw, F_GETFD) == -1 && errno == EBADF);
}
