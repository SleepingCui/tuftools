#include "xacc_model.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "apppaths.hpp"
#include "console.hpp"
#include "numfmt.hpp"
#include "pyjson.hpp"
#include "tools.hpp"

namespace tuf {

namespace {

constexpr const char* XACC_FILE = "xacc.json";
constexpr const char* LEGACY_WEIGHTS_FILE = "weights.json";
constexpr const char* LEGACY_COSTS_FILE = "costs.json";
constexpr const char* BACKUP_SUFFIX = ".v1.bak";
constexpr int kSchemaVersion = 2;
constexpr XaccScoreUnit kDefaultScoreScale = 1000;
constexpr XaccCostUnit kDefaultCostScale = 1000;
// A legacy value that misses the unit grid by more than this is refused instead
// of silently rounded (tgb.md 4.1).
constexpr double kExactEpsilon = 1e-6;
constexpr int kMaxTargetDecimals = 6;

bool file_exists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

bool read_text_file(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

std::int64_t pow10_i64(int exponent) {
    std::int64_t value = 1;
    for (int i = 0; i < exponent; ++i) value *= 10;
    return value;
}

// Converts a decimal value to units, reporting whether it lands exactly on the
// unit grid.  Callers decide what to do about an inexact value: the migration
// path refuses it and names the key, the interactive editor rounds and says so.
bool units_from_value(double value, std::int64_t scale, std::int64_t& out, bool& exact) {
    exact = false;
    if (!std::isfinite(value)) return false;
    const double scaled = value * static_cast<double>(scale);
    if (scaled > 9.0e18 || scaled < -9.0e18) return false;
    const std::int64_t units = static_cast<std::int64_t>(std::llround(scaled));
    exact = std::fabs(scaled - static_cast<double>(units)) <= kExactEpsilon;
    out = units;
    return true;
}

bool fill_missing_with_defaults(XaccModel& model, bool& repaired) {
    repaired = false;
    const std::map<std::string, double>& default_weights = fixed_jd_weights();
    const std::map<std::string, double>& default_costs = default_jd_costs();
    for (const std::string& key : jd_cost_keys()) {
        if (model.weight_units.find(key) == model.weight_units.end()) {
            std::int64_t units = 0;
            bool exact = false;
            if (!units_from_value(default_weights.at(key), model.score_scale, units, exact)) return false;
            model.weight_units[key] = units;
            repaired = true;
        }
        if (model.cost_units.find(key) == model.cost_units.end()) {
            std::int64_t units = 0;
            bool exact = false;
            if (!units_from_value(default_costs.at(key), model.cost_scale, units, exact)) return false;
            model.cost_units[key] = units;
            repaired = true;
        }
    }
    return true;
}

// The judgement weights are a fixed game rule, not a setting: whatever a data file
// claims is overwritten here, and a hand edit is REPORTED (ignored=true) instead of
// silently changing what every XACC percentage means.
bool use_fixed_weights(XaccModel& model, bool& ignored, std::string& error) {
    ignored = false;
    const std::map<std::string, double>& fixed = fixed_jd_weights();
    for (const std::string& key : jd_cost_keys()) {
        std::int64_t units = 0;
        bool exact = false;
        if (!units_from_value(fixed.at(key), model.score_scale, units, exact) || !exact) {
            error = "内置 XACC 权重无法用 1/" + std::to_string(model.score_scale) + " 的精度表示: " + key;
            return false;
        }
        auto it = model.weight_units.find(key);
        if (it != model.weight_units.end() && it->second != units) ignored = true;
        model.weight_units[key] = units;
    }
    return true;
}

bool validate_model(XaccModel& model, std::string& error) {
    if (model.score_scale <= 0) {
        error = "scoreScale 必须为正整数";
        return false;
    }
    if (model.cost_scale <= 0) {
        error = "costScale 必须为正整数";
        return false;
    }
    for (const std::string& key : jd_cost_keys()) {
        const XaccScoreUnit weight = model.weight_units.at(key);
        const XaccCostUnit cost = model.cost_units.at(key);
        if (weight < 0 || weight > model.score_scale) {
            error = "XACC 权重超出 0..scoreScale 范围: " + key + " = " +
                    std::to_string(weight) + "/" + std::to_string(model.score_scale);
            return false;
        }
        if (cost < 0) {
            error = "难度系数不能为负: " + key + " = " + std::to_string(cost);
            return false;
        }
    }
    model.order = jd_cost_keys();
    return true;
}

}  // namespace

// --------------------------------------------------------- integer helpers --

bool mul_checked(std::int64_t a, std::int64_t b, std::int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_mul_overflow(a, b, &out);
#else
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    const std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if ((a == -1 && b == kMin) || (b == -1 && a == kMin)) return false;
    const long double product = static_cast<long double>(a) * static_cast<long double>(b);
    if (product > static_cast<long double>(std::numeric_limits<std::int64_t>::max()) ||
        product < static_cast<long double>(kMin)) {
        return false;
    }
    const std::int64_t result = a * b;
    if (result / b != a) return false;
    out = result;
    return true;
#endif
}

bool add_checked(std::int64_t a, std::int64_t b, std::int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_add_overflow(a, b, &out);
#else
    const std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    const std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b)) return false;
    out = a + b;
    return true;
#endif
}

