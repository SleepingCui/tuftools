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

// Prompt loop for the difficulty coefficients.  They live in xacc.json as exact
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
