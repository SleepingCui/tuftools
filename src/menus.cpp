#include "menus.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "api.hpp"
#include "apppaths.hpp"
#include "console.hpp"
#include "http.hpp"
#include "info.hpp"
#include "pyjson.hpp"
#include "numfmt.hpp"
#include "tools.hpp"
#include "xacc_solver.hpp"

namespace tuf {

namespace {

std::string pad_right(const std::string& text, size_t width) {
    std::string out = text;
    while (out.size() < width) out += ' ';
    return out;
}

std::optional<long long> rank_int(const RankVal& value) {
    if (!value.numeric) return std::nullopt;
    return static_cast<long long>(value.num);
}

std::string rank_delta(const RankVal& old_rank, const RankVal& new_rank) {
    const std::optional<long long> before = rank_int(old_rank);
    const std::optional<long long> after = rank_int(new_rank);
    if (!before.has_value() || !after.has_value()) return "N/A";
    const long long delta = *before - *after;
    return delta >= 0 ? "+" + std::to_string(delta) : std::to_string(delta);
}

RankVal as_one_if_zero(RankVal value) { return value.is_zero() ? RankVal::from_number(1.0) : value; }

std::string float_of(const Json& value) { return value.key_text(); }

Json fetch_level_by_id(const std::string& id) {
    Json response = fetchapi_sync(BASE_URL + "/v2/database/levels/byId/" + url_quote(id));
    Json level = response.get("level");
    if (level.is_null() && response.get("id").key_text() == id) level = response;
    if (level.is_null()) throw std::runtime_error("API 返回内容中没有谱面详情");
    return level;
}

Json search_level(bool allow_sort_selection = false) {
    constexpr long long kSearchLimit = 10;
    std::string sort = "CLEARS_DESC";
    std::string sort_label = "通关数降序";
    if (allow_sort_selection) {
        const std::vector<std::pair<std::string, std::string>> sort_options = {
            {"CLEARS", "通关数"}, {"DOWNLOADS", "下载数"}, {"LIKES", "点赞数"},
            {"FAVORITES", "收藏数"}, {"BPM", "BPM"}, {"TILECOUNT", "砖块数"},
            {"DURATION", "时长"}, {"BASESCORE", "BaseScore"}, {"ID", "谱面 ID"}};
        std::cout << "搜索排序（默认通关数降序）:" << std::endl;
        for (size_t i = 0; i < sort_options.size(); ++i) {
            std::cout << (i + 1) << ". " << sort_options[i].second << std::endl;
        }
        const std::string sort_choice = read_trimmed("排序字段（回车默认通关数）: ");
        if (!sort_choice.empty()) {
            const std::optional<long long> index = try_parse_int(sort_choice);
            if (!index || *index < 1 || static_cast<size_t>(*index) > sort_options.size()) {
                std::cout << "排序字段无效，使用通关数降序。" << std::endl;
            } else {
                sort = sort_options[static_cast<size_t>(*index - 1)].first;
                sort_label = sort_options[static_cast<size_t>(*index - 1)].second;
                const std::string order = lower_ascii(read_trimmed("排序方向（1 降序 / 2 升序，默认降序）: "));
                if (order == "2") {
                    sort += "_ASC";
                    sort_label += "升序";
                } else {
                    sort += "_DESC";
                    sort_label += "降序";
                }
            }
        }
    }
    while (true) {
        const std::string query = read_trimmed("输入谱面名或 ID（回车取消）: ");
        if (query.empty()) return Json();

        const bool numeric_id = std::all_of(query.begin(), query.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
        if (numeric_id) {
            try {
                return fetch_level_by_id(query);
            } catch (const std::exception& e) {
                std::cout << "按谱面 ID 查询失败: " << e.what() << std::endl;
                continue;
            }
        }

        long long offset = 0;
        while (true) {
            Json response;
            try {
                const std::string url = BASE_URL + "/v2/database/levels?query=" + url_quote(query) +
                                        "&sort=" + url_quote(sort) + "&limit=" + std::to_string(kSearchLimit) +
                                        "&offset=" + std::to_string(offset);
                response = fetchapi_sync(url);
            } catch (const std::exception& e) {
                std::cout << "搜索谱面失败: " << e.what() << std::endl;
                break;
            }

            const std::vector<Json> levels = response.get("results").elements();
            if (levels.empty()) {
                if (offset == 0) std::cout << "没有找到谱面，请尝试更短的关键词。" << std::endl;
                else std::cout << "已经是最后一页。" << std::endl;
                break;
            }

            std::cout << "搜索结果（按" << sort_label << "；第 " << (offset / kSearchLimit + 1) << " 页）：" << std::endl;
            for (size_t i = 0; i < levels.size(); ++i) {
                const Json& level = levels[i];
                const Json& difficulty = level.get("difficulty");
                std::cout << "[" << (i + 1) << "] passes=" << level.get("clears").key_text()
                          << " | ID=" << level.get("id").key_text() << " | "
                          << level.get("song").as_string() << " - " << level.get("artist").as_string() << " | "
                          << difficulty.get("name").as_string() << " | tiles=" << level.get("tilecount").key_text()
                          << " | baseScore=" << level.get("baseScore").key_text()
                          << " | diffBase=" << difficulty.get("baseScore").key_text()
                          << " | ppBase=" << level.get("ppBaseScore").key_text() << std::endl;
            }

            const bool has_more = response.get("hasMore").truthy();
            std::cout << "输入序号选择；n 下一页，p 上一页，0 重新搜索";
            if (!has_more) std::cout << "（最后一页）";
            const std::string choice = read_trimmed(": ");
            if (choice == "0") break;
            if (choice == "n" || choice == "N") {
                if (has_more) offset += kSearchLimit;
                else std::cout << "已经是最后一页。" << std::endl;
                continue;
            }
            if (choice == "p" || choice == "P") {
                if (offset >= kSearchLimit) offset -= kSearchLimit;
                else std::cout << "已经是第一页。" << std::endl;
                continue;
            }
            const std::optional<long long> index = try_parse_int(choice);
            if (!index.has_value() || *index < 1 || static_cast<size_t>(*index) > levels.size()) {
                std::cout << "输入无效。" << std::endl;
                continue;
            }
            return levels[static_cast<size_t>(*index - 1)];
        }
    }
}

std::string join_csv(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) out += ',';
        out += value;
    }
    return out;
}

using TransformParams = std::vector<std::pair<std::string, std::string>>;

struct TransformPreset {
    std::string name;
    TransformParams params;
};

bool select_option_names(const std::string& title, const std::vector<std::string>& names,
                         std::vector<std::string>& selected) {
    if (names.empty()) return true;
    std::cout << title << std::endl;
    for (size_t i = 0; i < names.size(); ++i) std::cout << "[" << (i + 1) << "] " << names[i] << std::endl;
    const std::string input = read_trimmed("输入序号（空格分隔）、all 全选，或回车跳过: ");
    if (input.empty()) return true;
    if (lower_ascii(input) == "all") {
        selected = names;
        return true;
    }
    std::istringstream stream(input);
    std::string token;
    std::vector<bool> seen(names.size(), false);
    while (stream >> token) {
        const std::optional<long long> index = try_parse_int(token);
        if (!index || *index < 1 || static_cast<size_t>(*index) > names.size()) {
            std::cout << "序号无效，请按列表输入 1 到 " << names.size() << " 之间的序号。" << std::endl;
            selected.clear();
            return false;
        }
        if (!seen[static_cast<size_t>(*index - 1)]) {
            selected.push_back(names[static_cast<size_t>(*index - 1)]);
            seen[static_cast<size_t>(*index - 1)] = true;
        }
    }
    return true;
}

std::vector<std::string> json_strings(const std::vector<Json>& values) {
    std::vector<std::string> result;
    for (const Json& value : values) if (value.is_string()) result.push_back(value.as_string());
    return result;
}

bool load_transform_presets(std::vector<TransformPreset>& presets) {
    try {
        Json root;
        std::string error;
        if (!settings_read("transformPresets", root, error)) throw std::runtime_error(error);
        if (root.is_null()) return true;
        for (const Json& item : root.elements()) {
            if (presets.size() == 5) break;
            if (!item.is_object() || !item.get("name").is_string() || !item.get("params").is_object()) continue;
            TransformPreset preset;
            preset.name = item.get("name").as_string();
            for (const auto& param : item.get("params").items()) {
                if (param.second.is_string()) preset.params.emplace_back(param.first, param.second.as_string());
            }
            if (!preset.name.empty()) presets.push_back(std::move(preset));
        }
        return true;
    } catch (const std::exception& e) {
        std::cout << "读取自定义预设失败: " << e.what() << std::endl;
        return false;
    }
}

bool save_transform_presets(const std::vector<TransformPreset>& presets) {
    Json::Array entries;
    for (const TransformPreset& preset : presets) {
        Json item = Json::object();
        item.set("name", Json(preset.name));
        Json params = Json::object();
        for (const auto& param : preset.params) params.set(param.first, Json(param.second));
        item.set("params", params);
        entries.push_back(item);
    }
    std::string error;
    if (!settings_write("transformPresets", Json::array(entries), error)) {
        std::cout << "保存自定义预设失败: " << error << std::endl;
        return false;
    }
    return true;
}

bool build_custom_transform_params(const Json& options, TransformParams& params) {
    params.clear();
    const std::vector<std::string> event_types = json_strings(options.get("eventTypes").elements());
    const std::string event_mode = lower_ascii(read_trimmed("事件处理 (d 删除指定事件 / k 只保留指定事件 / 回车跳过): "));
    if (event_mode == "d" || event_mode == "k") {
        std::vector<std::string> events;
        if (!select_option_names("可选事件:", event_types, events)) return false;
        if (!events.empty()) params.emplace_back(event_mode == "d" ? "dropEvents" : "keepEvents", join_csv(events));
    } else if (!event_mode.empty()) {
        std::cout << "事件处理选项无效。" << std::endl;
        return false;
    }

    const std::string zoom_input = read_trimmed("相机缩放 baseCameraZoom（0.1-10，回车保持默认）: ");
    if (!zoom_input.empty()) {
        const std::optional<double> zoom = try_parse_double(zoom_input);
        if (!zoom.has_value() || *zoom < 0.1 || *zoom > 10.0) {
            std::cout << "相机缩放必须在 0.1 到 10 之间。" << std::endl;
            return false;
        }
        params.emplace_back("baseCameraZoom", py_float_str(*zoom));
    }

    std::string background = read_trimmed("自定义背景色 RGB（例如 #112233，回车跳过）: ");
    if (!background.empty()) {
        if (background.front() == '#') background.erase(background.begin());
        if (background.size() != 6 || !std::all_of(background.begin(), background.end(), [](unsigned char c) {
                return std::isxdigit(c) != 0;
            })) {
            std::cout << "背景色需要是 6 位十六进制 RGB。" << std::endl;
            return false;
        }
        long long opacity = 100;
        const std::string opacity_input = read_trimmed("背景不透明度百分比（0-100，默认100）: ");
        if (!opacity_input.empty()) {
            const std::optional<long long> parsed = try_parse_int(opacity_input);
            if (!parsed.has_value() || *parsed < 0 || *parsed > 100) {
                std::cout << "不透明度必须在 0 到 100 之间。" << std::endl;
                return false;
            }
            opacity = *parsed;
        }
        static const char* hex = "0123456789abcdef";
        const unsigned int alpha = static_cast<unsigned int>((opacity * 255 + 50) / 100);
        background += hex[(alpha >> 4) & 0xF];
        background += hex[alpha & 0xF];
        params.emplace_back("constantBackgroundColor", background);
    }

    if (lower_ascii(read_trimmed("移除前景闪光？(y/N): ")) == "y") params.emplace_back("removeForegroundFlash", "true");

    std::vector<std::string> filter_names = json_strings(options.get("filterTypes").elements());
    const std::vector<std::string> advanced = json_strings(options.get("advancedFilterTypes").elements());
    filter_names.insert(filter_names.end(), advanced.begin(), advanced.end());
    std::vector<std::string> filters;
    if (!select_option_names("可删除滤镜:", filter_names, filters)) return false;
    if (!filters.empty()) params.emplace_back("dropFilters", join_csv(filters));
    return true;
}

void show_transform_params(const TransformPreset& preset) {
    std::cout << preset.name << ":" << std::endl;
    if (preset.params.empty()) std::cout << "  （无转换参数）" << std::endl;
    for (const auto& param : preset.params) std::cout << "  " << param.first << " = " << param.second << std::endl;
}

void manage_transform_presets(const Json& options, std::vector<TransformPreset>& presets) {
    while (true) {
        std::cout << "\n自定义转换预设（" << presets.size() << "/5）" << std::endl;
        std::cout << "持久化文件: " << data_file("settings.json") << std::endl;
        if (presets.empty()) std::cout << "（暂无预设）" << std::endl;
        for (size_t i = 0; i < presets.size(); ++i) std::cout << "[" << (i + 1) << "] " << presets[i].name << std::endl;
        std::cout << "a 添加 | v 序号查看 | e 序号修改 | d 序号删除 | q 返回" << std::endl;
        const std::string command = read_trimmed("操作: ");
        if (command.empty() || lower_ascii(command) == "q") return;
        std::istringstream input(command);
        std::string action, index_text;
        input >> action >> index_text;
        action = lower_ascii(action);
        if (action == "a") {
            if (presets.size() >= 5) { std::cout << "最多保存 5 条自定义预设。" << std::endl; continue; }
            const std::string name = read_trimmed("预设名称: ");
            if (name.empty()) { std::cout << "名称不能为空。" << std::endl; continue; }
            TransformPreset preset;
            preset.name = name;
            if (!build_custom_transform_params(options, preset.params)) continue;
            presets.push_back(std::move(preset));
            save_transform_presets(presets);
        } else if (action == "v" || action == "e" || action == "d") {
            const std::optional<long long> index = try_parse_int(index_text);
            if (!index || *index < 1 || static_cast<size_t>(*index) > presets.size()) {
                std::cout << "请输入有效序号，例如 v 1。" << std::endl;
                continue;
            }
            const size_t at = static_cast<size_t>(*index - 1);
            if (action == "v") show_transform_params(presets[at]);
            else if (action == "d") {
                std::cout << "已删除预设: " << presets[at].name << std::endl;
                presets.erase(presets.begin() + static_cast<std::ptrdiff_t>(at));
                save_transform_presets(presets);
            } else {
                std::string name = read_trimmed("新名称（回车保留当前名称）: ");
                TransformPreset edited = presets[at];
                if (!name.empty()) edited.name = name;
                if (!build_custom_transform_params(options, edited.params)) continue;
                presets[at] = std::move(edited);
                save_transform_presets(presets);
            }
        } else {
            std::cout << "操作无效。" << std::endl;
        }
    }
}

std::string transform_zip_url(const std::string& file_id, const TransformParams& params) {
    std::string url = BASE_URL + "/cdn/levels/" + url_quote(file_id) + "/transform?format=zip";
    for (const auto& param : params) url += "&" + param.first + "=" + url_quote(param.second);
    return url;
}

std::string safe_level_filename(const Json& level, const std::string& suffix) {
    std::string name = "TUF_Level_" + level.get("id").key_text() + suffix;
    for (char& c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            c = '_';
        }
    }
    return name;
}