std::int64_t gcd_i64(std::int64_t a, std::int64_t b) {
    if (a < 0) a = (a == std::numeric_limits<std::int64_t>::min()) ? std::numeric_limits<std::int64_t>::max() : -a;
    if (b < 0) b = (b == std::numeric_limits<std::int64_t>::min()) ? std::numeric_limits<std::int64_t>::max() : -b;
    while (b != 0) {
        const std::int64_t next = a % b;
        a = b;
        b = next;
    }
    return a;
}

std::int64_t ceil_div_i64(std::int64_t a, std::int64_t b) {
    if (b <= 0) return 0;
    const std::int64_t quotient = a / b;
    const std::int64_t remainder = a % b;
    if (remainder == 0) return quotient;
    return (a > 0) ? quotient + 1 : quotient;
}

std::int64_t round_div_i64(std::int64_t a, std::int64_t b) {
    if (b <= 0) return 0;
    const std::int64_t quotient = a / b;
    const std::int64_t remainder = a % b;
    if (remainder == 0) return quotient;
    const std::int64_t twice = (remainder < 0 ? -remainder : remainder) * 2;
    if (twice >= b) return (a < 0) ? quotient - 1 : quotient + 1;
    return quotient;
}

// --------------------------------------------------------------- statuses ---

const char* xacc_status_text(XaccStatus status) {
    switch (status) {
        case XaccStatus::ok: return "成功";
        case XaccStatus::invalid_input: return "输入无效";
        case XaccStatus::invalid_config: return "配置无效";
        case XaccStatus::out_of_range: return "目标在物理范围外";
        case XaccStatus::unreachable: return "目标不可达";
        case XaccStatus::budget_exhausted: return "预算耗尽";
    }
    return "未知状态";
}

const char* xacc_solver_text(XaccSolver solver) {
    switch (solver) {
        case XaccSolver::automatic: return "自动";
        case XaccSolver::exact_dp: return "精确DP(小规模)";
        case XaccSolver::branch_and_bound: return "分支定界(大规模)";
        case XaccSolver::legacy_map_dp: return "旧版map DP";
    }
    return "未知求解器";
}

const std::vector<XaccSolver>& xacc_solver_all() {
    static const std::vector<XaccSolver> all = {XaccSolver::automatic, XaccSolver::exact_dp,
                                                XaccSolver::branch_and_bound, XaccSolver::legacy_map_dp};
    return all;
}

