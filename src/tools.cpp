#include "tools.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "api.hpp"
#include "apppaths.hpp"
#include "console.hpp"
#include "numfmt.hpp"

namespace tuf {

namespace {
constexpr const char* COSTS_FILE = "costs.json";
constexpr const char* DIFFICULTIES_FILE = "difficulties.json";
}  // namespace

// ============================================================== CostsMan ====

const std::vector<std::string>& jd_cost_keys() {
    static const std::vector<std::string> keys = {"perfect", "failMiss", "tooEarly", "early",
                                                  "late",    "ePerfect", "lPerfect"};
    return keys;
}

const std::map<std::string, double>& default_jd_costs() {
    static const std::map<std::string, double> costs = {
        {"perfect", 0.0},   {"failMiss", 1.0}, {"tooEarly", 1.5},  {"early", 50.0},
        {"late", 50.0},     {"ePerfect", 50.0}, {"lPerfect", 50.0},
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

const std::vector<std::string>& jd_keys() {
    static const std::vector<std::string> keys = {"failMiss", "tooEarly", "early", "ePerfect",
                                                  "perfect",  "lPerfect", "late"};
    return keys;
}

const std::map<std::string, double>& jd_weights() {
    static const std::map<std::string, double> weights = {
        {"failMiss", 0.0}, {"tooEarly", 0.2}, {"early", 0.4}, {"ePerfect", 0.75},
        {"perfect", 1.0},  {"lPerfect", 0.75}, {"late", 0.4},
    };
    return weights;
}

double xacc_calc(const std::vector<long long>& judgements) {
    if (judgements.size() != 7) return 0.0;
    long long total = 0;
    for (long long value : judgements) total += value;
    if (total == 0) return 0.0;

    const std::vector<std::string>& keys = jd_keys();
    double weighted_sum = 0.0;
    for (size_t i = 0; i < 7; ++i) weighted_sum += static_cast<double>(judgements[i]) * jd_weights().at(keys[i]);
    return weighted_sum / static_cast<double>(total);
}

std::optional<std::map<std::string, long long>> xacc_reverse(double target_acc, long long total,
                                                             const std::map<std::string, long long>& fixed_counts) {
    const std::map<std::string, double> costs_dict = costs_load();
    const std::vector<std::string>& keys = jd_keys();

    std::map<std::string, long long> weight_units;
    for (const std::string& key : keys) {
        weight_units[key] = static_cast<long long>(std::llround(jd_weights().at(key) * 20.0));
    }
    const double tolerant_percent = 0.005;
    const double eps = 1e-9;

    const auto fail = [](const std::string& reason) -> std::optional<std::map<std::string, long long>> {
        log("[XACCreverse] 无解: " + reason);
        return std::nullopt;
    };

    {
        std::string fixed_text = "{";
        bool first = true;
        for (const auto& kv : fixed_counts) {
            if (!first) fixed_text += ", ";
            first = false;
            fixed_text += "'" + kv.first + "': " + std::to_string(kv.second);
        }
        fixed_text += "}";
        log("[XACCreverse] 开始: target=" + format_fixed(target_acc, 6) + "%, total=" + std::to_string(total) +
            ", fixed=" + fixed_text);
    }

    if (total < 0 || target_acc < 0.0 || target_acc > 100.0) {
        return fail("total 或 target_acc 超出范围");
    }

    std::map<std::string, long long> result;
    for (const std::string& key : keys) result[key] = 0;

    long long rem_notes = total;
    long long fixed_score_units = 0;
    double fixed_cost = 0.0;

    for (const std::string& key : keys) {
        auto it = fixed_counts.find(key);
        if (it == fixed_counts.end()) continue;
        const long long val = it->second;
        if (val < 0 || val > rem_notes) return fail("固定数量无效: " + key + "=" + std::to_string(val));
        result[key] = val;
        rem_notes -= val;
        fixed_score_units += val * weight_units[key];
        fixed_cost += static_cast<double>(val) * costs_dict.at(key);
    }

    std::vector<std::string> free_keys;
    for (const std::string& key : keys) {
        if (!fixed_counts.count(key)) free_keys.push_back(key);
    }
    if (rem_notes && free_keys.empty()) {
        return fail("剩余物量大于 0，但没有可用的非固定判定");
    }

    const double min_score = (target_acc - tolerant_percent) / 100.0 * static_cast<double>(total);
    const double max_score = (target_acc + tolerant_percent) / 100.0 * static_cast<double>(total);
    log("[XACCreverse] fixed_score=" + format_fixed(static_cast<double>(fixed_score_units) / 20.0, 4) +
        ", remaining=" + std::to_string(rem_notes) + ", target_range=[" + format_fixed(min_score, 6) + ", " +
        format_fixed(max_score, 6) + "]");

    if (!rem_notes) {
        const double actual = static_cast<double>(fixed_score_units) / 20.0;
        if (min_score - eps <= actual && actual <= max_score + eps) {
            log("[XACCreverse] 固定判定已满足目标: score=" + format_fixed(actual, 6));
            return result;
        }
        return fail("固定判定分数 " + format_fixed(actual, 6) + " 不在目标区间");
    }

    // Same-weight judgements lose no score; use the cheapest one as the base.
    long long max_weight = 0;
    for (const std::string& key : free_keys) max_weight = std::max(max_weight, weight_units[key]);

    std::string base_key = free_keys.front();
    double base_cost = costs_dict.at(base_key);
    bool base_found = false;
    for (const std::string& key : free_keys) {
        if (weight_units[key] != max_weight) continue;
        const double cost = costs_dict.at(key);
        if (!base_found || cost < base_cost) {
            base_found = true;
            base_key = key;
            base_cost = cost;
        }
    }
    const double base_score_units = static_cast<double>(fixed_score_units) + static_cast<double>(rem_notes) * static_cast<double>(max_weight);

    long long loss_min = static_cast<long long>(std::ceil((base_score_units / 20.0 - max_score) * 20.0 - eps));
    long long loss_max = static_cast<long long>(std::floor((base_score_units / 20.0 - min_score) * 20.0 + eps));
    loss_min = std::max<long long>(0, loss_min);
    log("[XACCreverse] base=" + base_key + ", base_score=" + format_fixed(base_score_units / 20.0, 4) +
        ", loss_range=[" + std::to_string(loss_min) + ", " + std::to_string(loss_max) + "] (单位=0.05)");
    if (loss_min > loss_max) return fail("最高可达分数也低于目标区间");

    // Judgements with equal loss are interchangeable: keep only the cheapest one.
    std::map<long long, std::pair<std::string, double>> loss_item_map;
    for (const std::string& key : free_keys) {
        const long long item_loss = max_weight - weight_units[key];
        if (item_loss <= 0) continue;
        const double item_cost = costs_dict.at(key) - base_cost;
        auto it = loss_item_map.find(item_loss);
        if (it == loss_item_map.end() || item_cost < it->second.second) {
            loss_item_map[item_loss] = {key, item_cost};
        }
    }

    struct LossItem {
        std::string key;
        long long loss;
        double cost;
    };
    std::vector<LossItem> loss_items;
    for (const auto& kv : loss_item_map) {
        loss_items.push_back(LossItem{kv.second.first, kv.first, kv.second.second});
    }

    {
        std::string text = "[";
        for (size_t i = 0; i < loss_items.size(); ++i) {
            if (i) text += ", ";
            text += "('" + loss_items[i].key + "', " + std::to_string(loss_items[i].loss) + ", " +
                    format_fixed(loss_items[i].cost, 4) + ")";
        }
        text += "]";
        log("[XACCreverse] loss_items=" + text);
    }

    if (loss_items.empty()) {
        if (loss_min <= 0 && 0 <= loss_max) {
            result[base_key] += rem_notes;
            log("[XACCreverse] 无需替换，全部使用 " + base_key);
            return result;
        }
        return fail("没有可用于降低分数的判定");
    }

    struct State {
        double cost = 0.0;
        std::vector<long long> layout;
    };
    struct Best {
        double cost = 0.0;
        long long used = 0;
        long long loss = 0;
        std::vector<long long> layout;
    };

    std::map<long long, State> current;
    current[0] = State{0.0, std::vector<long long>(loss_items.size(), 0)};

    std::optional<Best> best;
    if (loss_min <= 0 && 0 <= loss_max) {
        Best candidate;
        candidate.cost = fixed_cost + static_cast<double>(rem_notes) * base_cost;
        candidate.used = 0;
        candidate.loss = 0;
        candidate.layout = std::vector<long long>(loss_items.size(), 0);
        best = candidate;
    }

    const long long progress_step = std::max<long long>(1, rem_notes / 10);
    for (long long count = 0; count < rem_notes; ++count) {
        if (current.empty()) continue;

        std::map<long long, State> next_states;
        for (const auto& entry : current) {
            for (size_t i = 0; i < loss_items.size(); ++i) {
                const long long new_loss = entry.first + loss_items[i].loss;
                if (new_loss > loss_max) continue;
                std::vector<long long> new_layout = entry.second.layout;
                new_layout[i] += 1;
                const double new_cost = entry.second.cost + loss_items[i].cost;
                auto it = next_states.find(new_loss);
                if (it == next_states.end() || new_cost < it->second.cost - eps) {
                    next_states[new_loss] = State{new_cost, new_layout};
                }
            }
        }
        current = std::move(next_states);

        for (const auto& entry : current) {
            const long long loss = entry.first;
            if (loss < loss_min || loss > loss_max) continue;
            double candidate_cost = fixed_cost + static_cast<double>(rem_notes - count - 1) * base_cost;
            for (size_t i = 0; i < entry.second.layout.size(); ++i) {
                candidate_cost += static_cast<double>(entry.second.layout[i]) * costs_dict.at(loss_items[i].key);
            }
            if (!best.has_value() || candidate_cost < best->cost - eps) {
                Best candidate;
                candidate.cost = candidate_cost;
                candidate.used = count + 1;
                candidate.loss = loss;
                candidate.layout = entry.second.layout;
                best = candidate;
            }
        }

        if ((count + 1) % progress_step == 0 || count + 1 == rem_notes) {
            log("[XACCreverse] DP " + std::to_string(count + 1) + "/" + std::to_string(rem_notes) +
                ", states=" + std::to_string(current.size()));
        }
    }

    if (!best.has_value()) return fail("DP 没有找到落在目标区间内的分数损失");

    for (size_t i = 0; i < best->layout.size(); ++i) result[loss_items[i].key] += best->layout[i];
    result[base_key] += rem_notes - best->used;

    double final_score = 0.0;
    for (const std::string& key : keys) final_score += static_cast<double>(result[key]) * jd_weights().at(key);
    log("[XACCreverse] 完成: loss=" + std::to_string(best->loss) + ", used_replacements=" + std::to_string(best->used) +
        ", score=" + format_fixed(final_score, 6) + ", cost=" + format_fixed(best->cost, 6));
    return result;
}

}  // namespace tuf