void save_download(const std::string& url, const std::string& filename, bool expect_zip) {
    if (url.empty()) {
        std::cout << "该下载选项没有可用的链接。" << std::endl;
        return;
    }
    const std::filesystem::path default_path = std::filesystem::current_path() / filename;
    std::cout << "默认保存位置: " << default_path.u8string() << std::endl;
    const std::string path_input = read_trimmed("保存路径（回车使用默认位置）: ");
    std::filesystem::path output_path =
        path_input.empty() ? default_path : std::filesystem::u8path(path_input);

    std::error_code fs_error;
    const std::filesystem::path parent = output_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, fs_error);
        if (fs_error) {
            std::cout << "无法创建保存目录: " << fs_error.message() << std::endl;
            return;
        }
    }
    const bool file_exists = std::filesystem::exists(output_path, fs_error);
    if (fs_error) {
        std::cout << "无法检查目标文件: " << fs_error.message() << std::endl;
        return;
    }
    if (file_exists) {
        const std::filesystem::path parent = output_path.parent_path();
        const std::string stem = output_path.stem().u8string();
        const std::string extension = output_path.extension().u8string();
        for (std::uint64_t suffix = 1;; ++suffix) {
            const std::filesystem::path candidate =
                parent / std::filesystem::u8path(stem + "_" + std::to_string(suffix) + extension);
            const bool candidate_exists = std::filesystem::exists(candidate, fs_error);
            if (fs_error) {
                std::cout << "无法检查目标文件: " << fs_error.message() << std::endl;
                return;
            }
            if (!candidate_exists) {
                output_path = candidate;
                break;
            }
        }
        std::cout << "文件已存在，改为保存到: " << output_path.u8string() << std::endl;
    }

    try {
        int last_percent = -1;
        std::uint64_t last_progress_bytes = 0;
        std::uint64_t sample_bytes = 0;
        double smoothed_bytes_per_second = 0.0;
        auto sample_time = std::chrono::steady_clock::now();
        auto transfer_start = sample_time;
        bool transfer_started = false;
        const HttpProgressCallback progress = [&](std::uint64_t downloaded, std::uint64_t total) {
            const auto now = std::chrono::steady_clock::now();
            if (downloaded > 0 && !transfer_started) {
                transfer_started = true;
                transfer_start = now;
                sample_time = now;
                sample_bytes = downloaded;
            } else if (transfer_started && downloaded > sample_bytes) {
                const double elapsed = std::chrono::duration<double>(now - sample_time).count();
                if (elapsed > 0.0) {
                    const double current_speed = static_cast<double>(downloaded - sample_bytes) / elapsed;
                    smoothed_bytes_per_second = smoothed_bytes_per_second == 0.0
                        ? current_speed : (smoothed_bytes_per_second * 0.65 + current_speed * 0.35);
                }
                sample_bytes = downloaded;
                sample_time = now;
            }
            const double elapsed_total = transfer_started
                ? std::chrono::duration<double>(now - transfer_start).count() : 0.0;
            const double average_speed = elapsed_total > 0.0
                ? static_cast<double>(downloaded) / elapsed_total : 0.0;
            const double speed = smoothed_bytes_per_second > 0.0 ? smoothed_bytes_per_second : average_speed;
            std::ostringstream status;
            if (total > 0) {
                const int percent = static_cast<int>((downloaded * 100) / total);
                if (percent == last_percent && downloaded < total) return;
                last_percent = percent;
                status << "下载进度: " << percent << "% (" << format_fixed(downloaded / 1048576.0, 2)
                       << " / " << format_fixed(total / 1048576.0, 2) << " MiB)";
                if (speed > 0.0) {
                    status << " | 速度: " << format_fixed(speed / 1048576.0, 2) << " MiB/s";
                    const double remaining_seconds = downloaded >= total ? 0.0 : (total - downloaded) / speed;
                    const long long eta = static_cast<long long>(std::ceil(remaining_seconds));
                    const long long hours = eta / 3600;
                    const long long minutes = (eta % 3600) / 60;
                    const long long seconds = eta % 60;
                    status << " | ETA: ";
                    if (hours > 0) status << hours << "小时" << minutes << "分" << seconds << "秒";
                    else if (minutes > 0) status << minutes << "分" << seconds << "秒";
                    else status << seconds << "秒";
                } else {
                    status << " | 速度: 计算中 | ETA: 计算中";
                }
            } else if (downloaded == 0 || downloaded - last_progress_bytes >= 512 * 1024) {
                last_progress_bytes = downloaded;
                status << "已下载: " << format_fixed(downloaded / 1048576.0, 2) << " MiB";
                if (speed > 0.0) status << " | 速度: " << format_fixed(speed / 1048576.0, 2) << " KiB/s | ETA: 未知（总大小未知）";
                else status << " | 速度: 计算中 | ETA: 未知（总大小未知）";
            } else {
                return;
            }
            std::cout << "\r" << status.str() << std::flush;
        };
        std::cout << "正在请求文件；转换下载可能需要等待服务器生成文件..." << std::endl;
        const HttpResponse response = http_get(url, active_proxy(), 60, progress);
        progress(static_cast<std::uint64_t>(response.body.size()),
                 static_cast<std::uint64_t>(response.body.size()));
        std::cout << std::endl;
        if (response.body.empty()) {
            std::cout << "下载失败：服务器返回了空文件。" << std::endl;
            return;
        }
        if (expect_zip && (response.body.size() < 4 || response.body[0] != 'P' || response.body[1] != 'K')) {
            std::cout << "下载失败：服务器返回内容不是有效的 ZIP 谱面文件。" << std::endl;
            return;
        }
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        if (!output.good()) {
            std::cout << "无法写入文件: " << output_path.u8string() << std::endl;
            return;
        }
        output.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
        output.close();
        if (!output.good()) {
            std::filesystem::remove(output_path, fs_error);
            std::cout << "写入文件失败: " << output_path.u8string() << std::endl;
            return;
        }
        std::cout << "下载完成: " << output_path.u8string() << " (" << response.body.size() << " bytes)"
                  << std::endl;
    } catch (const std::exception& e) {
        std::cout << std::endl << "下载失败: " << e.what() << std::endl;
    }
}