bool xacc_solver_from_text(const std::string& text, XaccSolver& out) {
    const std::string key = lower_ascii(trim(text));
    if (key.empty()) return false;

    // The numeric form follows the menu numbering: 1..N in xacc_solver_all()
    // order.  "0" stays accepted as an extra alias for the automatic choice.
    bool digits = true;
    for (char ch : key) {
        if (ch < '0' || ch > '9') {
            digits = false;
            break;
        }
    }
    if (digits) {
        if (key.size() > 9) return false;
        long long index = 0;
        for (char ch : key) index = index * 10 + (ch - '0');
        if (index == 0) {
            out = XaccSolver::automatic;
            return true;
        }
        const std::vector<XaccSolver>& all = xacc_solver_all();
        if (index >= 1 && index <= static_cast<long long>(all.size())) {
            out = all[static_cast<std::size_t>(index - 1)];
            return true;
        }
        return false;
    }

    if (key == "auto" || key == "automatic" || key == "自动") {
        out = XaccSolver::automatic;
        return true;
    }
    if (key == "dp" || key == "exact" || key == "exact_dp" || key == "精确dp" || key == "精确") {
        out = XaccSolver::exact_dp;
        return true;
    }
    if (key == "bb" || key == "bandb" || key == "branch_and_bound" || key == "分支定界") {
        out = XaccSolver::branch_and_bound;
        return true;
    }
    if (key == "legacy" || key == "map" || key == "legacymapdp" || key == "legacy_map_dp" || key == "旧版" ||
        key == "旧版mapdp") {
        out = XaccSolver::legacy_map_dp;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ model ---

XaccScoreUnit XaccModel::weight(const std::string& key) const {
    auto it = weight_units.find(key);
    return it == weight_units.end() ? 0 : it->second;
}

XaccCostUnit XaccModel::cost(const std::string& key) const {
    auto it = cost_units.find(key);
    return it == cost_units.end() ? 0 : it->second;
}

bool xacc_units_from_value(double value, std::int64_t scale, std::int64_t& out, bool& exact) {
    return units_from_value(value, scale, out, exact);
}

bool xacc_model_save(const XaccModel& model, std::string& error) {
    try {
        Json root = Json::object();
        root.set("schemaVersion", static_cast<long long>(kSchemaVersion));
        root.set("scoreScale", static_cast<long long>(model.score_scale));
        root.set("costScale", static_cast<long long>(model.cost_scale));
        root.set("maxDpCells", model.max_dp_cells);
        root.set("maxNodes", model.max_nodes);
        root.set("maxSeconds", model.max_seconds);
        Json weights = Json::object();
        Json costs = Json::object();
        for (const std::string& key : jd_cost_keys()) {
            weights.set(key, static_cast<long long>(model.weight(key)));
            costs.set(key, static_cast<long long>(model.cost(key)));
        }
        root.set("weights", weights);
        root.set("costs", costs);
        std::ofstream file(data_file(XACC_FILE), std::ios::binary | std::ios::trunc);
        if (!file.good()) {
            error = std::string("无法写入 ") + XACC_FILE;
            return false;
        }
        file << root.dump(4);
        return true;
    } catch (const std::exception& e) {
        error = std::string("保存 ") + XACC_FILE + " 失败: " + e.what();
        return false;
    }
}

namespace {

// Reads the schemaVersion 2 file.  Everything must be present; missing table
// entries are refilled from the built-in defaults and reported, so a hand edited
// file with one deleted key still works.
bool parse_v2(const Json& root, XaccModel& model, std::string& error, std::string* note) {
    if (!root.is_object()) {
        error = std::string(XACC_FILE) + " 顶层不是 JSON 对象";
        return false;
    }
    const Json version = root.get("schemaVersion");
    if (!version.is_number() || version.as_int() != kSchemaVersion) {
        error = std::string(XACC_FILE) + " 的 schemaVersion 不是 " + std::to_string(kSchemaVersion) +
                "，请删除该文件让程序从 costs.json / weights.json 重新迁移";
        return false;
    }
    const Json score_scale = root.get("scoreScale");
    const Json cost_scale = root.get("costScale");
    if (!score_scale.is_number() || !cost_scale.is_number()) {
        error = std::string(XACC_FILE) + " 缺少 scoreScale / costScale";
        return false;
    }
    model.score_scale = score_scale.as_int();
    model.cost_scale = cost_scale.as_int();
    if (model.score_scale <= 0 || model.cost_scale <= 0) {
        error = std::string(XACC_FILE) + " 的 scoreScale / costScale 必须为正";
        return false;
    }


    const Json max_dp_cells = root.get("maxDpCells");
    const Json max_nodes = root.get("maxNodes");
    const Json max_seconds = root.get("maxSeconds");
    bool budgets_missing = false;
    for (int which = 0; which < 3; ++which) {
        const Json& value = which == 0 ? max_dp_cells : (which == 1 ? max_nodes : max_seconds);
        if (value.is_null()) {
            budgets_missing = true;
            continue;
        }
        if (!value.is_number()) {
            error = std::string(XACC_FILE) + " 的求解器预算不是数值: " +
                    (which == 0 ? "maxDpCells" : (which == 1 ? "maxNodes" : "maxSeconds"));
            return false;
        }
    }
    if (!max_dp_cells.is_null()) model.max_dp_cells = max_dp_cells.as_int();
    if (!max_nodes.is_null()) model.max_nodes = max_nodes.as_int();
    if (!max_seconds.is_null()) model.max_seconds = max_seconds.as_double();
    if (model.max_dp_cells <= 0 || model.max_nodes <= 0 || !(model.max_seconds > 0.0)) {
        error = std::string(XACC_FILE) + " 的求解器预算必须为正 (maxDpCells / maxNodes / maxSeconds)";
        return false;
    }

    const Json weights = root.get("weights");
    const Json costs = root.get("costs");
    bool inexact_note = false;
    if (weights.is_object()) {
        for (const std::pair<std::string, Json>& item : weights.items()) {
            const Json& value = item.second;
            if (!value.is_number()) {
                error = std::string(XACC_FILE) + " 的权重不是数值: " + item.first;
                return false;
            }
            if (value.is_int()) {
                model.weight_units[item.first] = value.as_int();
            } else {
                std::int64_t units = 0;
                bool exact = false;
                if (!units_from_value(value.as_double(), model.score_scale, units, exact)) {
                    error = std::string(XACC_FILE) + " 的权重无法转换: " + item.first;
                    return false;
                }
                inexact_note = inexact_note || !exact;
                model.weight_units[item.first] = units;
            }
        }
    }
    if (costs.is_object()) {
        for (const std::pair<std::string, Json>& item : costs.items()) {
            const Json& value = item.second;
            if (!value.is_number()) {
                error = std::string(XACC_FILE) + " 的难度系数不是数值: " + item.first;
                return false;
            }
            if (value.is_int()) {
                model.cost_units[item.first] = value.as_int();
            } else {
                std::int64_t units = 0;
                bool exact = false;
                if (!units_from_value(value.as_double(), model.cost_scale, units, exact)) {
                    error = std::string(XACC_FILE) + " 的难度系数无法转换: " + item.first;
                    return false;
                }
                inexact_note = inexact_note || !exact;
                model.cost_units[item.first] = units;
            }
        }
    }

    bool repaired = false;
    if (!fill_missing_with_defaults(model, repaired)) {
        error = std::string(XACC_FILE) + " 缺少判定且有默认值无法转换";
        return false;
    }
    bool weights_ignored = false;
    if (!use_fixed_weights(model, weights_ignored, error)) return false;
    if (!validate_model(model, error)) return false;

    if (note != nullptr) {
        std::vector<std::string> reasons;
        if (repaired) reasons.push_back("缺少部分判定，已用默认值补齐");
        if (budgets_missing) reasons.push_back("缺少求解器预算，已用默认值补齐");
        if (inexact_note) reasons.push_back("存在按精度取整的数值");
        if (weights_ignored) reasons.push_back("判定权重是固定规则，已忽略文件里的 weights 并改回");
        if (!reasons.empty()) {
            if (!note->empty()) *note += "；";
            *note += std::string("注意: ") + XACC_FILE + " ";
            for (std::size_t i = 0; i < reasons.size(); ++i) {
                if (i != 0) *note += "；";
                *note += reasons[i];
            }
        }
    }
    // Rewrite the file when it was missing keys or carried custom weights, so a
    // hand edit cannot stay in the file pretending to be in effect.
    if (repaired || weights_ignored || budgets_missing) {
        std::string save_error;
        if (!xacc_model_save(model, save_error) && note != nullptr) {
            *note += "；" + save_error;
        }
    }
    return true;
}

// Converts the legacy costs.json (doubles) into the integer model, refusing any
// cost that does not land exactly on the unit grid.  Judgement weights are never
// taken from a file; a legacy weights.json is only retired.
bool migrate_legacy(XaccModel& model, std::string& error, std::string* note) {
    model.score_scale = kDefaultScoreScale;
    model.cost_scale = kDefaultCostScale;

    const std::map<std::string, double> legacy_costs = costs_load();

    const bool had_weights = file_exists(data_file(LEGACY_WEIGHTS_FILE));
    const bool had_costs = file_exists(data_file(LEGACY_COSTS_FILE));

    for (const std::string& key : jd_cost_keys()) {
        std::int64_t units = 0;
        bool exact = false;
        // Weights come from the compiled-in table only; a legacy weights.json is
        // retired below rather than honoured.
        const double weight = fixed_jd_weights().at(key);
        if (!units_from_value(weight, model.score_scale, units, exact) || !exact) {
            error = "权重无法转换: " + key + " = " + py_float_str(weight);
            return false;
        }
        model.weight_units[key] = units;

        const double cost = legacy_costs.count(key) ? legacy_costs.at(key) : default_jd_costs().at(key);
        if (!units_from_value(cost, model.cost_scale, units, exact)) {
            error = "难度系数无法转换: " + key + " = " + py_float_str(cost);
            return false;
        }
        if (!exact) {
            error = "难度系数无法精确表示到 1/" + std::to_string(model.cost_scale) + " 精度: " + key + " = " +
                    py_float_str(cost) + "；请先修正 " + LEGACY_COSTS_FILE;
            return false;
        }
        model.cost_units[key] = units;
    }

    if (!validate_model(model, error)) return false;

    std::string save_error;
    const bool saved = xacc_model_save(model, save_error);
    if (note != nullptr) {
        if (saved) {
            *note = std::string("已生成 ") + XACC_FILE;
        } else {
            *note = save_error + "，使用内存中的配置";
        }
    }

    // Retire the legacy files so a later hand edit cannot silently do nothing.
    if (had_weights || had_costs) {
        const char* names[2] = {LEGACY_WEIGHTS_FILE, LEGACY_COSTS_FILE};
        const bool present[2] = {had_weights, had_costs};
        for (int i = 0; i < 2; ++i) {
            if (!present[i]) continue;
            const std::string path = data_file(names[i]);
            const std::string backup = path + BACKUP_SUFFIX;
            std::remove(backup.c_str());
            if (std::rename(path.c_str(), backup.c_str()) != 0 && note != nullptr) {
                *note += std::string("；未能备份 ") + names[i];
            }
        }
    }
    if (had_weights && note != nullptr) {
        *note += std::string("；") + LEGACY_WEIGHTS_FILE +
                 " 已停用";
    }
    return true;
}

}  // namespace

bool xacc_model_load(XaccModel& model, std::string& error, std::string* note) {
    model = XaccModel();
    std::string text;
    if (read_text_file(data_file(XACC_FILE), text)) {
        try {
            const Json root = Json::parse(text);
            return parse_v2(root, model, error, note);
        } catch (const std::exception& e) {
            error = std::string(XACC_FILE) + " 无法解析: " + e.what();
            return false;
        }
    }
    return migrate_legacy(model, error, note);
}

// ----------------------------------------------------------------- target ---

bool xacc_parse_target(const std::string& text, XaccTarget& out, std::string& error) {
    std::string value = trim(text);
    if (!value.empty() && value.back() == '%') value = trim(value.substr(0, value.size() - 1));
    if (value.empty()) {
        error = "目标 XACC 不能为空";
        return false;
    }

    std::string digits;
    int decimals = 0;
    bool seen_dot = false;
    bool seen_digit = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c >= '0' && c <= '9') {
            digits.push_back(c);
            seen_digit = true;
            if (seen_dot) {
                ++decimals;
                if (decimals > kMaxTargetDecimals) {
                    error = "目标 XACC 小数位最多 " + std::to_string(kMaxTargetDecimals) + " 位";
                    return false;
                }
            }
            continue;
        }
        if (c == '.') {
            if (seen_dot) {
                error = "目标 XACC 格式错误: " + text;
                return false;
            }
            seen_dot = true;
            continue;
        }
        error = "目标 XACC 必须是 0..100 的数字: " + text;
        return false;
    }
    if (!seen_digit) {
        error = "目标 XACC 格式错误: " + text;
        return false;
    }

    // Strip leading zeros so the numerator stays small.
    std::size_t first = digits.find_first_not_of('0');
    const std::string trimmed = (first == std::string::npos) ? std::string("0") : digits.substr(first);

    std::int64_t numerator = 0;
    for (char c : trimmed) {
        if (!mul_checked(numerator, 10, numerator)) {
            error = "目标 XACC 数值过大: " + text;
            return false;
        }
        if (!add_checked(numerator, static_cast<std::int64_t>(c - '0'), numerator)) {
            error = "目标 XACC 数值过大: " + text;
            return false;
        }
    }

    std::int64_t limit = 0;
    if (!mul_checked(100, pow10_i64(decimals), limit) || numerator > limit) {
        error = "目标 XACC 必须在 0..100 之间: " + text;
        return false;
    }

    out.decimals = decimals;
    out.numerator = numerator;
    return true;
}

