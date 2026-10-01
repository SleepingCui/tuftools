// Python-style numeric formatting helpers (":.2f", ":g", rstrip tricks).
#pragma once

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

}  // namespace tuf