void download_level_chart(const Json& level) {
    const std::string level_id = level.get("id").key_text();
    const std::string file_id = level.get("fileId").as_string();
    const std::string original_url = level.get("dlLink").as_string();
    if (original_url.empty()) {
        std::cout << "该谱面没有可用的下载链接。" << std::endl;
        return;
    }

    Json cdn_data;
    try {
        cdn_data = fetchapi_sync(BASE_URL + "/v2/database/levels/" + url_quote(level_id) + "/cdnData");
    } catch (const std::exception& e) {
        std::cout << "读取转换选项失败: " << e.what() << "\n仍可下载原始 ZIP。" << std::endl;
    }
    const Json metadata = cdn_data.get("metadata");
    const Json transform_options = cdn_data.get("transformOptions");
    const bool can_transform = metadata.is_object() && transform_options.is_object() &&
                               !metadata.get("transformUnavailable").truthy();
    const bool can_download_v2 = can_transform && transform_options.get("version").as_int() >= 18;

    std::cout << "下载方式:" << std::endl;
    std::cout << "1. 原始 ZIP" << std::endl;
    if (can_transform) std::cout << "2. 转换 ZIP" << std::endl;
    if (can_download_v2) std::cout << "3. V2 .adofai" << std::endl;
    const std::string download_mode = read_trimmed("选择下载方式（回车取消）: ");
    if (download_mode.empty()) return;

    if (download_mode == "1") {
        save_download(original_url, safe_level_filename(level, ".zip"), true);
        return;
    }
    if (download_mode == "3" && can_download_v2) {
        if (file_id.empty()) {
            std::cout << "谱面缺少 CDN fileId，无法下载 V2 文件。" << std::endl;
            return;
        }
        save_download(BASE_URL + "/cdn/levels/" + url_quote(file_id) + "/level-v2.adofai",
                      safe_level_filename(level, ".adofai"), false);
        return;
    }
    if (download_mode != "2" || !can_transform) {
        std::cout << "下载方式无效或当前谱面不支持。" << std::endl;
        return;
    }
    if (file_id.empty()) {
        std::cout << "谱面缺少 CDN fileId，无法转换。" << std::endl;
        return;
    }

    std::vector<TransformPreset> stored_presets;
    load_transform_presets(stored_presets);
    while (true) {
        std::cout << "转换 ZIP 预设:" << std::endl;
        std::cout << "1. 推荐低特效（只保留 PositionTrack、RecolorTrack、ColorTrack、MoveTrack）" << std::endl;
        std::cout << "2. 推荐低特效+运镜（只保留 PositionTrack、RecolorTrack、ColorTrack、MoveCamera、RepeatEvents、MoveTrack）"
                  << std::endl;
        std::cout << "3. 删装饰（删除 AddDecoration）" << std::endl;
        std::cout << "4. 删装饰和滤镜（删除 AddDecoration 和 SetFilter）" << std::endl;
        std::cout << "5. 去全部特效（只保留 PositionTrack）" << std::endl;
        std::cout << "6. 自定义" << std::endl;
        for (size_t i = 0; i < stored_presets.size(); ++i) {
            std::cout << (i + 7) << ". 自定义预设: " << stored_presets[i].name << std::endl;
        }
        std::cout << "m. 管理自定义预设（最多 5 条）" << std::endl;
        const std::string preset = read_trimmed("选择转换方式（回车取消）: ");
        if (preset.empty()) return;
        if (lower_ascii(preset) == "m") {
            manage_transform_presets(transform_options, stored_presets);
            continue;
        }

        TransformParams params;
        if (preset == "1") {
            params.emplace_back("keepEvents", "PositionTrack,RecolorTrack,ColorTrack,MoveTrack");
        } else if (preset == "2") {
            params.emplace_back("keepEvents", "PositionTrack,RecolorTrack,ColorTrack,MoveCamera,RepeatEvents,MoveTrack");
        } else if (preset == "3") {
            params.emplace_back("dropEvents", "AddDecoration");
        } else if (preset == "4") {
            params.emplace_back("dropEvents", "AddDecoration,SetFilter");
        } else if (preset == "5") {
            params.emplace_back("keepEvents", "PositionTrack");
        } else if (preset == "6") {
            if (!build_custom_transform_params(transform_options, params)) continue;
        } else {
            const std::optional<long long> index = try_parse_int(preset);
            if (!index || *index < 7 || static_cast<size_t>(*index - 7) >= stored_presets.size()) {
                std::cout << "无效选择。" << std::endl;
                continue;
            }
            params = stored_presets[static_cast<size_t>(*index - 7)].params;
        }
        save_download(transform_zip_url(file_id, params), safe_level_filename(level, "_converted.zip"), true);
        return;
    }
}

