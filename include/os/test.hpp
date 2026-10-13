#pragma once
#include <stdexcept>
#include <string>
// Checks remain active in Release builds.
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)
template<class Exception, class F> void check_throws(F&& function) {
    try { function(); } catch (const Exception&) { return; }
    throw std::runtime_error("Expected exception was not thrown");
}
