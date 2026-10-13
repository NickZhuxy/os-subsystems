#include "os/rpc.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace os::rpc {
namespace {
void put(Bytes& out, std::uint64_t value, unsigned count) {
    for (unsigned i = count; i > 0; --i)
        out.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
}
std::uint64_t get(std::span<const std::uint8_t> input) {
    std::uint64_t value = 0;
    for (auto byte : input) value = (value << 8) | byte;
    return value;
}
constexpr std::uint32_t magic = 0x4f535250; // OSRP
}
void put_u64(Bytes& bytes, std::uint64_t value) { put(bytes, value, 8); }
std::uint64_t get_u64(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 8) throw std::invalid_argument("u64 requires 8 bytes");
    return get(bytes);
}
Bytes encode(const Frame& frame) {
    if (frame.body.size() > max_payload || frame.id == 0 ||
        frame.type < Type::request || frame.type > Type::error)
        throw std::invalid_argument("invalid frame");
    Bytes out;
    out.reserve(header_size + frame.body.size());
    put(out, magic, 4); put(out, 1, 2); put(out, static_cast<unsigned>(frame.type), 2);
    put(out, frame.id, 8); put(out, frame.body.size(), 4);
    out.insert(out.end(), frame.body.begin(), frame.body.end());
    return out;
}
std::optional<Decoded> decode(std::span<const std::uint8_t> input) {
    if (input.size() < header_size) return std::nullopt;
    if (get(input.first(4)) != magic || get(input.subspan(4, 2)) != 1)
        throw std::invalid_argument("invalid magic or version");
    auto type = get(input.subspan(6, 2));
    auto id = get(input.subspan(8, 8));
    auto length = get(input.subspan(16, 4));
    if (type < 1 || type > 3 || id == 0 || length > max_payload)
        throw std::invalid_argument("invalid type, ID, or length");
    auto total = header_size + static_cast<std::size_t>(length);
    if (input.size() < total) return std::nullopt;
    return Decoded{Frame{static_cast<Type>(type), id,
        Bytes(input.begin() + header_size, input.begin() + total)}, total};
}
Bytes echo_request(std::span<const std::uint8_t> body) {
    Bytes request{1};
    request.insert(request.end(), body.begin(), body.end());
    return request;
}
Bytes add_request(std::uint64_t left, std::uint64_t right) {
    Bytes request{2}; put_u64(request, left); put_u64(request, right); return request;
}
Bytes delayed_echo_request(std::uint64_t delay_ms, std::span<const std::uint8_t> body) {
    Bytes request{3}; put_u64(request, delay_ms);
    request.insert(request.end(), body.begin(), body.end()); return request;
}
Reply builtin_handler(std::span<const std::uint8_t> request) {
    if (request.empty()) throw std::invalid_argument("missing method");
    switch (request[0]) {
    case 1: return {Bytes(request.begin() + 1, request.end()), std::chrono::milliseconds(0)};
    case 2: {
        if (request.size() != 17) throw std::invalid_argument("add requires two u64 values");
        Bytes result;
        put_u64(result, get_u64(request.subspan(1, 8)) + get_u64(request.subspan(9, 8)));
        return {std::move(result), std::chrono::milliseconds(0)}; // Addition modulo 2^64.
    }
    case 3: {
        if (request.size() < 9) throw std::invalid_argument("delay requires u64 milliseconds");
        auto delay = get_u64(request.subspan(1, 8));
        if (delay > 60000) throw std::invalid_argument("delay exceeds 60000 ms");
        return {Bytes(request.begin() + 9, request.end()), std::chrono::milliseconds(delay)};
    }
    default: throw std::invalid_argument("unknown method");
    }
}
double percentile(const std::vector<Sample>& samples, double fraction) {
    if (samples.empty() || fraction <= 0 || fraction > 1)
        throw std::invalid_argument("invalid percentile");
    std::vector<double> sorted;
    sorted.reserve(samples.size());
    for (const auto& sample : samples) sorted.push_back(sample.latency_us);
    std::sort(sorted.begin(), sorted.end());
    return sorted[static_cast<std::size_t>(std::ceil(fraction * sorted.size())) - 1];
}
}
