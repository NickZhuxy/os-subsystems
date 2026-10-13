#pragma once
#include <ostream>
#include <string>
inline void quoted(std::ostream& out, const std::string& text) {
    constexpr char hex[] = "0123456789abcdef"; out << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32 || c >= 127) out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else out << c;
    }
    out << '"';
}