void print_level_details(const Json& level) {
    const Json& difficulty = level.get("difficulty");
    std::cout << "\n谱面详情" << std::endl;
    std::cout << "ID: " << level.get("id").key_text() << std::endl;
    std::cout << "歌曲: " << level.get("song").as_string() << std::endl;
    std::cout << "艺术家: " << level.get("artist").as_string() << std::endl;
    if (!level.get("suffix").is_null()) std::cout << "后缀: " << level.get("suffix").as_string() << std::endl;
    std::cout << "难度: " << difficulty.get("name").as_string() << std::endl;
    std::cout << "谱师/制作团队: " << level.get("creator").as_string() << std::endl;
    std::cout << "BaseScore: " << level.get("baseScore").key_text()
              << " | PP BaseScore: " << level.get("ppBaseScore").key_text()
              << " | 难度 BaseScore: " << difficulty.get("baseScore").key_text() << std::endl;
    std::cout << "BPM: " << level.get("bpm").key_text() << " | 砖块数: " << level.get("tilecount").key_text()
              << " | 时长(ms): " << level.get("levelLengthInMs").key_text() << std::endl;
    std::cout << "通关数: " << level.get("clears").key_text()
              << " | 独立通关玩家数: " << level.get("uniqueClears").key_text()
              << " | 下载数: " << level.get("downloadCount").key_text()
              << " | 收藏数: " << level.get("likes").key_text() << std::endl;
    if (!level.get("videoLink").as_string().empty()) {
        std::cout << "视频: " << level.get("videoLink").as_string() << std::endl;
    }
    if (!level.get("workshopLink").as_string().empty()) {
        std::cout << "创意工坊: " << level.get("workshopLink").as_string() << std::endl;
    }
    const std::vector<Json> tags = level.get("tags").elements();
    if (!tags.empty()) {
        std::cout << "标签: ";
        for (size_t i = 0; i < tags.size(); ++i) {
            if (i != 0) std::cout << ", ";
            std::cout << tags[i].get("name").as_string();
        }
        std::cout << std::endl;
    }
    const Json description_value = level.get("description");
    if (description_value.is_string() && !description_value.as_string().empty()) {
        std::cout << "简介: " << description_value.as_string() << std::endl;
    }
    const std::string notes = level.get("notes").as_string();
    if (!notes.empty()) std::cout << "备注: " << notes << std::endl;
}

}  // namespace

// Shared with jsoncli.cpp (rank.preview): the top-20 weighted ranked score keeps
// the best pass per level, so a repeated candidate cannot occupy two slots.
constexpr size_t kRankedScoreLimit = 20;
constexpr double kRankedScoreWeight = 0.9;

