#include "chartdl.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "api.hpp"
#include "apppaths.hpp"
#include "console.hpp"
#include "pyjson.hpp"

namespace tuf {
namespace {

constexpr std::size_t kMaxCustomPresets = 5;

std::string path_text(const std::filesystem::path& path) { return path.u8string(); }

bool all_hex_digits(const std::string& text) {
    if (text.empty()) return false;
    for (const char c : text) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::vector<std::string> split_commas(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : text) {
        if (c == ',') {
            parts.push_back(trim(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(trim(current));
    return parts;
}

std::string join_commas(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) out += ',';
        out += part;
    }
    return out;
}

// Suffixes "name_1.ext", "name_2.ext"... until the path is free.
std::filesystem::path free_path(std::filesystem::path path, bool& suffixed, std::string& error) {
    std::error_code fs_error;
    const bool exists = std::filesystem::exists(path, fs_error);
    if (fs_error) {
        error = "无法检查目标文件: " + fs_error.message();
        return path;
    }
    if (!exists) return path;
    const std::filesystem::path parent = path.parent_path();
    const std::string stem = path.stem().u8string();
    const std::string extension = path.extension().u8string();
    for (std::uint64_t suffix = 1;; ++suffix) {
        const std::filesystem::path candidate =
            parent / std::filesystem::u8path(stem + "_" + std::to_string(suffix) + extension);
        const bool candidate_exists = std::filesystem::exists(candidate, fs_error);
        if (fs_error) {
            error = "无法检查目标文件: " + fs_error.message();
            return path;
        }
        if (!candidate_exists) {
            suffixed = true;
            return candidate;
        }
    }
}

}  // namespace

const std::vector<TransformPreset>& builtin_transform_presets() {
    static const std::vector<TransformPreset> presets = {
        {"low_effects", "推荐低特效",
         TransformParams{{"keepEvents", "PositionTrack,RecolorTrack,ColorTrack,MoveTrack"}}},
        {"low_effects_camera", "推荐低特效+运镜",
         TransformParams{{"keepEvents",
                          "PositionTrack,RecolorTrack,ColorTrack,MoveCamera,RepeatEvents,MoveTrack"}}},
        {"no_decorations", "删装饰", TransformParams{{"dropEvents", "AddDecoration"}}},
        {"no_decorations_filters", "删装饰和滤镜",
         TransformParams{{"dropEvents", "AddDecoration,SetFilter"}}},
        {"essential_only", "去全部特效", TransformParams{{"keepEvents", "PositionTrack"}}},
    };
    return presets;
}

std::string transform_zip_url(const std::string& file_id, const TransformParams& params) {
    std::string url = BASE_URL + "/cdn/levels/" + url_quote(file_id) + "/transform?format=zip";
    for (const auto& param : params) url += "&" + param.first + "=" + url_quote(param.second);
    return url;
}

std::string level_v2_url(const std::string& file_id) {
    return BASE_URL + "/cdn/levels/" + url_quote(file_id) + "/level-v2.adofai";
}

std::string safe_level_filename(const Json& level, const std::string& suffix) {
    std::string name = "TUF_Level_" + level.get("id").key_text() + suffix;
    for (char& c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
            c == '*') {
            c = '_';
        }
    }
    return name;
}

Json fetch_cdn_data(const std::string& level_id) {
    return fetchapi_sync(BASE_URL + "/v2/database/levels/" + url_quote(level_id) + "/cdnData");
}

Json load_custom_presets() {
    Json root;
    std::string error;
    if (!settings_read("transformPresets", root, error)) throw std::runtime_error(error);
    Json::Array entries;
    if (!root.is_array()) return Json::array(entries);
    for (const Json& item : root.elements()) {
        if (entries.size() >= kMaxCustomPresets) break;
        if (!item.is_object() || !item.get("name").is_string() || !item.get("params").is_object()) continue;
        const std::string name = item.get("name").as_string();
        if (name.empty()) continue;
        Json entry = Json::object();
        entry.set("name", name);
        Json params = Json::object();
        for (const auto& param : item.get("params").items()) {
            if (param.second.is_string()) params.set(param.first, param.second.as_string());
        }
        entry.set("params", params);
        entries.push_back(entry);
    }
    return Json::array(entries);
}

bool save_custom_presets(const Json& presets, std::string& error) {
    Json::Array entries;
    if (presets.is_array()) {
        for (const Json& item : presets.elements()) {
            if (entries.size() >= kMaxCustomPresets) break;
            if (!item.is_object() || !item.get("name").is_string()) continue;
            Json entry = Json::object();
            entry.set("name", item.get("name").as_string());
            Json params = Json::object();
            if (item.get("params").is_object()) {
                for (const auto& param : item.get("params").items()) {
                    if (param.second.is_string()) params.set(param.first, param.second.as_string());
                }
            }
            entry.set("params", params);
            entries.push_back(entry);
        }
    }
    return settings_write("transformPresets", Json::array(entries), error);
}

bool append_transform_param(TransformParams& params, const std::string& key, const std::string& value,
                            std::string& error) {
    const std::string name = trim(key);
    if (name == "keepEvents" || name == "dropEvents" || name == "dropFilters") {
        const std::vector<std::string> parts = split_commas(value);
        for (const std::string& part : parts) {
            if (part.empty()) {
                error = "参数 \"" + name + "\" 里有空的事件/滤镜名";
                return false;
            }
        }
        params.emplace_back(name, join_commas(parts));
        return true;
    }
    if (name == "baseCameraZoom") {
        const std::optional<double> zoom = try_parse_double(trim(value));
        if (!zoom.has_value() || *zoom < 0.1 || *zoom > 10.0) {
            error = "相机缩放必须在 0.1 到 10 之间。";
            return false;
        }
        params.emplace_back(name, py_float_str(*zoom));
        return true;
    }
    if (name == "constantBackgroundColor") {
        std::string color = lower_ascii(trim(value));
        if (!color.empty() && color.front() == '#') color.erase(color.begin());
        if (!all_hex_digits(color) || (color.size() != 6 && color.size() != 8)) {
            error = "背景色需要是 6 位或 8 位十六进制 RGB(A)。";
            return false;
        }
        if (color.size() == 6) color += "ff";
        params.emplace_back(name, color);
        return true;
    }
    if (name == "removeForegroundFlash") {
        const std::string lowered = lower_ascii(trim(value));
        if (lowered == "true" || lowered == "1" || lowered == "y" || lowered == "yes") {
            params.emplace_back(name, "true");
            return true;
        }
        if (lowered == "false" || lowered == "0" || lowered == "n" || lowered == "no") {
            params.emplace_back(name, "false");
            return true;
        }
        error = "removeForegroundFlash 需要 true/false。";
        return false;
    }
    error = "未知转换参数 \"" + name +
            "\"（可用: keepEvents, dropEvents, dropFilters, baseCameraZoom, constantBackgroundColor, "
            "removeForegroundFlash）";
    return false;
}

ChartDownload download_to_file(const std::string& url, const std::string& path, bool expect_zip,
                               const HttpProgressCallback& progress) {
    ChartDownload out;
    if (url.empty()) {
        out.error = "该下载选项没有可用的链接。";
        return out;
    }

    std::error_code fs_error;
    std::filesystem::path output_path = std::filesystem::u8path(path);
    const std::filesystem::path parent = output_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, fs_error);
        if (fs_error) {
            out.error = "无法创建保存目录: " + fs_error.message();
            return out;
        }
    }
    output_path = free_path(output_path, out.suffixed, out.error);
    if (!out.error.empty()) return out;

    try {
        const HttpResponse response = http_get(url, active_proxy(), 60, progress);
        if (response.body.empty()) {
            out.error = "下载失败：服务器返回了空文件。";
            return out;
        }
        if (expect_zip && (response.body.size() < 4 || response.body[0] != 'P' || response.body[1] != 'K')) {
            out.error = "下载失败：服务器返回内容不是有效的 ZIP 谱面文件。";
            return out;
        }
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        if (!output.good()) {
            out.error = "无法写入文件: " + path_text(output_path);
            return out;
        }
        output.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
        output.close();
        if (!output.good()) {
            std::filesystem::remove(output_path, fs_error);
            out.error = "写入文件失败: " + path_text(output_path);
            return out;
        }
        out.ok = true;
        out.path = path_text(output_path);
        out.bytes = static_cast<std::uint64_t>(response.body.size());
    } catch (const std::exception& e) {
        out.error = std::string("下载失败: ") + e.what();
    }
    return out;
}

}  // namespace tuf
