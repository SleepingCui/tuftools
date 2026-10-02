// Console helpers: UTF-8 setup and Python-like input() wrappers.
#pragma once

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace tuf {

inline void setup_console_utf8() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
}

// Best-effort terminal width, used to lay out adaptive tables. Falls back to
// `fallback` when stdout is not a console (piped or redirected output).
inline int terminal_width(int fallback = 120) {
#ifdef _WIN32
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &info)) {
        const int width = static_cast<int>(info.srWindow.Right - info.srWindow.Left) + 1;
        if (width > 0) return width;
    }
#else
    struct winsize size {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) return static_cast<int>(size.ws_col);
#endif
    return fallback;
}

inline std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    auto ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (b < e && ws(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && ws(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

inline std::string lower_ascii(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline std::string upper_ascii(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// Raised when stdin reaches EOF, so piped input cannot spin the menus forever.
struct InputClosed : std::runtime_error {
    InputClosed() : std::runtime_error("stdin closed") {}
};

// Python input(prompt): prints the prompt, then reads one line.
// EOF raises InputClosed (Python's input() raises EOFError there too).
inline std::string read_line(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();
    std::string line;
    if (!std::getline(std::cin, line)) throw InputClosed();
    return line;
}

inline std::string read_trimmed(const std::string& prompt) { return trim(read_line(prompt)); }

// Raises like Python's float("abc") -> ValueError; caller decides how to react.
inline double parse_double_or_throw(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty()) throw std::runtime_error("could not convert string to float: ''");
    size_t used = 0;
    double value = 0.0;
    try {
        value = std::stod(t, &used);
    } catch (const std::exception&) {
        throw std::runtime_error("could not convert string to float: '" + t + "'");
    }
    if (used != t.size()) throw std::runtime_error("could not convert string to float: '" + t + "'");
    return value;
}

inline long long parse_int_or_throw(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty()) throw std::runtime_error("invalid literal for int() with base 10: ''");
    size_t used = 0;
    long long value = 0;
    try {
        value = std::stoll(t, &used);
    } catch (const std::exception&) {
        throw std::runtime_error("invalid literal for int() with base 10: '" + t + "'");
    }
    if (used != t.size()) throw std::runtime_error("invalid literal for int() with base 10: '" + t + "'");
    return value;
}

inline std::optional<double> try_parse_double(const std::string& text) {
    try {
        return parse_double_or_throw(text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline std::optional<long long> try_parse_int(const std::string& text) {
    try {
        return parse_int_or_throw(text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace tuf