std::optional<double> ranked_score_after_candidate(const Json& player, double candidate_score,
                                                   const std::string& candidate_level_id, double& added_score,
                                                   std::string& error) {
    const PassList passes = fetch_all_player_passes(player.get("name").as_string(), player.get("id"));
    const double current_ranked_score = player.get("rankedScore").number_or(0.0);
    if (passes.rows.empty() && player.get("totalPasses").number_or(0.0) > 0.0) {
        error = "没有获取到玩家通关记录";
        return std::nullopt;
    }

    // Ranked score keeps the best pass for each level before applying the top-20
    // 0.9 decay. Repeated clears therefore cannot occupy multiple slots.
    std::map<std::string, double> best_by_level;
    for (const Json& pass : passes.rows) {
        if (pass.get("isHidden").truthy() || pass.get("isDuplicate").truthy()) continue;
        const Json score = pass.get("scoreV2");
        if (!score.is_number()) continue;

        Json level_id = pass.get("levelId");
        if (level_id.is_null()) level_id = pass.get("level").get("id");
        if (level_id.is_null()) level_id = pass.get("id");
        if (level_id.is_null()) continue;

        const std::string key = level_id.key_text();
        const double value = score.as_double();
        auto it = best_by_level.find(key);
        if (it == best_by_level.end() || value > it->second) best_by_level[key] = value;
    }

    std::vector<double> scores;
    scores.reserve(best_by_level.size() + 1);
    for (const auto& entry : best_by_level) scores.push_back(entry.second);

    if (!candidate_level_id.empty()) {
        auto it = best_by_level.find(candidate_level_id);
        if (it != best_by_level.end()) {
            if (candidate_score > it->second) {
                scores.erase(std::find(scores.begin(), scores.end(), it->second));
                scores.push_back(candidate_score);
            }
        } else {
            scores.push_back(candidate_score);
        }
    } else {
        scores.push_back(candidate_score);
    }

    std::sort(scores.begin(), scores.end(), std::greater<double>());
    auto weighted_top_20 = [](const std::vector<double>& values) {
        double total = 0.0;
        const size_t count = std::min(values.size(), kRankedScoreLimit);
        for (size_t i = 0; i < count; ++i) {
            total += values[i] * std::pow(kRankedScoreWeight, static_cast<double>(i));
        }
        return total;
    };

    std::vector<double> old_scores;
    old_scores.reserve(best_by_level.size());
    for (const auto& entry : best_by_level) old_scores.push_back(entry.second);
    std::sort(old_scores.begin(), old_scores.end(), std::greater<double>());
    const double old_weighted_score = weighted_top_20(old_scores);
    if (std::abs(current_ranked_score - old_weighted_score) > 0.05) {
        error = "通关记录重算值与玩家当前 rankedScore 不一致";
        return std::nullopt;
    }

    const double new_weighted_score = weighted_top_20(scores);
    added_score = new_weighted_score - old_weighted_score;
    return current_ranked_score + added_score;
}

// ============================================================== ppcalc.py ====

void calculate_rank_changes(double calculated_score, const std::string& selected_level_id) {
    std::cout << "选择查找玩家的方式:" << std::endl;
    std::cout << "1. 按玩家名称" << std::endl;
    std::cout << "2. 按玩家唯一 ID" << std::endl;
    const std::string p_mode = read_trimmed("> ");

    Json player = Json::object();
    if (p_mode == "1") {
        const std::string name = read_trimmed("玩家名:");
        const Json results = player_search(name);
        if (results.size() == 0) {
            std::cout << "未找到玩家" << std::endl;
            return;
        }
        player = choose_player(results);
    } else if (p_mode == "2") {
        const std::string pid = read_trimmed("ID:");
        try {
            player = get_player(pid);
        } catch (const std::exception&) {
            std::cout << "未找到玩家" << std::endl;
            return;
        }
    } else {
        std::cout << "无效选择" << std::endl;
        return;
    }

    std::string candidate_level_id = selected_level_id;
    if (candidate_level_id.empty() && lower_ascii(read_trimmed("本次成绩是否对应已有谱面？(y/N): ")) == "y") {
        candidate_level_id = read_trimmed("关卡 ID: ");
        if (candidate_level_id.empty()) {
            std::cout << "已有谱面需要填写关卡 ID，已取消排名估算。" << std::endl;
            return;
        }
    }

    if (player.is_object() && !player.items().empty() &&
        (!player.has("rankedScoreRank") || !player.has("rankedScore"))) {
        try {
            player = get_player(player.get("id").key_text());
        } catch (const std::exception&) {
            std::cout << "无法获取玩家数据" << std::endl;
            return;
        }
    }

    const double old_score = player.get("rankedScore").number_or(0.0);
    double added_score = 0.0;
    std::string score_error;
    const std::optional<double> new_score_value =
        ranked_score_after_candidate(player, calculated_score, candidate_level_id, added_score, score_error);
    if (!new_score_value.has_value()) {
        std::cout << "无法计算加权排位分: " << score_error << std::endl;
        return;
    }
    const double new_score = *new_score_value;

    const Json old_global_json = player.get("rankedScoreRank");

    Json future_player = Json::object();
    future_player.set("country", player.get("country"));
    future_player.set("rankedScore", new_score);

    std::map<std::string, std::string> jobs;
    jobs["old_country"] = rank_url(player, "rankedScore", "country");
    jobs["new_global"] = rank_url(future_player, "rankedScore", "global");
    jobs["new_country"] = rank_url(future_player, "rankedScore", "country");
    if (old_global_json.is_null()) jobs["old_global"] = rank_url(player, "rankedScore", "global");

    std::map<std::string, std::string> filtered;
    for (const auto& job : jobs) {
        if (!job.second.empty()) filtered[job.first] = job.second;
    }
    const std::map<std::string, RankVal> ranks = fetch_ranks(filtered);

    auto lookup = [&ranks](const std::string& key) {
        auto it = ranks.find(key);
        return it == ranks.end() ? RankVal::unknown() : it->second;
    };

    RankVal old_global_rank = old_global_json.is_null() ? lookup("old_global") : RankVal::from_json(old_global_json);
    RankVal old_country_rank = lookup("old_country");
    RankVal new_global_rank = lookup("new_global");
    RankVal new_country_rank = lookup("new_country");

    old_global_rank = as_one_if_zero(old_global_rank);
    old_country_rank = as_one_if_zero(old_country_rank);
    new_global_rank = as_one_if_zero(new_global_rank);
    new_country_rank = as_one_if_zero(new_country_rank);

    const std::string global_delta = rank_delta(old_global_rank, new_global_rank);
    const std::string country_delta = rank_delta(old_country_rank, new_country_rank);

    std::cout << "玩家: " << player.get("name").as_string() << " (ID: " << player.get("id").key_text() << ")"
              << std::endl;
    std::cout << "加权排位分: " << format_fixed(old_score, 2) << " -> " << format_fixed(new_score, 2) << " ("
              << (added_score >= 0.0 ? "+" : "") << format_fixed(added_score, 2)
              << ")" << std::endl;
    std::cout << "全球排名: " << old_global_rank.str() << " -> " << new_global_rank.str() << " (" << global_delta << ")"
              << std::endl;
    std::cout << "国家排名: " << old_country_rank.str() << " -> " << new_country_rank.str() << " (" << country_delta
              << ")" << std::endl;
}