bool xacc_target_window(const XaccTarget& target, const XaccModel& model, XaccCount total,
                        XaccScoreUnit& slo, XaccScoreUnit& shi, std::string& error) {
    slo = 0;
    shi = 0;
    if (total < 0) {
        error = "物量不能为负";
        return false;
    }
    // percent = score_units * 100 / (total * score_scale); the typed value is
    // numerator / 10^decimals, accepted over [num - 0.5, num + 0.5) / 10^d.
    std::int64_t denominator = 200;
    if (!mul_checked(denominator, pow10_i64(target.decimals), denominator)) {
        error = "精度过大";
        return false;
    }
    std::int64_t base = 0;
    if (!mul_checked(total, model.score_scale, base)) {
        error = "物量与权重精度相乘溢出";
        return false;
    }
    std::int64_t low = 0;
    std::int64_t high = 0;
    if (!mul_checked(2 * target.numerator - 1, base, low) || !mul_checked(2 * target.numerator + 1, base, high)) {
        error = "目标 XACC 与物量相乘溢出";
        return false;
    }
    slo = ceil_div_i64(low, denominator);
    shi = ceil_div_i64(high, denominator) - 1;
    if (slo < 0) slo = 0;
    if (shi < slo) {
        error = "该精度下不存在可达的分数区间";
        return false;
    }
    return true;
}

