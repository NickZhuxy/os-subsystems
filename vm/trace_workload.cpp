#include <cstddef>
#include <cstdint>
// Volatile makes the intended memory references observable to Lackey.
static volatile std::uint8_t pages[64 * 4096];
int main() {
    std::uint32_t checksum = 0;
    for (std::size_t round = 0; round < 256; ++round) {
        for (std::size_t page = 0; page < 64; ++page) {
            auto index = ((page * 17 + round) % 64) * 4096 + round % 64;
            pages[index] = static_cast<std::uint8_t>(page + round);
            checksum += pages[index];
        }
    }
    return checksum == 0xffffffffU;
}