void handle_pp_calc() {
    TUFScoreCalculator calculator;
    DifficultyManager diffman;

    std::cout << "选择BaseScore来源:" << std::endl;
    std::cout << "1. 按难度等级" << std::endl;
    std::cout << "2. 按谱面搜索" << std::endl;
    std::cout << "3. 直接输入BaseScore" << std::endl;
    const std::string base_mode = read_trimmed("> ");

    std::string difficulty;
    std::optional<double> base_score;
    Json selected_level;
    if (base_mode == "1") {
        if (DifficultyManager::file_exists()) {
            const std::string update = lower_ascii(read_trimmed("是否更新难度数据库? (y/N): "));
            if (update == "y") {
                try {
                    diffman.update();
                    calculator.diffman().load();
                    std::cout << "难度数据库更新完成。" << std::endl;
                } catch (const std::exception& e) {
                    std::cout << "更新失败: " << e.what() << std::endl;
                }
            }
        } else {
            std::cout << "下载难度数据库..." << std::endl;
            try {
                diffman.update();
                calculator.diffman().load();
                std::cout << "下载完成" << std::endl;
            } catch (const std::exception& e) {
                std::cout << "下载失败: " << e.what() << std::endl;
                return;
            }
        }
        difficulty = upper_ascii(read_trimmed("难度: "));
    } else if (base_mode == "2") {
        selected_level = search_level();
        if (selected_level.is_null()) return;
    } else if (base_mode == "3") {
        const std::optional<double> parsed = try_parse_double(read_trimmed("BaseScore: "));
        if (!parsed.has_value()) {
            std::cout << "could not convert string to float" << std::endl;
            return;
        }
        base_score = parsed;
    } else {
        std::cout << "无效选择" << std::endl;
        return;
    }

    const bool marathon = lower_ascii(read_trimmed("是否Marathon(y/N): ")) == "y";

    Json level_data;
    if (base_mode == "2") {
        if (marathon) {
            Json api_difficulty = selected_level.get("difficulty");
            api_difficulty.set("name", "Marathon");
            selected_level.set("difficulty", api_difficulty);
        }
        const Json api_tilecount = selected_level.get("tilecount");
        if (!api_tilecount.is_number() || api_tilecount.as_int() <= 0) {
            std::cout << "API 未返回有效砖块数。" << std::endl;
            return;
        }
        level_data = Json::object();
        level_data.set("level", selected_level);
        std::cout << "已选择谱面 ID=" << selected_level.get("id").key_text() << " | "
                  << selected_level.get("song").as_string() << " - " << selected_level.get("artist").as_string()
                  << " | tiles=" << api_tilecount.key_text() << std::endl;
    } else {
        const std::string tilecount_input = read_trimmed("关卡砖块数: ");
        const std::optional<long long> tilecount_parsed = try_parse_int(tilecount_input);
        if (!tilecount_parsed.has_value()) {
            std::cout << "砖块数必须是整数" << std::endl;
            return;
        }
        const long long tilecount = *tilecount_parsed;
        if (tilecount <= 0) {
            std::cout << "砖块数必须大于 0" << std::endl;
            return;
        }
        try {
            level_data = calculator.build_level_data(difficulty, marathon, tilecount, base_score);
        } catch (const std::exception& e) {
            std::cout << e.what() << std::endl;
            return;
        }
    }

    const std::optional<double> accuracy_parsed =
        try_parse_double(read_trimmed("XACC（百分比；建议输入精确值，最多显示4位小数）: "));
    if (!accuracy_parsed.has_value()) {
        std::cout << "could not convert string to float" << std::endl;
        return;
    }
    const double accuracy = *accuracy_parsed;

    const std::string misses_input = read_trimmed("空敲数 (默认0): ");
    long long misses = 0;
    if (!misses_input.empty()) {
        const std::optional<long long> parsed = try_parse_int(misses_input);
        if (!parsed.has_value()) {
            std::cout << "invalid literal for int() with base 10: '" << misses_input << "'" << std::endl;
            return;
        }
        misses = *parsed;
    }

    const std::string speed_input = read_trimmed("速度倍率 (默认1.0): ");
    double speed = 1.0;
    if (!speed_input.empty()) {
        const std::optional<double> parsed = try_parse_double(speed_input);
        if (!parsed.has_value()) {
            std::cout << "could not convert string to float: '" << speed_input << "'" << std::endl;
            return;
        }
        speed = *parsed;
    }

    const bool no_hold = lower_ascii(read_trimmed("是否禁用Hold+Tap? (y/N): ")) == "y";

    const Json result = calculator.calculate_score(level_data, accuracy, misses, speed, no_hold);

    std::cout << std::endl;
    std::cout << "XACC: " << float_of(result.get("accuracy_pct")) << "%" << std::endl;
    std::cout << "基础分: " << float_of(result.get("base_score")) << std::endl;
    std::cout << "分数倍率: " << float_of(result.get("multiplier")) << "x" << std::endl;
    std::cout << "速度修正: " << float_of(result.get("speed_mod")) << "x" << std::endl;
    std::cout << "空敲修正: " << float_of(result.get("empty_tap_mod")) << "x" << std::endl;
    if (verbose_enabled()) {
        if (result.get("xacc_curve_custom").as_bool()) {
            std::cout << "[verbose] XACC曲线: 谱面自定义 (cutoff="
                      << float_of(result.get("xacc_curve_cutoff").as_double() * 100.0) << "%, topMultiplier="
                      << float_of(result.get("xacc_curve_top_multiplier")) << ", poleOffset="
                      << float_of(result.get("xacc_curve_pole_offset")) << ")" << std::endl;
        } else {
            std::cout << "[verbose] XACC曲线: 标准默认曲线" << std::endl;
        }
    }

    std::cout << std::endl;
    std::cout << "PP分: " << float_of(result.get("score")) << std::endl;

    const std::string calc_rank = lower_ascii(read_trimmed("\n是否计算排名变化? (y/N): "));
    if (calc_rank == "y") {
        const std::string selected_level_id = base_mode == "2" ? selected_level.get("id").key_text() : "";
        calculate_rank_changes(result.get("score").as_double(), selected_level_id);
    }

    std::cout << std::endl;
    stats();
}

void handle_level_lookup() {
    const Json level = search_level(true);
    if (level.is_null()) return;

    print_level_details(level);
    if (lower_ascii(read_trimmed("\n下载该谱面？(y/N): ")) == "y") {
        download_level_chart(level);
    }
}

// ============================================================ acccalc.py ====

