// Python-style numeric formatting helpers (":.2f", ":g", rstrip tricks).
#pragma once

#include <cctype>
#include <cstdio>
#include <string>

namespace tuf {

inline std::string format_fixed(double value, int digits) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
    return std::string(buffer);
}

// f"{value:.Nf}".rstrip("0").rstrip(".")
inline std::string trim_fixed(double value, int digits) {
    std::string text = format_fixed(value, digits);
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    return text;
}

// f"{value:g}"
inline std::string format_g(double value) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return std::string(buffer);
}

// Digits printed after the decimal point of a numeric literal: "99.28" -> 2,
// "99" -> 0, "-9.9e1" -> 1 (mantissa only).  Unparsable input reports 0.
inline int decimal_places_of(const std::string& text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    if (start < end && (text[start] == '+' || text[start] == '-')) ++start;

    int digits = 0;
    bool seen_dot = false;
    for (size_t i = start; i < end; ++i) {
        const char ch = text[i];
        if (ch == '.') {
            if (seen_dot) break;
            seen_dot = true;
            continue;
        }
        if (ch == 'e' || ch == 'E') break;
        if (ch < '0' || ch > '9') break;
        if (seen_dot) ++digits;
    }
    return digits;
}

}  // namespace tuf
