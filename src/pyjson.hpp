#pragma once

// Thin Python-semantics adapter over the vendored nlohmann/json library.
//
// All parsing, serialization and value storage are delegated to
// nlohmann::ordered_json (third_party/nlohmann/json.hpp, v3.11.3).  This header
// only adds the small amount of glue needed to behave like the Python dict /
// json module usage in the original tool:
//   * get(key) returns null instead of throwing on a missing key
//   * number_or() / as_string() / key_text() mirror Python's duck typing
//   * py_float_str() / py_round() / py_repr() mirror Python's repr / round
//   * url_quote() mirrors urllib.parse.quote's default safe set
//   * dump(indent) keeps Python's json.dumps layout (indent >= 0)

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace tuf {

using OrderedJson = nlohmann::ordered_json;

inline std::string py_float_str(double value);

inline double py_round(double value, int digits);

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    Json() : value_(nullptr) {}
    Json(std::nullptr_t) : value_(nullptr) {}
    Json(bool value) : value_(value) {}
    Json(int value) : value_(value) {}
    Json(long long value) : value_(value) {}
    Json(unsigned long long value) : value_(value) {}
    Json(double value) : value_(value) {}
    Json(const char* value) : value_(std::string(value == nullptr ? "" : value)) {}
    Json(const std::string& value) : value_(value) {}
    Json(OrderedJson value) : value_(std::move(value)) {}

    // ------------------------------------------------------------- factories --
    static Json array() { return Json(OrderedJson::array()); }

    static Json array(const Array& items) {
        OrderedJson out = OrderedJson::array();
        for (const Json& item : items) out.push_back(item.value_);
        return Json(std::move(out));
    }

    static Json object() { return Json(OrderedJson::object()); }

    static Json object(const Object& items) {
        OrderedJson out = OrderedJson::object();
        for (const auto& item : items) out[item.first] = item.second.value_;
        return Json(std::move(out));
    }

    // Delegates to nlohmann's parser (throws nlohmann::json::parse_error).
    static Json parse(const std::string& text) { return Json(OrderedJson::parse(text)); }

    // ---------------------------------------------------------------- access --
    bool is_null() const { return value_.is_null(); }
    bool is_bool() const { return value_.is_boolean(); }
    bool is_int() const { return value_.is_number_integer(); }
    bool is_double() const { return value_.is_number_float(); }
    bool is_number() const { return value_.is_number(); }
    bool is_string() const { return value_.is_string(); }
    bool is_array() const { return value_.is_array(); }
    bool is_object() const { return value_.is_object(); }

    bool has(const std::string& key) const { return value_.is_object() && value_.contains(key); }

    // Missing key or non-object value yields null, matching dict.get().
    Json get(const std::string& key) const {
        if (!value_.is_object()) return Json();
        auto it = value_.find(key);
        if (it == value_.end()) return Json();
        return Json(*it);
    }

    void set(const std::string& key, const Json& value) {
        if (!value_.is_object()) value_ = OrderedJson::object();
        value_[key] = value.value_;
    }

    Object items() const {
        Object out;
        if (!value_.is_object()) return out;
        out.reserve(value_.size());
        for (auto it = value_.begin(); it != value_.end(); ++it) out.emplace_back(it.key(), Json(it.value()));
        return out;
    }

    Array elements() const {
        Array out;
        if (!value_.is_array()) return out;
        out.reserve(value_.size());
        for (const auto& item : value_) out.push_back(Json(item));
        return out;
    }

    size_t size() const {
        if (value_.is_array() || value_.is_object() || value_.is_string()) return value_.size();
        return 0;
    }

    Json operator[](size_t index) const {
        if (value_.is_array() && index < value_.size()) return Json(value_[index]);
        return Json();
    }

    // -------------------------------------------------------------- coercion --
    bool as_bool() const {
        if (value_.is_boolean()) return value_.get<bool>();
        return truthy();
    }

    // Python truthiness: null / 0 / "" / empty container are false.
    bool truthy() const {
        if (value_.is_null()) return false;
        if (value_.is_boolean()) return value_.get<bool>();
        if (value_.is_number()) return value_.get<double>() != 0.0;
        if (value_.is_string()) return !value_.get<std::string>().empty();
        return !value_.empty();
    }

    double as_double() const {
        if (value_.is_number()) return value_.get<double>();
        if (value_.is_boolean()) return value_.get<bool>() ? 1.0 : 0.0;
        if (value_.is_string()) {
            try {
                return std::stod(value_.get<std::string>());
            } catch (...) {
                return 0.0;
            }
        }
        return 0.0;
    }

    long long as_int() const {
        if (value_.is_number_integer()) return value_.get<long long>();
        if (value_.is_number_unsigned()) return static_cast<long long>(value_.get<unsigned long long>());
        if (value_.is_number_float()) return static_cast<long long>(value_.get<double>());
        if (value_.is_string()) {
            try {
                return std::stoll(value_.get<std::string>());
            } catch (...) {
                return 0;
            }
        }
        return 0;
    }

    std::string as_string() const {
        if (value_.is_string()) return value_.get<std::string>();
        return key_text();
    }

    std::string as_string_ref() const { return as_string(); }

    // Python str(value): used where the tool interpolates a raw value.
    std::string key_text() const {
        if (value_.is_null()) return "null";
        if (value_.is_boolean()) return value_.get<bool>() ? "true" : "false";
        if (value_.is_string()) return value_.get<std::string>();
        if (value_.is_number_integer()) return std::to_string(value_.get<long long>());
        if (value_.is_number_unsigned()) return std::to_string(value_.get<unsigned long long>());
        if (value_.is_number_float()) return py_float_str(value_.get<double>());
        return value_.dump();
    }

    double number_or(double fallback) const { return is_number() ? as_double() : fallback; }

    std::string dump(int indent = -1) const {
        if (indent < 0) return value_.dump();
        return value_.dump(indent);
    }

    const OrderedJson& raw() const { return value_; }

    friend bool operator==(const Json& lhs, const Json& rhs) { return lhs.value_ == rhs.value_; }
    friend bool operator!=(const Json& lhs, const Json& rhs) { return !(lhs == rhs); }

private:
    OrderedJson value_;
};

