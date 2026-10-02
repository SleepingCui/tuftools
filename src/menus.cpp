#include "menus.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "api.hpp"
#include "console.hpp"
#include "info.hpp"
#include "pyjson.hpp"
#include "numfmt.hpp"
#include "tools.hpp"

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

}  // namespace

// ============================================================== ppcalc.py ====

void calculate_rank_changes(double calculated_score) {
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
    const double new_score = old_score + calculated_score;

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
    std::cout << "排位分数: " << format_fixed(old_score, 2) << " -> " << format_fixed(new_score, 2) << " (+"
              << format_fixed(calculated_score, 2) << ")" << std::endl;
    std::cout << "全球排名: " << old_global_rank.str() << " -> " << new_global_rank.str() << " (" << global_delta << ")"
              << std::endl;
    std::cout << "国家排名: " << old_country_rank.str() << " -> " << new_country_rank.str() << " (" << country_delta
              << ")" << std::endl;
}

void handle_pp_calc() {
    TUFScoreCalculator calculator;
    DifficultyManager diffman;

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

    std::cout << "选择BaseScore来源:" << std::endl;
    std::cout << "1. 按难度等级" << std::endl;
    std::cout << "2. 直接输入BaseScore" << std::endl;
    const std::string base_mode = read_trimmed("> ");

    std::string difficulty;
    std::optional<double> base_score;
    if (base_mode == "2") {
        const std::optional<double> parsed = try_parse_double(read_trimmed("BaseScore: "));
        if (!parsed.has_value()) {
            std::cout << "could not convert string to float" << std::endl;
            return;
        }
        base_score = parsed;
    } else if (base_mode == "1") {
        difficulty = upper_ascii(read_trimmed("难度: "));
    } else {
        std::cout << "无效选择" << std::endl;
        return;
    }

    const bool marathon = lower_ascii(read_trimmed("是否Marathon(y/N): ")) == "y";

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

    Json level_data;
    try {
        level_data = calculator.build_level_data(difficulty, marathon, tilecount, base_score);
    } catch (const std::exception& e) {
        std::cout << e.what() << std::endl;
        return;
    }

    const std::optional<double> accuracy_parsed = try_parse_double(read_trimmed("XACC: "));
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

    const double raw_score = result.get("raw_score").as_double();
    const double base_score_result = result.get("base_score").as_double();
    std::cout << std::endl;
    if (raw_score < base_score_result) {
        std::cout << "PP分: " << float_of(result.get("base_score")) << " (" << float_of(result.get("raw_score")) << ")"
                  << std::endl;
    } else {
        std::cout << "PP分: " << float_of(result.get("score")) << std::endl;
    }

    const std::string calc_rank = lower_ascii(read_trimmed("\n是否计算排名变化? (y/N): "));
    if (calc_rank == "y") {
        calculate_rank_changes(result.get("score").as_double());
    }

    std::cout << std::endl;
    stats();
}

// ============================================================ acccalc.py ====