namespace {

const std::vector<std::string>& xacc_keys(bool xperfect) { return jd_keys(xperfect); }

// XPerfect splits `perfect` into +perfect / -perfect / xperfect, so the judgement
// set (and therefore the number of values the user types) changes with the mode.
bool ask_xperfect_mode() { return lower_ascii(read_trimmed("是否启用 XPerfect? (y/N): ")) == "y"; }

// Session-wide solver choice for the reverse search; `automatic` picks the exact
// dense DP for small inputs and branch-and-bound for large ones.
XaccSolver g_acc_solver = XaccSolver::automatic;

// Loads the configuration once per operation.  Returns false and prints the reason
// when the model cannot be built.
bool acc_load_model(XaccModel& model) {
    std::string error;
    std::string note;
    if (!xacc_model_load(model, error, &note)) {
        std::cout << "读取配置失败: " << error << std::endl;
        return false;
    }
    if (!note.empty()) std::cout << note << std::endl;
    return true;
}

// Prompt loop for the difficulty coefficients.  They live in settings.json as exact
// integer units, so the edit round-trips exactly.  Judgement weights are NOT part
// of this: they are the fixed game rule that defines XACC.
bool acc_edit_units(XaccModel& model) {
    bool modified = false;

    for (const std::string& key : model.order) {
        const std::string current = xacc_format_cost(model, key);
        const std::string value = read_trimmed(key + " [" + current + "]: ");
        if (value.empty()) continue;

        const std::optional<double> parsed = try_parse_double(value);
        if (!parsed.has_value()) {
            std::cout << "输入无效，" << key << " 保持原值" << std::endl;
            continue;
        }
        std::int64_t units = 0;
        bool exact = false;
        if (!xacc_units_from_value(*parsed, model.cost_scale, units, exact) || !exact) {
            std::cout << "输入无效，无法用 1/" << model.cost_scale << " 的精度表示: " << value << std::endl;
            continue;
        }
        if (units < 0) {
            std::cout << "难度系数不能为负数" << std::endl;
            continue;
        }
        model.cost_units[key] = units;
        modified = true;
    }
    return modified;
}

void acc_choose_solver() {
    const std::vector<XaccSolver>& available = xacc_solver_all();
    std::cout << std::endl << "当前求解器: " << xacc_solver_text(g_acc_solver) << std::endl;
    for (size_t i = 0; i < available.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << xacc_solver_text(available[i]) << std::endl;
    }

    const std::string value = read_trimmed("选择求解器 (回车保持不变): ");
    if (value.empty()) return;
    XaccSolver chosen = g_acc_solver;
    if (!xacc_solver_from_text(value, chosen)) {
        std::cout << "无效选择" << std::endl;
        return;
    }
    g_acc_solver = chosen;
    std::cout << "已设为 " << xacc_solver_text(g_acc_solver) << std::endl;
}

void acc_run1() {
    const bool xperfect = ask_xperfect_mode();
    const std::vector<std::string>& keys = xacc_keys(xperfect);

    XaccModel model;
    if (!acc_load_model(model)) return;

    std::cout << "输入格式: "
              << (xperfect ? "failMiss tooEarly early EPerfect +perfect xperfect -perfect LPerfect late"
                           : "failMiss tooEarly early EPerfect perfect LPerfect late")
              << std::endl;

    const std::string raw = read_trimmed("判定数据: ");

    std::vector<std::string> values;
    std::istringstream stream(raw);
    std::string token;
    while (stream >> token) values.push_back(token);

    if (values.size() != keys.size()) {
        std::cout << "需要输入" << keys.size() << "个数字" << std::endl;
        return;
    }

    std::vector<long long> judgements;
    for (const std::string& value : values) {
        const std::optional<long long> parsed = try_parse_int(value);
        if (!parsed.has_value()) {
            std::cout << "请输入整数" << std::endl;
            return;
        }
        if (*parsed < 0) {
            std::cout << "判定数量不能为负数" << std::endl;
            return;
        }
        judgements.push_back(*parsed);
    }

    // Sum in integer score units, so the printed value is exact rather than the
    // accumulated float noise of summing decimal weights.
    XaccCount total = 0;
    XaccScoreUnit weighted = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        total += judgements[i];
        std::int64_t term = 0;
        if (!mul_checked(model.weight(keys[i]), judgements[i], term) || !add_checked(weighted, term, weighted)) {
            std::cout << "数值超出可计算范围" << std::endl;
            return;
        }
    }
    if (total == 0) {
        std::cout << "物量为 0，无法计算 XACC" << std::endl;
        return;
    }

    std::cout << std::endl;
    std::cout << "XACC: " << py_float_str(xacc_acc_percent(weighted, total, model)) << "%" << std::endl;
}

// Collects the total, the target and the pinned judgement counts shared by every
// reverse-query entry point.  Returns false when the user's input was rejected.
bool ask_reverse_query(bool xperfect, const std::vector<std::string>& keys, long long& total, XaccTarget& target,
                       std::map<std::string, long long>& fixed_counts) {
    const std::optional<long long> total_parsed = try_parse_int(read_trimmed("物量: "));
    if (!total_parsed.has_value()) {
        std::cout << "invalid literal for int() with base 10" << std::endl;
        return false;
    }
    total = *total_parsed;
    if (total <= 0) {
        std::cout << "物量必须大于 0" << std::endl;
        return false;
    }

    // The target is parsed as an exact decimal (never through a double), so the
    // search window is exactly the set of accuracies that round to what was typed.
    const std::string target_text = read_trimmed("XACC: ");
    std::string error;
    if (!xacc_parse_target(target_text, target, error)) {
        std::cout << error << std::endl;
        return false;
    }

    for (const std::string& key : keys) {
        const std::string value = read_trimmed("固定 " + key + " 的数量 (回车表示不固定): ");
        if (value.empty()) continue;
        const std::optional<long long> parsed = try_parse_int(value);
        if (!parsed.has_value()) {
            std::cout << "已跳过固定 " << key << std::endl;
            continue;
        }
        if (*parsed < 0) {
            std::cout << "已跳过固定 " << key << " (数量不能为负数)" << std::endl;
            continue;
        }
        fixed_counts[key] = *parsed;
    }
    (void)xperfect;
    return true;
}

void acc_run2() {
    const bool xperfect = ask_xperfect_mode();
    const std::vector<std::string>& keys = xacc_keys(xperfect);

    long long total = 0;
    XaccTarget target;
    std::map<std::string, long long> fixed_counts;
    if (!ask_reverse_query(xperfect, keys, total, target, fixed_counts)) return;

    XaccModel model;
    if (!acc_load_model(model)) return;

    std::cout << "\n正在计算...这可能需要一些时间" << std::endl;
    std::cout << "求解器: " << xacc_solver_text(g_acc_solver) << std::endl;

    XaccReverseOptions options;
    options.solver = g_acc_solver;
    options.max_nodes = model.max_nodes;
    options.max_dp_cells = model.max_dp_cells;
    options.max_seconds = model.max_seconds;
    const XaccReverseResult result = xacc_reverse_search(model, target, total, fixed_counts, xperfect, options);

    std::cout << std::endl;
    if (!result.ok()) {
        std::cout << "无法达成该目标 XACC: " << result.message << std::endl;
    } else {
        for (const std::string& key : keys) {
            const std::string status = fixed_counts.count(key) ? "(Locked)" : "";
            auto it = result.counts.find(key);
            const long long value = it == result.counts.end() ? 0 : it->second;
            std::cout << " " << pad_right(key, 10) << ": " << value << " " << status << std::endl;
        }

        constexpr int kAccDisplayDecimals = 6;
        const double achieved_acc = xacc_acc_percent(result.score_units, total, model);
        std::string achieved_text = trim_fixed(achieved_acc, kAccDisplayDecimals);
        if (decimal_places_of(achieved_text) < target.decimals) {
            achieved_text = format_fixed(achieved_acc, target.decimals);
        }

        std::cout << std::endl << " XACC: " << achieved_text << "%";
        if (!result.exact) {
            std::cout << "  (该物量下无法精确到小数点后 " << target.decimals << " 位，以上为最接近的可达值)";
        }
        std::cout << std::endl;
        std::cout << " 求解器 " << xacc_solver_text(result.solver) << "，与目标相差 " << result.distance_units
                  << " 个分数单位，难度系数合计 " << result.cost_units << std::endl;
        std::cout << " 节点 " << result.nodes << "，剪枝 " << result.pruned << "，状态 " << result.states << std::endl;
        if (!result.message.empty()) std::cout << " " << result.message << std::endl;
        if (!result.note.empty()) std::cout << " " << result.note << std::endl;
    }
    std::cout << " Elapsed " << py_float_str(result.elapsed_ms) << " ms" << std::endl;
}