// ------------------------------------------------------------ python repr ---

inline std::string py_float_str(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";

    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    std::string text(buffer, result.ptr);

    // Python always renders floats with a fractional part ("1.0", not "1").
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

inline double py_round(double value, int digits) {
    if (!std::isfinite(value)) return value;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits < 0 ? 0 : digits, value);
    return std::strtod(buffer, nullptr);
}

inline std::string py_repr(const Json& value);

inline std::string py_repr_string(const std::string& text) {
    const bool has_single = text.find('\'') != std::string::npos;
    const bool has_double = text.find('"') != std::string::npos;
    const char quote = (has_single && !has_double) ? '"' : '\'';

    std::string out(1, quote);
    for (char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == quote) {
            out += '\\';
            out += c;
        } else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    out += quote;
    return out;
}

inline std::string py_repr(const Json& value) {
    if (value.is_null()) return "None";
    if (value.is_bool()) return value.as_bool() ? "True" : "False";
    if (value.is_string()) return py_repr_string(value.as_string());
    if (value.is_number()) return value.key_text();

    if (value.is_array()) {
        std::string out = "[";
        const Json::Array items = value.elements();
        for (size_t i = 0; i < items.size(); ++i) {
            if (i) out += ", ";
            out += py_repr(items[i]);
        }
        return out + "]";
    }

    std::string out = "{";
    const Json::Object items = value.items();
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ", ";
        out += py_repr_string(items[i].first) + ": " + py_repr(items[i].second);
    }
    return out + "}";
}

inline std::string py_repr_q(const Json& value) { return value.is_null() ? "?" : py_repr(value); }

// --------------------------------------------------------- urllib quoting --

inline std::string url_quote(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '_' || c == '-' || c == '.' || c == '~' || c == '/';
        if (unreserved) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

}  // namespace tuf