bool xacc_target_units(const XaccTarget& target, const XaccModel& model, XaccCount total,
                       XaccScoreUnit& out, std::string& error) {
    out = 0;
    if (total < 0) {
        error = "物量不能为负";
        return false;
    }
    std::int64_t denominator = 100;
    if (!mul_checked(denominator, pow10_i64(target.decimals), denominator)) {
        error = "精度过大";
        return false;
    }
    std::int64_t base = 0;
    if (!mul_checked(target.numerator, total, base) || !mul_checked(base, model.score_scale, base)) {
        error = "目标 XACC 与物量相乘溢出";
        return false;
    }
    out = round_div_i64(base, denominator);
    return true;
}

// --------------------------------------------------------------- printing ---

double xacc_acc_percent(XaccScoreUnit score_units, XaccCount total, const XaccModel& model) {
    if (total <= 0) return 0.0;
    const double denominator = static_cast<double>(total) * static_cast<double>(model.score_scale);
    if (denominator <= 0.0) return 0.0;
    return 100.0 * static_cast<double>(score_units) / denominator;
}

std::string xacc_format_acc(XaccScoreUnit score_units, XaccCount total, const XaccModel& model, int decimals) {
    if (decimals < 0) decimals = 0;
    if (decimals > kMaxTargetDecimals) decimals = kMaxTargetDecimals;
    return format_fixed(xacc_acc_percent(score_units, total, model), decimals);
}

std::string xacc_format_weight(const XaccModel& model, const std::string& key) {
    if (model.score_scale <= 0) return py_float_str(0.0);
    return py_float_str(static_cast<double>(model.weight(key)) / static_cast<double>(model.score_scale));
}

std::string xacc_format_cost(const XaccModel& model, const std::string& key) {
    if (model.cost_scale <= 0) return py_float_str(0.0);
    return py_float_str(static_cast<double>(model.cost(key)) / static_cast<double>(model.cost_scale));
}

}  // namespace tuf