// Lists the combinations that are interchangeable with the reported optimum.
void acc_run_equivalents() {
    const bool xperfect = ask_xperfect_mode();
    const std::vector<std::string>& keys = xacc_keys(xperfect);

    long long total = 0;
    XaccTarget target;
    std::map<std::string, long long> fixed_counts;
    if (!ask_reverse_query(xperfect, keys, total, target, fixed_counts)) return;

    XaccModel model;
    if (!acc_load_model(model)) return;

    XaccReverseOptions options;
    options.solver = g_acc_solver;
    options.max_nodes = model.max_nodes;
    options.max_dp_cells = model.max_dp_cells;
    options.max_seconds = model.max_seconds;

    std::cout << "\n正在枚举...这可能需要一些时间" << std::endl;
    const XaccEquivalentSet set = xacc_equivalents(model, target, total, fixed_counts, xperfect, 64, options);
    std::cout << std::endl;
    if (!set.ok()) {
        std::cout << "无法达成该目标 XACC: " << set.message << std::endl;
        return;
    }

    const double acc = xacc_acc_percent(set.score_units, total, model);
    std::cout << " 共找到 " << set.solutions.size() << " 个等效组合"
              << "，均达成 XACC: " << trim_fixed(acc, 6) << "%"
              << "，与目标相差 " << set.distance_units << " 个分数单位"
              << "，难度系数合计 " << set.cost_units << std::endl;
    if (set.truncated) {
        std::cout << " (仅显示前 " << set.solutions.size() << " 个)" << std::endl;
    }
    std::cout << std::endl;

    long long index = 1;
    for (const std::map<std::string, long long>& solution : set.solutions) {
        std::cout << " [" << index << "]";
        for (const std::string& key : keys) {
            auto it = solution.find(key);
            const long long value = it == solution.end() ? 0 : it->second;
            std::cout << " " << key << ":" << value;
        }
        std::cout << std::endl;
        ++index;
    }
}

void acc_run3() {
    XaccModel model;
    if (!acc_load_model(model)) return;

    std::cout << "当前各判定难度系数 (数值越低，算法越倾向于用它凑分):" << std::endl;
    for (const std::string& key : model.order) {
        std::cout << "  " << pad_right(key, 10) << ": " << xacc_format_cost(model, key) << std::endl;
    }

    std::cout << std::endl << "请输入新系数 (回车保持不变):" << std::endl;
    if (!acc_edit_units(model)) {
        std::cout << "Not modified" << std::endl;
        return;
    }

    std::string error;
    if (xacc_model_save(model, error)) {
        std::cout << "保存成功" << std::endl;
    } else {
        std::cout << "保存失败: " << error << std::endl;
    }
}


void acc_edit_solver_params() {
    XaccModel model;
    if (!acc_load_model(model)) return;

    std::cout << "如果你不知道这些参数的含义，请不要修改\n" << std::endl;
    std::cout << "求解器参数:" << std::endl;
    std::cout << " " << pad_right("max_dp_cells", 12) << ": " << model.max_dp_cells << std::endl;
    std::cout << " " << pad_right("max_nodes", 12) << ": " << model.max_nodes << std::endl;
    std::cout << " " << pad_right("max_seconds", 12) << ": " << py_float_str(model.max_seconds) << std::endl;
    std::cout << " " << pad_right("cost_scale", 12) << ": " << model.cost_scale << std::endl;
    std::cout << std::endl;

    bool modified = false;

    const std::string dp_cells = read_trimmed("max_dp_cells [" + std::to_string(model.max_dp_cells) + "]: ");
    if (!dp_cells.empty()) {
        const std::optional<long long> parsed = try_parse_int(dp_cells);
        if (!parsed.has_value() || *parsed <= 0) {
            std::cout << "输入无效，max_dp_cells 保持原值" << std::endl;
        } else {
            model.max_dp_cells = *parsed;
            modified = true;
        }
    }

    const std::string nodes = read_trimmed("max_nodes [" + std::to_string(model.max_nodes) + "]: ");
    if (!nodes.empty()) {
        const std::optional<long long> parsed = try_parse_int(nodes);
        if (!parsed.has_value() || *parsed <= 0) {
            std::cout << "输入无效，max_nodes 保持原值" << std::endl;
        } else {
            model.max_nodes = *parsed;
            modified = true;
        }
    }

    const std::string seconds = read_trimmed("max_seconds [" + py_float_str(model.max_seconds) + "]: ");
    if (!seconds.empty()) {
        const std::optional<double> parsed = try_parse_double(seconds);
        if (!parsed.has_value() || !(*parsed > 0.0) || *parsed > 86400.0) {
            std::cout << "输入无效，max_seconds 保持原值" << std::endl;
        } else {
            model.max_seconds = *parsed;
            modified = true;
        }
    }

    const std::string scale = read_trimmed("cost_scale [" + std::to_string(model.cost_scale) + "]: ");
    if (!scale.empty()) {
        const std::optional<long long> parsed = try_parse_int(scale);
        if (!parsed.has_value() || *parsed <= 0) {
            std::cout << "输入无效，cost_scale 保持原值" << std::endl;
        } else if (*parsed != model.cost_scale) {
            const long double factor = static_cast<long double>(*parsed) / static_cast<long double>(model.cost_scale);
            bool rounded = false;
            for (std::pair<const std::string, XaccCostUnit>& entry : model.cost_units) {
                const long double scaled = static_cast<long double>(entry.second) * factor;
                if (scaled > 9.0e15L) {
                    entry.second = 9000000000000000LL;
                    rounded = true;
                    continue;
                }
                const long long value = std::llround(scaled);
                if (std::fabs(scaled - static_cast<long double>(value)) > 1.0e-6L) rounded = true;
                entry.second = value;
            }
            model.cost_scale = *parsed;
            std::cout << "已按 1/" << model.cost_scale << " 换算各判定难度系数"
                      << (rounded ? "（部分系数按新精度取整）" : "") << std::endl;
            modified = true;
        }
    }

    if (!modified) {
        std::cout << "Not modified" << std::endl;
        return;
    }

    std::string error;
    if (xacc_model_save(model, error)) {
        std::cout << "保存成功" << std::endl;
    } else {
        std::cout << "保存失败: " << error << std::endl;
    }
}

}  // namespace

void handle_acc_calc() {
    std::cout << std::endl;
    while (true) {
        std::cout << std::endl;
        std::cout << "1. 根据判定计算 XACC" << std::endl;
        std::cout << "2. 根据 XACC 推算判定" << std::endl;
        std::cout << "3. 列出等效判定组合" << std::endl;
        std::cout << "4. 自定义难度系数" << std::endl;
        std::cout << "5. 选择求解器 (当前: " << xacc_solver_text(g_acc_solver) << ")" << std::endl;
        std::cout << "6. 编辑求解器参数" << std::endl;
        std::cout << "b. 返回主菜单" << std::endl;

        const std::string choice = read_trimmed("> ");

        if (choice == "1") {
            acc_run1();
        } else if (choice == "2") {
            acc_run2();
        } else if (choice == "3") {
            acc_run_equivalents();
        } else if (choice == "4") {
            acc_run3();
        } else if (choice == "5") {
            acc_choose_solver();
        } else if (choice == "6") {
            acc_edit_solver_params();
        } else if (choice == "b") {
            break;
        } else {
            std::cout << "无效选择" << std::endl;
        }
    }
}

}  // namespace tuf
