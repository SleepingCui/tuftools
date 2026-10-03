#include "tools.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "api.hpp"
#include "apppaths.hpp"
#include "console.hpp"
#include "numfmt.hpp"
#include "xacc_model.hpp"
#include "xacc_solver.hpp"

namespace tuf {

namespace {
constexpr const char* COSTS_FILE = "costs.json";
constexpr const char* DIFFICULTIES_FILE = "difficulties.json";
}  // namespace

// ============================================================== CostsMan ====

const std::vector<std::string>& jd_cost_keys() {
    static const std::vector<std::string> keys = {"perfect",  "failMiss", "tooEarly", "early",    "late",
                                                  "ePerfect", "lPerfect", "+perfect", "xperfect", "-perfect"};
    return keys;
}

const std::map<std::string, double>& default_jd_costs() {
    static const std::map<std::string, double> costs = {
        {"perfect", 0.0},   {"failMiss", 1.0}, {"tooEarly", 1.5},  {"early", 50.0},
        {"late", 50.0},     {"ePerfect", 50.0}, {"lPerfect", 50.0},
        // XPerfect sub-judgements: equal weight to perfect, but not equally easy to hit.
        {"+perfect", 10.0}, {"xperfect", 0.0}, {"-perfect", 10.0},
    };
    return costs;
}

std::map<std::string, double> costs_load() {
    const std::map<std::string, double>& defaults = default_jd_costs();
    std::ifstream file(data_file(COSTS_FILE), std::ios::binary);
    if (!file.good()) {
        costs_save(defaults);
        return defaults;
    }
    try {
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const Json parsed = Json::parse(buffer.str());
        std::map<std::string, double> costs;
        for (const std::string& key : jd_cost_keys()) {
            costs[key] = parsed.get(key).is_number() ? parsed.get(key).as_double() : defaults.at(key);
        }
        return costs;
    } catch (const std::exception& e) {
        std::cout << "读取配置文件失败!使用默认值: " << e.what() << std::endl;
        return defaults;
    }
}

void costs_save(const std::map<std::string, double>& costs) {
    try {
        Json root = Json::object();
        for (const std::string& key : jd_cost_keys()) {
            auto it = costs.find(key);
            root.set(key, it == costs.end() ? default_jd_costs().at(key) : it->second);
        }
        std::ofstream file(data_file(COSTS_FILE), std::ios::binary | std::ios::trunc);
        file << root.dump(4);
    } catch (const std::exception& e) {
        std::cout << "保存配置文件失败: " << e.what() << std::endl;
    }
}

// ============================================================== DiffMan =====

DifficultyManager::DifficultyManager() { load(); }

bool DifficultyManager::file_exists() {
    std::ifstream file(data_file(DIFFICULTIES_FILE), std::ios::binary);
    return file.good();
}

std::vector<Json> DifficultyManager::load() {
    if (!file_exists()) {
        std::cout << "下载难度数据..." << std::endl;
        return update();
    }
    std::ifstream file(data_file(DIFFICULTIES_FILE), std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const Json parsed = Json::parse(buffer.str());
    difficulties_.clear();
    if (parsed.is_array()) {
        for (const Json& item : parsed.elements()) difficulties_.push_back(item);
    }
    return difficulties_;
}

std::vector<Json> DifficultyManager::update() {
    const Json raw = fetchapi_sync(BASE_URL + "/v2/database/difficulties");

    std::vector<Json> result;
    if (raw.is_array()) {
        for (const Json& item : raw.elements()) {
            Json entry = Json::object();
            entry.set("name", item.get("name"));
            entry.set("baseScore", item.get("baseScore"));
            result.push_back(entry);
        }
    }

    std::ofstream file(data_file(DIFFICULTIES_FILE), std::ios::binary | std::ios::trunc);
    file << Json::array(result).dump(2);

    difficulties_ = result;
    downloaded_ = true;
    return result;
}

double DifficultyManager::get_base_score(const std::string& difficulty_name) const {
    const std::string wanted = upper_ascii(difficulty_name);
    for (const Json& item : difficulties_) {
        if (upper_ascii(item.get("name").as_string()) == wanted) {
            return item.get("baseScore").as_double();
        }
    }
    throw std::runtime_error("未知难度: " + wanted);
}

// ==================================================== TUFScoreCalculator =---

const std::vector<std::string>& score_judgement_keys() {
    static const std::vector<std::string> keys = {"miss", "early", "ePerfect", "perfect", "lPerfect", "late"};
    return keys;
}

const std::map<std::string, double>& score_judgement_weights() {
    static const std::map<std::string, double> weights = {
        {"miss", 0.2}, {"early", 0.4}, {"ePerfect", 0.75}, {"perfect", 1.0}, {"lPerfect", 0.75}, {"late", 0.4},
    };
    return weights;
}

TUFScoreCalculator::TUFScoreCalculator()
    : diffman_(), difficulties_(diffman_.load()) {}

Json TUFScoreCalculator::build_level_data(const std::string& difficulty_name, bool marathon, long long tilecount,
                                          std::optional<double> base_score) {
    const std::string name = upper_ascii(difficulty_name);

    double diff_base = 0.0;
    double level_base = 0.0;
    if (!base_score.has_value()) {
        diff_base = diffman_.get_base_score(name);
        level_base = 0.0;
    } else {
        diff_base = *base_score;
        level_base = *base_score;
    }

    Json difficulty = Json::object();
    difficulty.set("name", marathon ? std::string("Marathon") : (name.empty() ? std::string("Custom") : name));
    difficulty.set("baseScore", diff_base);

    Json level = Json::object();
    level.set("song", "");
    level.set("artist", "");
    level.set("tilecount", tilecount);
    level.set("ppBaseScore", 0);
    level.set("baseScore", level_base);
    level.set("difficulty", difficulty);
    level.set("xaccCurve", Json());
    level.set("xaccCurveMeta", Json());

    Json root = Json::object();
    root.set("level", level);
    return root;
}

std::pair<double, std::string> TUFScoreCalculator::get_base_score(const Json& level_data, double accuracy) const {
    const Json& level = level_data.has("level") ? level_data.get("level") : level_data;
    const Json& difficulty = level.get("difficulty");

    if (accuracy >= 0.9999) {
        const double pp_base = level.get("ppBaseScore").number_or(0.0);
        if (pp_base != 0.0) return {pp_base, "ppBaseScore"};
    }
    const double base = level.get("baseScore").number_or(0.0);
    if (base != 0.0) return {base, "baseScore"};

    const double diff_base = difficulty.get("baseScore").number_or(0.0);
    if (diff_base != 0.0) return {diff_base, "difficulty.baseScore"};

    return {1000.0, "default"};
}

double TUFScoreCalculator::calculate_score_multiplier(double accuracy, double base_score, const Json*) const {
    if (accuracy < 0.95 || accuracy > 1.0) return 1.0;
    if (accuracy == 1.0) return base_score != 0.0 ? -2100.0 / (base_score + 262.5) + 14.0 : 1.0;
    return -0.027 / (accuracy - 1.0054) + 0.513;
}

double TUFScoreCalculator::calculate_speed_modifier(double speed, bool is_marathon) const {
    if (is_marathon) {
        if (speed == 0.0 || speed == 1.0) return 1.0;
        if (speed < 1.0) return 0.0;
        return std::max(0.0, 2.0 - speed);
    }

    if (speed == 0.0 || speed == 1.0) return 1.0;
    if (speed < 1.0) return 0.0;
    if (speed < 1.1) return -3.5 * speed + 4.5;
    if (speed < 1.5) return 0.65;
    if (speed < 2.0) return 0.7 * speed - 0.4;
    return 1.0;
}

double TUFScoreCalculator::calculate_empty_tap_modifier(long long empty_taps, long long tilecount) {
    const long long adjusted_empty_taps = std::max<long long>(0, empty_taps - tilecount / 315);
    if (empty_taps == 0) return 1.1;
    if (adjusted_empty_taps == 0) return 1.0;
    if (adjusted_empty_taps == 1) return 0.9;
    if (adjusted_empty_taps <= 25.5) {
        return 0.9 - 0.2 * std::pow((static_cast<double>(adjusted_empty_taps) - 1.0) / 24.5, 0.7);
    }
    if (adjusted_empty_taps <= 50) {
        return 0.5 + 0.2 * std::pow((50.0 - static_cast<double>(adjusted_empty_taps)) / 24.5, 0.7);
    }
    return 0.5;
}

Json TUFScoreCalculator::calculate_score(const Json& level_data, double accuracy_pct, long long misses, double speed,
                                         bool is_no_hold_tap) const {
    const Json& level = level_data.has("level") ? level_data.get("level") : level_data;
    const Json& difficulty = level.get("difficulty");

    const double accuracy = accuracy_pct / 100.0;

    const std::pair<double, std::string> base = get_base_score(level_data, accuracy);
    const double base_score = base.first;
    const std::string base_source = base.second;
    const double multiplier = calculate_score_multiplier(accuracy, base_score);

    const bool is_marathon = difficulty.get("name").as_string() == "Marathon";
    const double speed_mod = calculate_speed_modifier(speed, is_marathon);

    const long long tilecount = level.get("tilecount").is_number() ? level.get("tilecount").as_int() : 0;
    const double empty_tap_mod = calculate_empty_tap_modifier(misses, tilecount);
    const double default_setting_mod = is_no_hold_tap ? 0.9 : 1.0;
    const double raw_score = base_score * multiplier * speed_mod * empty_tap_mod * default_setting_mod;
    const double final_score = std::max(raw_score, base_score);

    Json result = Json::object();
    result.set("accuracy", accuracy);
    result.set("accuracy_pct", py_round(accuracy_pct, 2));
    result.set("score", final_score);
    result.set("raw_score", raw_score);
    result.set("base_score", base_score);
    result.set("base_source", base_source);
    result.set("multiplier", py_round(multiplier, 4));
    result.set("speed_mod", py_round(speed_mod, 4));
    result.set("empty_tap_mod", py_round(empty_tap_mod, 4));
    result.set("default_setting_mod", default_setting_mod);
    result.set("tilecount", tilecount);
    return result;
}

// =========================================================== XACCTools =====
// These two functions are thin compatibility wrappers around the exact integer
// model in xacc_model.hpp / xacc_solver.hpp.  New code should load one XaccModel
// per operation and call the model-aware overloads directly.

double xacc_calc(const std::vector<long long>& judgements, bool xperfect) {
    XaccModel model;
    std::string error;
    if (!xacc_model_load(model, error)) {
        log("[XACC] 读取配置失败: " + error);
        return 0.0;
    }
    return xacc_calc(model, judgements, xperfect);
}

double xacc_calc(const XaccModel& model, const std::vector<long long>& judgements, bool xperfect) {
    const std::vector<std::string>& keys = jd_keys(xperfect);
    if (judgements.size() != keys.size()) return 0.0;

    XaccCount total = 0;
    XaccScoreUnit weighted = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (judgements[i] < 0) return 0.0;
        total += judgements[i];
        std::int64_t term = 0;
        if (!mul_checked(model.weight(keys[i]), judgements[i], term)) return 0.0;
        if (!add_checked(weighted, term, weighted)) return 0.0;
    }
    if (total == 0) return 0.0;
    return xacc_acc_percent(weighted, total, model) / 100.0;
}

std::optional<std::map<std::string, long long>> xacc_reverse(double target_acc, long long total,
                                                             const std::map<std::string, long long>& fixed_counts,
                                                             int acc_decimals, bool xperfect) {
    XaccModel model;
    std::string error;
    if (!xacc_model_load(model, error)) {
        log("[XACCreverse] 读取配置失败: " + error);
        return std::nullopt;
    }
    if (total < 0) {
        log("[XACCreverse] 无解: 物量不能为负数");
        return std::nullopt;
    }
    if (!(target_acc >= 0.0) || target_acc > 100.0) {
        log("[XACCreverse] 无解: 目标 XACC 必须落在 0 到 100 之间");
        return std::nullopt;
    }

    const int decimals = std::min(6, std::max(0, acc_decimals));
    // The double entry point only survives for compatibility; render it back to a
    // decimal string first so the exact parser (not the double) decides the window.
    const std::string text = format_fixed(target_acc, decimals);
    XaccTarget target;
    if (!xacc_parse_target(text, target, error)) {
        log("[XACCreverse] 目标无效: " + error);
        return std::nullopt;
    }

    const XaccReverseResult result = xacc_reverse_search(model, target, total, fixed_counts, xperfect);
    if (!result.ok()) {
        log("[XACCreverse] 无解: " + result.message);
        return std::nullopt;
    }
    for (const auto& entry : result.counts) {
        log("[XACCreverse] " + entry.first + "=" + std::to_string(entry.second));
    }
    log("[XACCreverse] 完成: score=" + format_fixed(result.score_units, 6) + ", cost=" +
        std::to_string(result.cost_units) + ", solver=" + xacc_solver_text(result.solver) +
        ", optimal=" + std::string(result.optimal ? "yes" : "no"));
    return result.counts;
}

}  // namespace tuf