namespace {

const std::vector<std::string>& xacc_keys() { return jd_keys(); }

void acc_run1() {
    std::cout << "输入格式: failMiss tooEarly early EPerfect perfect LPerfect late" << std::endl;

    const std::string raw = read_trimmed("判定数据: ");

    std::vector<std::string> values;
    std::istringstream stream(raw);
    std::string token;
    while (stream >> token) values.push_back(token);

    if (values.size() != 7) {
        std::cout << "需要输入7个数字" << std::endl;
        return;
    }

    std::vector<long long> judgements;
    for (const std::string& value : values) {
        const std::optional<long long> parsed = try_parse_int(value);
        if (!parsed.has_value()) {
            std::cout << "请输入整数" << std::endl;
            return;
        }
        judgements.push_back(*parsed);
    }

    std::cout << std::endl;
    std::cout << "XACC: " << py_float_str(xacc_calc(judgements) * 100.0) << "%" << std::endl;
}

void acc_run2() {
    const std::optional<long long> total_parsed = try_parse_int(read_trimmed("物量: "));
    if (!total_parsed.has_value()) {
        std::cout << "invalid literal for int() with base 10" << std::endl;
        return;
    }
    const std::string target_text = read_trimmed("XACC: ");
    const std::optional<double> target_parsed = try_parse_double(target_text);
    if (!target_parsed.has_value()) {
        std::cout << "could not convert string to float" << std::endl;
        return;
    }

    const long long total = *total_parsed;
    const double target_acc = *target_parsed;
    // The decimals the user typed define the required precision (search window, and whether
    // the result counts as exact).  The result is always printed at its true accuracy.
    const int acc_decimals = std::min(6, decimal_places_of(target_text));

    if (target_acc < 0.0 || target_acc > 100.0) {
        std::cout << "XACC 范围是 0.0% ~ 100.0%" << std::endl;
        return;
    }

    const std::vector<std::string>& keys = xacc_keys();
    std::map<std::string, long long> fixed_counts;
    for (const std::string& key : keys) {
        const std::string value = read_trimmed("固定 " + key + " 的数量 (直接回车表示不固定): ");
        if (value.empty()) continue;
        const std::optional<long long> parsed = try_parse_int(value);
        if (!parsed.has_value()) {
            std::cout << "已跳过固定 " << key << std::endl;
            continue;
        }
        fixed_counts[key] = *parsed;
    }

    std::cout << "正在计算...这可能需要一些时间" << std::endl;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const std::optional<std::map<std::string, long long>> result =
        xacc_reverse(target_acc, total, fixed_counts, acc_decimals);
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    const double elapsed =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();

    std::cout << std::endl;
    if (!result.has_value()) {
        std::cout << "无法达成该目标 XACC" << std::endl;
    } else {
        const std::map<std::string, long long>& counts = *result;
        for (const std::string& key : keys) {
            const std::string status = fixed_counts.count(key) ? "(Locked)" : "";
            auto it = counts.find(key);
            const long long value = it == counts.end() ? 0 : it->second;
            std::cout << " " << pad_right(key, 10) << ": " << value << " " << status << std::endl;
        }

        // 1 weight unit = 0.05 score = 5/total percent. Sum the units as integers so the
        // printed value is the true accuracy of the split, not accumulated float noise.
        long long score_units = 0;
        for (const std::string& key : keys) {
            auto it = counts.find(key);
            const long long value = it == counts.end() ? 0 : it->second;
            score_units += value * std::llround(jd_weights().at(key) * 20.0);
        }
        const double actual_acc = static_cast<double>(score_units) * 5.0 / static_cast<double>(total);
        std::cout << std::endl << " XACC: " << py_float_str(actual_acc) << "%";
        if (format_fixed(actual_acc, acc_decimals) != format_fixed(target_acc, acc_decimals)) {
            std::cout << "  (该物量下无法精确到小数点后 " << acc_decimals << " 位，以上为最接近 " << format_fixed(target_acc, acc_decimals)
                      << "% 的可达值)";
        }
        std::cout << std::endl;
    }
    std::cout << " Elapsed " << py_float_str(elapsed) << " ms" << std::endl;
}

void acc_run3() {
    std::map<std::string, double> current_costs = costs_load();

    std::cout << "当前各判定难度系数 (数值越低，算法越倾向于用它凑分):" << std::endl;
    for (const std::string& key : jd_cost_keys()) {
        auto it = current_costs.find(key);
        const double value = it == current_costs.end() ? 0.0 : it->second;
        std::cout << "  " << pad_right(key, 10) << ": " << py_float_str(value) << std::endl;
    }

    std::cout << std::endl << "请输入新系数 (直接回车保持不变):" << std::endl;
    bool modified = false;
    for (const std::string& key : jd_cost_keys()) {
        auto it = current_costs.find(key);
        const double current = it == current_costs.end() ? 0.0 : it->second;
        const std::string value = read_trimmed(key + " [" + py_float_str(current) + "]: ");
        if (!value.empty()) {
            const std::optional<double> parsed = try_parse_double(value);
            if (!parsed.has_value()) {
                std::cout << "输入无效，" << key << " 保持原值: could not convert string to float: '" << value << "'"
                          << std::endl;
                continue;
            }
            current_costs[key] = *parsed;
            modified = true;
        }
    }

    std::cout << std::endl;
    if (modified) {
        costs_save(current_costs);
        std::cout << "保存成功" << std::endl;
    } else {
        std::cout << "Not modified" << std::endl;
    }
}

}  // namespace

void handle_acc_calc() {
    std::cout << std::endl;
    while (true) {
        std::cout << std::endl;
        std::cout << "1. 根据判定计算 XACC" << std::endl;
        std::cout << "2. 根据 XACC 推算判定" << std::endl;
        std::cout << "3. 自定义难度系数" << std::endl;
        std::cout << "b. 返回主菜单" << std::endl;

        const std::string choice = read_trimmed("> ");

        if (choice == "1") {
            acc_run1();
        } else if (choice == "2") {
            acc_run2();
        } else if (choice == "3") {
            acc_run3();
        } else if (choice == "b") {
            break;
        } else {
            std::cout << "无效选择" << std::endl;
        }
    }
}

}  // namespace tuf
