#pragma once
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace os {
class Arguments {
    std::unordered_map<std::string, std::string> values_;
public:
    Arguments(int argc, char** argv) {
        for (int i = 1; i < argc; i += 2) {
            std::string key = argv[i];
            if (!key.starts_with("--") || i + 1 >= argc || !values_.emplace(key, argv[i + 1]).second)
                throw std::invalid_argument("expected unique --option value pairs");
        }
    }
    std::string take(const std::string& key, const std::string& fallback = "") {
        auto it = values_.find(key);
        if (it == values_.end()) return fallback;
        auto result = it->second; values_.erase(it); return result;
    }
    std::uint64_t number(const std::string& key, std::uint64_t fallback, std::uint64_t low, std::uint64_t high) {
        auto text = take(key, std::to_string(fallback));
        std::uint64_t value = 0;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value < low || value > high)
            throw std::invalid_argument("invalid value for " + key);
        return value;
    }
    void finish() const {
        if (!values_.empty()) throw std::invalid_argument("unknown option: " + values_.begin()->first);
    }
};
}
