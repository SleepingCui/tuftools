#include "xacc_solver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "api.hpp"
#include "numfmt.hpp"
#include "tools.hpp"

namespace tuf {

namespace {

// Reverse-search tracing.  Everything below goes through api.cpp log(), which
// prints to stdout only when the program was started with -v / --verbose, so a
// normal run stays quiet and the whole trace can be asked for when a result
// looks wrong.  String building is skipped entirely when verbose is off.
void trace(const std::string& message) {
    if (verbose_enabled()) log("[XACCreverse] " + message);
}

std::string num(long long value) { return std::to_string(value); }

constexpr XaccCostUnit kCostInf = std::numeric_limits<XaccCostUnit>::max() / 4;
constexpr long long kNoteInf = std::numeric_limits<long long>::max() / 4;
constexpr long long kMaxLegacyStates = 2000000;
constexpr std::size_t kMaxDominanceEntries = 2000000;
constexpr std::size_t kMaxLexTies = 64;
constexpr long double kTieEpsilon = 1e-9L;

long long floor_div_any(long long a, long long b) {
    if (b == 0) return 0;
    const long long quotient = a / b;
    const long long remainder = a % b;
    if (remainder != 0 && ((remainder < 0) != (b < 0))) return quotient - 1;
    return quotient;
}

long long ceil_div_any(long long a, long long b) {
    if (b == 0) return 0;
    const long long quotient = a / b;
    const long long remainder = a % b;
    if (remainder != 0 && ((remainder < 0) == (b < 0))) return quotient + 1;
    return quotient;
}

bool sub_checked(std::int64_t a, std::int64_t b, std::int64_t& out) {
    if (b == std::numeric_limits<std::int64_t>::min()) return false;
    return add_checked(a, -b, out);
}

std::int64_t pow10_local(int exponent) {
    std::int64_t value = 1;
    for (int i = 0; i < exponent; ++i) value *= 10;
    return value;
}

double seconds_since(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

struct Item {
    std::string key;
    int key_index = 0;
    XaccScoreUnit weight = 0;
    XaccCostUnit cost = 0;
    XaccScoreUnit u = 0;   // weight - min free weight, in score units
    std::int64_t ur = 0;   // u / gcd, the normalised loss unit
};

struct Candidate {
    bool valid = false;
    XaccScoreUnit score = 0;
    XaccCostUnit cost = 0;
    long long non_base = 0;
    XaccScoreUnit distance = 0;
    std::vector<long long> counts;
};

// Final tie-break (tgb.md 4.3): two judgements with the same weight and the same
// difficulty coefficient are interchangeable as far as score and cost go, so the
// remaining ordering must still be deterministic and identical for every solver.
// We concentrate the counts on the judgements listed first in jd_keys().
bool counts_prefer_earlier(const std::vector<long long>& a, const std::vector<long long>& b) {
    const std::size_t count = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return a.size() > b.size();
}

// The single ordering used everywhere (tgb.md 4.3).  Integer comparisons only.
bool candidate_better(const Candidate& a, const Candidate& b) {
    if (!b.valid) return true;
    if (a.distance != b.distance) return a.distance < b.distance;
    if (a.cost != b.cost) return a.cost < b.cost;
    if (a.non_base != b.non_base) return a.non_base < b.non_base;
    return counts_prefer_earlier(a.counts, b.counts);
}

class Search {
public:
    Search(const XaccModel& model, const XaccTarget& target, XaccCount total,
           const std::map<std::string, long long>& fixed, bool xperfect, const XaccReverseOptions& options)
        : model_(model), target_(target), total_(total), options_(options), keys_(jd_keys(xperfect)),
          fixed_in_(fixed), xperfect_(xperfect) {}

    XaccReverseResult run();

private:
    XaccReverseResult fail(XaccStatus status, const std::string& message) const;
    bool prepare(std::string& error);
    XaccReverseResult build_result(const Candidate& best, bool optimal, XaccSolver solver, long long states,
                                   long long cells);
    XaccReverseResult run_dense_dp();
    XaccReverseResult run_branch_and_bound();
    XaccReverseResult run_legacy();
    long double estimate_cells() const;

    void seed_incumbent();
    void seed_counts(const std::vector<long long>& counts);
    void dfs(int position, long long remaining, long long accumulated_ur, XaccCostUnit accumulated_cost,
             long long non_base);
    void evaluate_leaf(long long final_ur, XaccCostUnit accumulated_cost, long long non_base);
    long long branch_distance(long long lo, long long hi, long long accumulated_ur, std::int64_t congruence) const;
    bool lp_cost_bound(long long remaining, long double needed_ur, long double& out) const;

    const XaccModel& model_;
    XaccTarget target_;
    XaccCount total_ = 0;
    XaccReverseOptions options_;
    std::vector<std::string> keys_;
    std::map<std::string, long long> fixed_in_;
    bool xperfect_ = false;

    std::map<std::string, long long> fixed_counts_;
    std::vector<Item> items_;
    XaccCount free_notes_ = 0;
    XaccScoreUnit fixed_score_ = 0;
    XaccCostUnit fixed_cost_ = 0;
    XaccScoreUnit target_units_ = 0;
    XaccScoreUnit window_low_ = 0;
    XaccScoreUnit window_high_ = 0;
    XaccScoreUnit min_weight_ = 0;
    XaccScoreUnit base_score_all_ = 0;  // score when every free note sits on the base item
    XaccScoreUnit target_loss_ = 0;     // target - base_score_all_: the score the free notes must add
    std::int64_t gcd_ = 1;
    std::int64_t max_ur_ = 0;
    int base_index_ = 0;

    // branch and bound state
    int n_ = 0;
    std::vector<int> branch_;
    std::vector<long long> assigned_;
    std::vector<std::int64_t> suffix_min_;
    std::vector<std::int64_t> suffix_max_;
    std::vector<std::int64_t> suffix_gcd_;
    std::map<std::tuple<int, long long, long long>, std::pair<XaccCostUnit, long long>> seen_;
    Candidate incumbent_;
    long long nodes_ = 0;
    long long pruned_ = 0;
    long long incumbent_updates_ = 0;
    bool budget_hit_ = false;
    std::chrono::steady_clock::time_point started_;
};

XaccReverseResult Search::fail(XaccStatus status, const std::string& message) const {
    XaccReverseResult result;
    result.status = status;
    result.message = message;
    result.elapsed_ms = seconds_since(started_) * 1000.0;
    return result;
}

// ------------------------------------------------------------ preparation --

bool Search::prepare(std::string& error) {
    if (total_ < 0) {
        error = "物量不能为负";
        return false;
    }
    for (const std::pair<const std::string, long long>& entry : fixed_in_) {
        if (entry.second < 0) {
            error = "固定数量不能为负: " + entry.first;
            return false;
        }
        if (std::find(keys_.begin(), keys_.end(), entry.first) == keys_.end()) {
            error = "当前模式下没有判定 " + entry.first;
            return false;
        }
    }
    if (!xacc_target_window(target_, model_, total_, window_low_, window_high_, error)) return false;
    if (!xacc_target_units(target_, model_, total_, target_units_, error)) return false;

    long long fixed_notes = 0;
    for (std::size_t index = 0; index < keys_.size(); ++index) {
        const std::string& key = keys_[index];
        auto it = fixed_in_.find(key);
        if (it == fixed_in_.end()) {
            Item item;
            item.key = key;
            item.key_index = static_cast<int>(index);
            item.weight = model_.weight(key);
            item.cost = model_.cost(key);
            items_.push_back(item);
            continue;
        }
        const long long count = it->second;
        if (!add_checked(fixed_notes, count, fixed_notes)) {
            error = "固定数量总和溢出";
            return false;
        }
        std::int64_t add = 0;
        if (!mul_checked(model_.weight(key), count, add) || !add_checked(fixed_score_, add, fixed_score_)) {
            error = "固定部分的分数溢出";
            return false;
        }
        if (!mul_checked(model_.cost(key), count, add) || !add_checked(fixed_cost_, add, fixed_cost_)) {
            error = "固定部分的难度系数溢出";
            return false;
        }
        fixed_counts_[key] = count;
    }
    if (fixed_notes > total_) {
        error = "固定数量总和超过物量";
        return false;
    }
    free_notes_ = total_ - fixed_notes;
    n_ = static_cast<int>(items_.size());
    if (free_notes_ > 0 && n_ == 0) {
        error = "没有可自由分配的判定";
        return false;
    }
    if (verbose_enabled()) {
        trace("模式 " + std::string(xperfect_ ? "XPerfect" : "普通") + "，物量 " + num(total_) + "（固定 " +
              num(fixed_notes) + "，自由 " + num(free_notes_) + "），可自由分配的判定 " + num(n_) + " 种");
        std::string fixed_text;
        for (const std::string& key : keys_) {
            auto it = fixed_counts_.find(key);
            if (it == fixed_counts_.end() || it->second == 0) continue;
            if (!fixed_text.empty()) fixed_text += ", ";
            fixed_text += key + "=" + num(it->second);
        }
        trace("固定判定: " + (fixed_text.empty() ? std::string("(无)") : fixed_text));
        trace("目标 " + xacc_format_acc(target_units_, total_, model_, target_.decimals) + "% → 分数单位 " +
              num(target_units_) + "，接受窗口 [" + num(window_low_) + ", " + num(window_high_) + "]");
    }
    return true;
}

long double Search::estimate_cells() const {
    if (n_ == 0 || max_ur_ <= 0) return 1.0L;
    if (free_notes_ > options_.max_dp_cells) return static_cast<long double>(options_.max_dp_cells) + 1.0L;
    long double cells = 0.0L;
    for (long long t = 1; t <= free_notes_; ++t) {
        cells += static_cast<long double>(t) * static_cast<long double>(max_ur_) + 1.0L;
        if (cells > static_cast<long double>(options_.max_dp_cells)) break;
    }
    return cells;
}

// -------------------------------------------------------------- reporting ---

XaccReverseResult Search::build_result(const Candidate& best, bool optimal, XaccSolver solver, long long states,
                                       long long cells) {
    XaccReverseResult result;
    result.status = XaccStatus::ok;
    result.cost_units = best.cost;
    result.score_units = best.score;
    result.actual_acc = xacc_acc_percent(best.score, total_, model_);
    result.exact = (best.score >= window_low_ && best.score <= window_high_);
    result.optimal = optimal;
    result.distance_units = best.distance;
    result.solver = solver;
    result.nodes = nodes_;
    result.pruned = pruned_;
    result.incumbent_updates = incumbent_updates_;
    result.states = states;
    result.cells = cells;
    result.elapsed_ms = seconds_since(started_) * 1000.0;
    for (const std::string& key : keys_) {
        auto it = fixed_counts_.find(key);
        if (it != fixed_counts_.end()) result.counts[key] = it->second;
    }
    for (int i = 0; i < n_; ++i) result.counts[items_[i].key] = best.counts[i];
    if (!result.exact) {
        result.message = "在 " + std::to_string(static_cast<long long>(total_)) + " 物量下无法精确到 " +
                         std::to_string(target_.decimals) + " 位小数，已返回最接近的组合";
    }
    if (!optimal) {
        result.note = "预算耗尽";
    }
    return result;
}

// ------------------------------------------------------------------ entry ---

XaccReverseResult Search::run() {
    started_ = std::chrono::steady_clock::now();

    if (verbose_enabled()) {
        trace("开始: 模式 " + std::string(xperfect_ ? "XPerfect" : "普通") + "，物量 " + num(total_) + "，目标 " +
              num(target_.numerator) + "/10^" + num(target_.decimals) + " 百分数，求解器选项 " +
              xacc_solver_text(options_.solver) + "，固定输入 " + num(static_cast<long long>(fixed_in_.size())) + " 项");
        for (const std::pair<const std::string, long long>& entry : fixed_in_) {
            trace("  固定 " + entry.first + " = " + num(entry.second));
        }
    }

    std::string error;
    if (!prepare(error)) {
        trace("输入无效: " + error);
        return fail(XaccStatus::invalid_input, error);
    }

    std::vector<long long> empty(n_, 0);
    if (free_notes_ == 0) {
        Candidate only;
        only.valid = true;
        only.counts = empty;
        only.score = fixed_score_;
        only.cost = fixed_cost_;
        only.non_base = 0;
        only.distance = static_cast<XaccScoreUnit>(std::llabs(fixed_score_ - target_units_));
        XaccSolver used = options_.solver == XaccSolver::automatic ? XaccSolver::exact_dp : options_.solver;
        XaccReverseResult result = build_result(only, true, used, 0, 0);
        if (!result.exact) result.message = "固定数量已完全确定分数，与目标不符";
        return result;
    }

    min_weight_ = items_[0].weight;
    XaccScoreUnit max_weight = items_[0].weight;
    for (const Item& item : items_) {
        min_weight_ = std::min(min_weight_, item.weight);
        max_weight = std::max(max_weight, item.weight);
    }

    std::int64_t span = 0;
    if (!mul_checked(free_notes_, min_weight_, span) || !add_checked(fixed_score_, span, span)) {
        return fail(XaccStatus::invalid_input, "分数范围溢出");
    }
    XaccScoreUnit reachable_low = span;
    if (!mul_checked(free_notes_, max_weight, span) || !add_checked(fixed_score_, span, span)) {
        return fail(XaccStatus::invalid_input, "分数范围溢出");
    }
    XaccScoreUnit reachable_high = span;
    if (reachable_high < window_low_ || reachable_low > window_high_) {
        trace("可达分数 [" + num(reachable_low) + ", " + num(reachable_high) + "] 与窗口无交集，直接判为不可达");
        return fail(XaccStatus::out_of_range,
                    "该物量下分数范围是 " + xacc_format_acc(reachable_low, total_, model_, target_.decimals) +
                        "% ~ " + xacc_format_acc(reachable_high, total_, model_, target_.decimals) +
                        "%，不可能达到目标");
    }
    trace("可达分数范围 [" + num(reachable_low) + ", " + num(reachable_high) + "]");

    std::int64_t divisor = 0;
    for (Item& item : items_) {
        item.u = item.weight - min_weight_;
        divisor = gcd_i64(divisor, item.u);
    }
    base_index_ = 0;
    for (int i = 1; i < n_; ++i) {
        const bool better = items_[i].u < items_[base_index_].u ||
                            (items_[i].u == items_[base_index_].u && items_[i].cost < items_[base_index_].cost);
        if (better) base_index_ = i;
    }

    if (!mul_checked(free_notes_, min_weight_, span) || !add_checked(fixed_score_, span, base_score_all_)) {
        return fail(XaccStatus::invalid_input, "分数范围溢出");
    }

    if (divisor <= 0) {
        // Every free judgement carries the same weight: the score is fixed, so
        // only the cheapest placement matters.
        int cheapest = 0;
        for (int i = 1; i < n_; ++i) {
            if (items_[i].cost < items_[cheapest].cost) cheapest = i;
        }
        Candidate only;
        only.valid = true;
        only.counts.assign(n_, 0);
        only.counts[cheapest] = free_notes_;
        only.score = base_score_all_;
        only.cost = fixed_cost_ + static_cast<XaccCostUnit>(free_notes_) * items_[cheapest].cost;
        only.non_base = free_notes_ - only.counts[base_index_];
        only.distance = static_cast<XaccScoreUnit>(std::llabs(only.score - target_units_));
        return build_result(only, true, XaccSolver::exact_dp, 0, 1);
    }

    gcd_ = divisor;
    max_ur_ = 0;
    for (Item& item : items_) {
        item.ur = item.u / gcd_;
        max_ur_ = std::max(max_ur_, item.ur);
    }
    // base_score_all_ is the *lowest* reachable score (every free note on the
    // minimum-weight judgement), so the free notes must ADD target_loss_ to reach
    // the target.  The branch-and-bound prunes in exactly these terms.
    target_loss_ = target_units_ - base_score_all_;

    const long double cells = estimate_cells();
    XaccSolver chosen = options_.solver;
    if (chosen == XaccSolver::automatic) {
        chosen = (cells <= static_cast<long double>(options_.max_dp_cells)) ? XaccSolver::exact_dp
                                                                           : XaccSolver::branch_and_bound;
    }
    if (chosen == XaccSolver::exact_dp && cells > static_cast<long double>(options_.max_dp_cells)) {
        return fail(XaccStatus::budget_exhausted,
                    "精确DP 需要约 " + std::to_string(static_cast<long long>(cells)) + " 个状态，超过预算 " +
                        std::to_string(options_.max_dp_cells) + "；请改用分支定界或减小物量");
    }

    if (verbose_enabled()) {
        trace("基准(最低分) = 全部自由音符放在最小权重判定上: " + num(base_score_all_) + "，需要补 " +
              num(target_loss_) + " 个分数单位");
        trace("分数步长 gcd = " + num(gcd_) + "，最大归一化步长 max_ur = " + num(max_ur_));
        for (const Item& item : items_) {
            trace("  判定 " + item.key + ": 权重 " + num(item.weight) + "，相对最低权重 u=" + num(item.u) +
                  "，归一化 ur=" + num(item.ur) + "，难度系数 " + num(item.cost) +
                  (item.key_index == base_index_ ? "  ← base" : ""));
        }
        trace("求解器 " + std::string(xacc_solver_text(chosen)) +
              (options_.solver == XaccSolver::automatic
                   ? "（自动选择：DP 估算格数 " + num(static_cast<long long>(cells)) + " vs 预算 " +
                         num(options_.max_dp_cells) + "）"
                   : "（用户指定）") +
              "；节点预算 " + num(options_.max_nodes) + "，时间预算 " + format_fixed(options_.max_seconds, 2) + " s");
    }

    switch (chosen) {
        case XaccSolver::legacy_map_dp: return run_legacy();
        case XaccSolver::branch_and_bound: return run_branch_and_bound();
        case XaccSolver::exact_dp:
        case XaccSolver::automatic: return run_dense_dp();
    }
    return fail(XaccStatus::invalid_input, "未知求解器");
}

// ------------------------------------------------------------ dense DP ------

XaccReverseResult Search::run_dense_dp() {
    struct Entry {
        XaccCostUnit cost = kCostInf;
        long long non_base = kNoteInf;
    };
    auto entry_less = [](const Entry& a, const Entry& b) {
        if (a.cost != b.cost) return a.cost < b.cost;
        return a.non_base < b.non_base;
    };

    // offsets[t] is the start of layer t; layer t spans t*max_ur_+1 entries.
    std::vector<std::size_t> offsets(static_cast<std::size_t>(free_notes_) + 2, 0);
    std::size_t total_cells = 0;
    for (long long t = 1; t <= free_notes_; ++t) {
        const std::size_t width = static_cast<std::size_t>(t) * static_cast<std::size_t>(max_ur_) + 1;
        offsets[static_cast<std::size_t>(t)] = total_cells;
        total_cells += width;
        if (total_cells > static_cast<std::size_t>(options_.max_dp_cells)) {
            return fail(XaccStatus::budget_exhausted, "精确DP 状态数超过预算");
        }
    }
    offsets[static_cast<std::size_t>(free_notes_) + 1] = total_cells;
    if (verbose_enabled()) {
        trace("精确DP: " + num(free_notes_) + " 层 × 每层最多 " + num(static_cast<long long>(max_ur_)) +
              " 步 → 总格数 " + num(static_cast<long long>(total_cells)) + "（预算 " +
              num(options_.max_dp_cells) + "），每格保留 (难度系数, 非base数) 字典序最小者");
    }

    std::vector<std::uint16_t> choice(total_cells, 0);
    std::vector<Entry> previous(1);
    previous[0] = Entry{0, 0};
    std::vector<Entry> current;

    for (long long t = 1; t <= free_notes_; ++t) {
        const std::size_t base = offsets[static_cast<std::size_t>(t)];
        const std::size_t width = offsets[static_cast<std::size_t>(t) + 1] - base;
        current.assign(width, Entry{});
        const std::size_t previous_width = previous.size();
        for (int i = 0; i < n_; ++i) {
            const std::size_t step = static_cast<std::size_t>(items_[i].ur);
            const XaccCostUnit step_cost = items_[i].cost;
            const long long step_non_base = (i == base_index_) ? 0 : 1;
            for (std::size_t k = 0; k < previous_width; ++k) {
                const Entry& source = previous[k];
                if (source.cost >= kCostInf) continue;
                const std::size_t target = k + step;
                if (target >= width) continue;
                Entry candidate;
                candidate.cost = source.cost + step_cost;
                candidate.non_base = source.non_base + step_non_base;
                if (entry_less(candidate, current[target])) {
                    current[target] = candidate;
                    choice[base + target] = static_cast<std::uint16_t>(i);
                }
            }
        }
        previous.swap(current);
    }

    // previous now holds the last layer.
    bool found = false;
    XaccScoreUnit best_distance = 0;
    XaccCostUnit best_cost = 0;
    long long best_non_base = 0;
    std::vector<long long> ties;
    for (std::size_t k = 0; k < previous.size(); ++k) {
        const Entry& entry = previous[k];
        if (entry.cost >= kCostInf) continue;
        std::int64_t loss = 0;
        if (!mul_checked(static_cast<std::int64_t>(k), gcd_, loss)) continue;
        std::int64_t score = 0;
        if (!add_checked(base_score_all_, loss, score)) continue;
        const XaccScoreUnit distance = static_cast<XaccScoreUnit>(std::llabs(score - target_units_));
        const XaccCostUnit cost = fixed_cost_ + entry.cost;
        const bool better = !found || distance < best_distance ||
                            (distance == best_distance &&
                             (cost < best_cost || (cost == best_cost && entry.non_base < best_non_base)));
        if (better) {
            found = true;
            best_distance = distance;
            best_cost = cost;
            best_non_base = entry.non_base;
            ties.clear();
            ties.push_back(static_cast<long long>(k));
        } else if (distance == best_distance && cost == best_cost && entry.non_base == best_non_base &&
                   ties.size() < kMaxLexTies) {
            ties.push_back(static_cast<long long>(k));
        }
    }
    if (!found) {
        return fail(XaccStatus::unreachable, "精确DP 没有找到任何可达分数");
    }

    Candidate best;
    for (long long tie : ties) {
        std::vector<long long> counts(static_cast<std::size_t>(n_), 0);
        long long k = tie;
        bool valid = true;
        for (long long t = free_notes_; t >= 1; --t) {
            const std::uint16_t index = choice[offsets[static_cast<std::size_t>(t)] + static_cast<std::size_t>(k)];
            counts[index] += 1;
            k -= items_[index].ur;
        }
        if (k != 0) valid = false;
        if (!valid) continue;
        Candidate candidate;
        candidate.valid = true;
        candidate.counts = counts;
        std::int64_t loss = 0;
        if (!mul_checked(tie, gcd_, loss) || !add_checked(base_score_all_, loss, candidate.score)) continue;
        candidate.cost = fixed_cost_;
        for (int i = 0; i < n_; ++i) {
            std::int64_t add = 0;
            if (!mul_checked(items_[i].cost, counts[i], add) || !add_checked(candidate.cost, add, candidate.cost)) {
                candidate.valid = false;
                break;
            }
        }
        if (!candidate.valid) continue;
        candidate.non_base = free_notes_ - counts[base_index_];
        candidate.distance = static_cast<XaccScoreUnit>(std::llabs(candidate.score - target_units_));
        if (candidate_better(candidate, best)) best = candidate;
    }
    if (!best.valid) return fail(XaccStatus::unreachable, "精确DP 回溯失败");

    return build_result(best, true, XaccSolver::exact_dp, 0, static_cast<long long>(total_cells));
}

// ------------------------------------------------------- branch and bound ---

long long Search::branch_distance(long long lo, long long hi, long long accumulated_ur,
                                  std::int64_t congruence) const {
    if (congruence <= 0) {
        std::int64_t value = 0;
        if (!mul_checked(accumulated_ur, gcd_, value)) return 0;
        const long long delta = static_cast<long long>(value - target_loss_);
        return delta < 0 ? -delta : delta;
    }
    const long long first = ceil_div_any(lo - accumulated_ur, congruence);
    const long long last = floor_div_any(hi - accumulated_ur, congruence);
    if (first > last) return std::numeric_limits<long long>::max() / 4;
    const long double target_ur = static_cast<long double>(target_loss_) / static_cast<long double>(gcd_);
    long long middle = static_cast<long long>(
        std::llround(static_cast<double>((target_ur - static_cast<long double>(accumulated_ur)) /
                                         static_cast<long double>(congruence))));
    middle = std::max(first, std::min(last, middle));
    long long best = std::numeric_limits<long long>::max() / 4;
    for (long long j = middle - 1; j <= middle + 1; ++j) {
        if (j < first || j > last) continue;
        std::int64_t value = 0;
        const long long ur = accumulated_ur + congruence * j;
        if (!mul_checked(ur, gcd_, value)) continue;
        const long long delta = static_cast<long long>(value - target_loss_);
        best = std::min(best, delta < 0 ? -delta : delta);
    }
    return best;
}

bool Search::lp_cost_bound(long long remaining, long double needed_ur, long double& out) const {
    bool found = false;
    long double best = 0.0L;
    for (int i = 0; i < n_; ++i) {
        const long double contributed = static_cast<long double>(items_[i].ur) * static_cast<long double>(remaining);
        if (std::fabs(contributed - needed_ur) <= kTieEpsilon) {
            const long double value = static_cast<long double>(remaining) * static_cast<long double>(items_[i].cost);
            if (!found || value < best) {
                best = value;
                found = true;
            }
        }
    }
    for (int i = 0; i < n_; ++i) {
        for (int j = i + 1; j < n_; ++j) {
            const long long difference = items_[i].ur - items_[j].ur;
            if (difference == 0) continue;
            const long double left =
                (needed_ur - static_cast<long double>(remaining) * static_cast<long double>(items_[j].ur)) /
                static_cast<long double>(difference);
            const long double right = static_cast<long double>(remaining) - left;
            if (left < -kTieEpsilon || right < -kTieEpsilon) continue;
            const long double value = std::max(0.0L, left) * static_cast<long double>(items_[i].cost) +
                                      std::max(0.0L, right) * static_cast<long double>(items_[j].cost);
            if (!found || value < best) {
                best = value;
                found = true;
            }
        }
    }
    if (!found) return false;
    out = best;
    return true;
}

void Search::seed_counts(const std::vector<long long>& counts) {
    Candidate candidate;
    candidate.valid = true;
    candidate.counts = counts;
    candidate.cost = fixed_cost_;
    candidate.score = fixed_score_;
    for (int i = 0; i < n_; ++i) {
        std::int64_t add = 0;
        if (!mul_checked(items_[i].weight, counts[i], add) || !add_checked(candidate.score, add, candidate.score)) {
            candidate.valid = false;
            break;
        }
        if (!mul_checked(items_[i].cost, counts[i], add) || !add_checked(candidate.cost, add, candidate.cost)) {
            candidate.valid = false;
            break;
        }
    }
    if (!candidate.valid) return;
    candidate.non_base = free_notes_ - counts[base_index_];
    candidate.distance = static_cast<XaccScoreUnit>(std::llabs(candidate.score - target_units_));
    if (candidate_better(candidate, incumbent_)) {
        incumbent_ = candidate;
        ++incumbent_updates_;
    }
}

void Search::seed_incumbent() {
    // A quick feasible solution makes the pruning effective from the first node
    // (tgb.md 5B).  At most two judgement types are used.
    std::vector<long long> counts(static_cast<std::size_t>(n_), 0);
    for (int i = 0; i < n_; ++i) {
        std::fill(counts.begin(), counts.end(), 0);
        counts[i] = free_notes_;
        seed_counts(counts);
    }
    const long double target_ur = static_cast<long double>(target_loss_) / static_cast<long double>(gcd_);
    for (int i = 0; i < n_; ++i) {
        for (int j = 0; j < n_; ++j) {
            if (i == j) continue;
            const long long difference = items_[i].ur - items_[j].ur;
            long long guess = 0;
            if (difference == 0) {
                guess = free_notes_ / 2;
            } else {
                const long double raw = (target_ur - static_cast<long double>(free_notes_) *
                                                          static_cast<long double>(items_[j].ur)) /
                                        static_cast<long double>(difference);
                if (raw < -4.0L || raw > static_cast<long double>(free_notes_) + 4.0L) continue;
                guess = static_cast<long long>(std::llround(static_cast<double>(raw)));
            }
            for (long long delta = -2; delta <= 2; ++delta) {
                const long long first = guess + delta;
                if (first < 0 || first > free_notes_) continue;
                std::fill(counts.begin(), counts.end(), 0);
                counts[i] = first;
                counts[j] = free_notes_ - first;
                seed_counts(counts);
            }
        }
    }
}

void Search::evaluate_leaf(long long final_ur, XaccCostUnit accumulated_cost, long long non_base) {
    std::vector<long long> counts(static_cast<std::size_t>(n_), 0);
    for (int p = 0; p < n_; ++p) counts[branch_[p]] += assigned_[p];
    std::int64_t loss = 0;
    if (!mul_checked(final_ur, gcd_, loss)) return;
    Candidate candidate;
    candidate.valid = true;
    candidate.counts = counts;
    if (!add_checked(base_score_all_, loss, candidate.score)) return;
    candidate.cost = accumulated_cost;
    candidate.non_base = non_base;
    candidate.distance = static_cast<XaccScoreUnit>(std::llabs(candidate.score - target_units_));
    if (candidate_better(candidate, incumbent_)) {
        const bool closer = !incumbent_.valid || candidate.distance < incumbent_.distance;
        incumbent_ = candidate;
        ++incumbent_updates_;
        // Log every step towards the target (rare and always interesting) plus the
        // early history of cheap-but-equal-distance improvements.
        if (verbose_enabled() && (closer || incumbent_updates_ <= 20 || (incumbent_updates_ % 1000) == 0)) {
            trace("节点 " + num(nodes_) + " 上界更新 #" + num(incumbent_updates_) + ": 距离 " +
                  num(candidate.distance) + "，难度系数 " + num(candidate.cost) +
                  (closer ? "  ← 更接近目标" : ""));
        }
    }
}

void Search::dfs(int position, long long remaining, long long accumulated_ur, XaccCostUnit accumulated_cost,
                 long long non_base) {
    ++nodes_;
    const bool over_nodes = nodes_ > options_.max_nodes;
    const bool over_time = (nodes_ & 0x3FF) == 0 && seconds_since(started_) > options_.max_seconds;
    if (over_nodes || over_time) {
        budget_hit_ = true;
        return;
    }
    if (position >= n_) return;

    if (position == n_ - 1) {
        assigned_[position] = remaining;
        const Item& last = items_[branch_[position]];
        std::int64_t add = 0;
        if (!mul_checked(last.cost, remaining, add)) {
            budget_hit_ = true;
            return;
        }
        const long long extra = (branch_[position] == base_index_) ? 0 : remaining;
        evaluate_leaf(accumulated_ur + remaining * last.ur, accumulated_cost + add, non_base + extra);
        return;
    }

    const long double needed_ur = static_cast<long double>(target_loss_) / static_cast<long double>(gcd_) -
                                  static_cast<long double>(accumulated_ur);

    // Dominance: the same (position, remaining, accumulated_ur) means the same
    // future, so an earlier visit with a strictly lower cost wins outright.
    const std::tuple<int, long long, long long> key(position, remaining, accumulated_ur);
    auto seen = seen_.find(key);
    if (seen != seen_.end() && seen->second.first < accumulated_cost && seen->second.second <= non_base) {
        ++pruned_;
        return;
    }
    if (seen == seen_.end()) {
        if (seen_.size() < kMaxDominanceEntries) seen_[key] = std::make_pair(accumulated_cost, non_base);
    } else if (accumulated_cost < seen->second.first) {
        // Remember the cheapest visit seen so far; the same key has the same
        // future, so anything strictly dearer is dominated by it.
        seen->second = std::make_pair(accumulated_cost, non_base);
    }

    const std::int64_t min_ur = suffix_min_[static_cast<std::size_t>(position)];
    const std::int64_t max_ur = suffix_max_[static_cast<std::size_t>(position)];
    const std::int64_t congruence = suffix_gcd_[static_cast<std::size_t>(position)];
    const long long reachable_first = accumulated_ur + remaining * min_ur;
    const long long reachable_last = accumulated_ur + remaining * max_ur;
    const long long best_guess = branch_distance(reachable_first, reachable_last, accumulated_ur, congruence);
    const long long allowed = incumbent_.valid ? incumbent_.distance : std::numeric_limits<long long>::max() / 4;
    if (best_guess > allowed) {
        ++pruned_;
        return;
    }
    if (best_guess == 0 && incumbent_.valid && incumbent_.distance == 0) {
        long double bound = 0.0L;
        if (lp_cost_bound(remaining, needed_ur, bound) &&
            static_cast<long double>(accumulated_cost) + bound > static_cast<long double>(incumbent_.cost)) {
            ++pruned_;
            return;
        }
    }

    const Item& item = items_[branch_[position]];
    const std::int64_t next_min = suffix_min_[static_cast<std::size_t>(position) + 1];
    const std::int64_t next_max = suffix_max_[static_cast<std::size_t>(position) + 1];

    long long low = 0;
    long long high = remaining;
    // lo(n) = accumulated + n*ur + (remaining-n)*next_min must be able to reach the
    // target window widened by the incumbent distance, and so must hi(n).  Both
    // are linear in n, so each condition turns into one integer bound.  Anything
    // that would overflow simply skips that bound (the search stays correct,
    // only the pruning gets weaker).
    std::int64_t low_slope = 0;
    std::int64_t high_slope = 0;
    std::int64_t shared = 0;
    std::int64_t low_base = 0;
    std::int64_t high_base = 0;
    std::int64_t upper_target = 0;
    std::int64_t lower_target = 0;
    bool can_bound = mul_checked(gcd_, item.ur - next_min, low_slope) &&
                     mul_checked(gcd_, item.ur - next_max, high_slope) &&
                     mul_checked(gcd_, accumulated_ur, shared) && (low_base = shared, true) &&
                     (high_base = shared, true) &&
                     mul_checked(gcd_, remaining, shared) &&
                     mul_checked(shared, next_min, shared) && add_checked(low_base, shared, low_base) &&
                     mul_checked(gcd_, remaining, shared) &&
                     mul_checked(shared, next_max, shared) && add_checked(high_base, shared, high_base) &&
                     add_checked(target_loss_, allowed, upper_target) &&
                     sub_checked(target_loss_, allowed, lower_target);

    if (can_bound) {
        auto apply_le = [&low, &high](long long slope, long long base, long long bound) {
            std::int64_t difference = 0;
            if (!sub_checked(bound, base, difference)) return;
            if (slope == 0) {
                if (difference < 0) {
                    low = 1;
                    high = 0;
                }
                return;
            }
            if (slope > 0) {
                const long long limit = floor_div_any(difference, slope);
                if (limit < high) high = limit;
            } else {
                const long long limit = ceil_div_any(difference, slope);
                if (limit > low) low = limit;
            }
        };
        apply_le(low_slope, low_base, upper_target);
        std::int64_t neg_high_slope = 0;
        std::int64_t neg_high_base = 0;
        std::int64_t neg_lower_target = 0;
        if (sub_checked(0, high_slope, neg_high_slope) && sub_checked(0, high_base, neg_high_base) &&
            sub_checked(0, lower_target, neg_lower_target)) {
            apply_le(neg_high_slope, neg_high_base, neg_lower_target);
        }
    }

    if (low < 0) low = 0;
    if (high > remaining) high = remaining;
    if (low > high) {
        ++pruned_;
        return;
    }

    // Branch order (tgb.md 5C): try the closed-form estimate first.  Placing
    // needed_ur / ur_i notes on this judgement is the cheapest way to reach the
    // target when this judgement is the cheap one, so the good incumbents appear
    // early instead of after an exhaustive sweep of the hopeless counts.
    const long long span = high - low;
    long long center = low;
    if (item.ur > 0) {
        const long double ratio = needed_ur / static_cast<long double>(item.ur);
        long long guess = low;
        if (ratio >= static_cast<long double>(high)) {
            guess = high;
        } else if (ratio > static_cast<long double>(low)) {
            guess = static_cast<long long>(std::llround(static_cast<double>(ratio)));
        }
        if (guess < low) guess = low;
        if (guess > high) guess = high;
        center = guess;
    } else {
        center = high;
    }

    for (long long step = 0; step <= span; ++step) {
        const long long takes[2] = {center - step, center + step};
        for (int which = 0; which < 2; ++which) {
            if (step == 0 && which == 1) continue;
            const long long take = takes[which];
            if (take < low || take > high) continue;
            assigned_[position] = take;
            std::int64_t add = 0;
            if (!mul_checked(item.cost, take, add)) {
                budget_hit_ = true;
                return;
            }
            dfs(position + 1, remaining - take, accumulated_ur + take * item.ur, accumulated_cost + add,
                non_base + ((branch_[position] == base_index_) ? 0 : take));
            if (budget_hit_) return;
        }
    }
}

XaccReverseResult Search::run_branch_and_bound() {
    branch_.resize(static_cast<std::size_t>(n_));
    for (int i = 0; i < n_; ++i) branch_[static_cast<std::size_t>(i)] = i;
    std::sort(branch_.begin(), branch_.end(), [this](int a, int b) {
        if (items_[a].ur != items_[b].ur) return items_[a].ur > items_[b].ur;
        if (items_[a].cost != items_[b].cost) return items_[a].cost < items_[b].cost;
        return items_[a].key_index < items_[b].key_index;
    });
    assigned_.assign(static_cast<std::size_t>(n_), 0);
    suffix_min_.assign(static_cast<std::size_t>(n_) + 1, std::numeric_limits<std::int64_t>::max());
    suffix_max_.assign(static_cast<std::size_t>(n_) + 1, std::numeric_limits<std::int64_t>::min());
    suffix_gcd_.assign(static_cast<std::size_t>(n_) + 1, 0);
    for (int p = n_ - 1; p >= 0; --p) {
        const std::int64_t ur = items_[branch_[static_cast<std::size_t>(p)]].ur;
        suffix_min_[static_cast<std::size_t>(p)] =
            std::min(ur, suffix_min_[static_cast<std::size_t>(p) + 1]);
        suffix_max_[static_cast<std::size_t>(p)] =
            std::max(ur, suffix_max_[static_cast<std::size_t>(p) + 1]);
        suffix_gcd_[static_cast<std::size_t>(p)] =
            gcd_i64(suffix_gcd_[static_cast<std::size_t>(p) + 1], ur);
    }

    seed_incumbent();
    if (verbose_enabled()) {
        if (incumbent_.valid) {
            trace("两类型闭式解给出的初始上界: 距离 " + num(incumbent_.distance) + "，难度系数 " +
                  num(incumbent_.cost) + (incumbent_.distance == 0 ? "" : ""));
        }
        std::string order_text;
        for (std::size_t i = 0; i < branch_.size(); ++i) {
            if (i != 0) order_text += " → ";
            const Item& item = items_[static_cast<std::size_t>(branch_[i])];
            order_text += item.key + "(ur=" + num(item.ur) + ",cost=" + num(item.cost) + ")";
        }
        trace("分支顺序: " + order_text);
    }
    dfs(0, free_notes_, 0, fixed_cost_, 0);

    if (!incumbent_.valid) {
        if (budget_hit_) {
            trace("预算耗尽且没有找到任何可行组合: 节点 " + num(nodes_) + "，剪枝 " + num(pruned_));
            return fail(XaccStatus::budget_exhausted, "预算内没有找到可行组合");
        }
        trace("搜索完成但没有可行组合");
        return fail(XaccStatus::unreachable, "没有可行的判定分布");
    }
    if (budget_hit_) {
        trace("预算耗尽（结果未证明最优）: 节点 " + num(nodes_) + "，剪枝 " + num(pruned_) + "，上界更新 " +
              num(incumbent_updates_) + " 次，用时 " + format_fixed(seconds_since(started_) * 1000.0, 2) + " ms");
    } else {
        trace("搜索空间已穷尽（结果已证明最优）: 节点 " + num(nodes_) + "，剪枝 " + num(pruned_) + "，上界更新 " +
              num(incumbent_updates_) + " 次，用时 " + format_fixed(seconds_since(started_) * 1000.0, 2) + " ms");
    }
    return build_result(incumbent_, !budget_hit_, XaccSolver::branch_and_bound, 0, 0);
}

// --------------------------------------------------------- legacy map DP ----

XaccReverseResult Search::run_legacy() {
    int base = 0;
    for (int i = 1; i < n_; ++i) {
        const bool better = items_[i].weight > items_[base].weight ||
                            (items_[i].weight == items_[base].weight && items_[i].cost < items_[base].cost);
        if (better) base = i;
    }
    const XaccScoreUnit base_weight = items_[base].weight;
    const XaccCostUnit base_cost = items_[base].cost;

    struct LossItem {
        int item = 0;
        XaccScoreUnit loss = 0;
        XaccCostUnit delta = 0;
    };
    std::vector<LossItem> loss_items;
    {
        std::map<XaccScoreUnit, int> best_by_loss;
        for (int i = 0; i < n_; ++i) {
            if (i == base) continue;
            const XaccScoreUnit loss = base_weight - items_[i].weight;
            if (loss <= 0) continue;
            auto it = best_by_loss.find(loss);
            if (it == best_by_loss.end() || items_[i].cost < items_[it->second].cost) best_by_loss[loss] = i;
        }
        for (const std::pair<const XaccScoreUnit, int>& entry : best_by_loss) {
            LossItem loss_item;
            loss_item.item = entry.second;
            loss_item.loss = entry.first;
            loss_item.delta = items_[entry.second].cost - base_cost;
            loss_items.push_back(loss_item);
        }
    }

    const XaccScoreUnit base_total = fixed_score_ + static_cast<XaccScoreUnit>(free_notes_) * base_weight;
    const XaccScoreUnit ideal = base_total - target_units_;

    if (loss_items.empty()) {
        std::vector<long long> counts(static_cast<std::size_t>(n_), 0);
        counts[base] = free_notes_;
        Candidate only;
        only.valid = true;
        only.counts = counts;
        only.score = base_total;
        only.cost = fixed_cost_ + static_cast<XaccCostUnit>(free_notes_) * base_cost;
        only.non_base = free_notes_ - counts[base_index_];
        only.distance = static_cast<XaccScoreUnit>(std::llabs(only.score - target_units_));
        return build_result(only, true, XaccSolver::legacy_map_dp, 0, 0);
    }

    // The original widened the typed window by 0.4% and capped it at 4096 units
    // of its 20-per-weight grid; the same half width in model units keeps the
    // fallback comparable.
    std::int64_t denominator = 0;
    if (!mul_checked(200, pow10_local(target_.decimals), denominator)) {
        return fail(XaccStatus::invalid_input, "精度过大");
    }
    std::int64_t product = 0;
    if (!mul_checked(total_, model_.score_scale, product)) {
        return fail(XaccStatus::invalid_input, "目标与物量相乘溢出");
    }
    const std::int64_t half_exact = ceil_div_i64(product, denominator);
    const std::int64_t slack = static_cast<std::int64_t>(model_.score_scale) * 4 / 10;
    std::int64_t cap = 0;
    if (!mul_checked(4096, model_.score_scale, cap)) return fail(XaccStatus::invalid_input, "权重精度过大");
    cap /= 20;
    const std::int64_t half = std::min(std::max(half_exact, slack), cap);

    XaccScoreUnit loss_min = base_total - (target_units_ + static_cast<XaccScoreUnit>(half));
    if (loss_min < 0) loss_min = 0;
    const XaccScoreUnit loss_max = base_total - (target_units_ - static_cast<XaccScoreUnit>(half));
    if (loss_max < 0) {
        return fail(XaccStatus::unreachable, "最高可达分数也低于目标区间");
    }

    struct State {
        XaccCostUnit delta_cost = 0;
        std::vector<long long> layout;
    };
    std::map<XaccScoreUnit, State> current;
    current[0] = State{0, std::vector<long long>(loss_items.size(), 0)};

    Candidate best;
    long long states = 1;
    bool budget = false;

    if (verbose_enabled()) {
        trace("旧版 map DP: base=" + items_[base].key + "（最高权重 " + num(base_weight) + "，取难度系数最低者），可用替换 " +
              num(static_cast<long long>(loss_items.size())) + " 种");
        for (const LossItem& loss_item : loss_items) {
            trace("  替换 " + items_[loss_item.item].key + ": 损失 " + num(loss_item.loss) + "，相对 base 的难度系数差 " +
                  num(loss_item.delta));
        }
        trace("损失窗口 [loss_min, loss_max] = [" + num(loss_min) + ", " + num(loss_max) + "]，容差 half=" + num(half) +
              "（精确半宽 " + num(half_exact) + "，下限 " + num(slack) + "，上限 " + num(cap) + "）");
    }

    for (long long step = 0; step < free_notes_; ++step) {
        std::map<XaccScoreUnit, State> next_states;
        for (const std::pair<const XaccScoreUnit, State>& entry : current) {
            for (std::size_t i = 0; i < loss_items.size(); ++i) {
                const XaccScoreUnit new_loss = entry.first + loss_items[i].loss;
                if (new_loss > loss_max) continue;
                const XaccCostUnit new_cost = entry.second.delta_cost + loss_items[i].delta;
                auto it = next_states.find(new_loss);
                if (it == next_states.end() || new_cost < it->second.delta_cost) {
                    State state;
                    state.delta_cost = new_cost;
                    state.layout = entry.second.layout;
                    state.layout[i] += 1;
                    next_states[new_loss] = std::move(state);
                }
            }
        }
        if (next_states.empty()) break;
        const long long used = step + 1;
        for (const std::pair<const XaccScoreUnit, State>& entry : next_states) {
            if (entry.first < loss_min) continue;
            const XaccCostUnit candidate_cost = fixed_cost_ + static_cast<XaccCostUnit>(free_notes_) * base_cost +
                                                entry.second.delta_cost;
            const XaccScoreUnit distance =
                static_cast<XaccScoreUnit>(std::llabs(entry.first - ideal));
            const bool better = !best.valid || distance < best.distance ||
                                (distance == best.distance && candidate_cost < best.cost);
            if (better) {
                std::vector<long long> counts(static_cast<std::size_t>(n_), 0);
                for (std::size_t i = 0; i < loss_items.size(); ++i) counts[loss_items[i].item] += entry.second.layout[i];
                counts[base] += free_notes_ - used;
                best.valid = true;
                best.counts = counts;
                best.cost = candidate_cost;
                best.distance = distance;
                best.non_base = free_notes_ - counts[base_index_];
            }
        }
        states = static_cast<long long>(next_states.size());
        if (states > kMaxLegacyStates) {
            budget = true;
            trace("旧版 map DP 状态数 " + num(states) + " 超过预算 " + num(kMaxLegacyStates) + "，在第 " + num(used) +
                  " 层停止");
            break;
        }
        current.swap(next_states);
    }
    trace("旧版 map DP 结束: 最后一层状态数 " + num(states) + "，用时 " + format_fixed(seconds_since(started_) * 1000.0, 2) +
          " ms");

    if (!best.valid) {
        if (budget) return fail(XaccStatus::budget_exhausted, "旧版DP 状态数超过预算，未找到落在目标区间内的组合");
        return fail(XaccStatus::unreachable, "旧版DP 没有找到落在目标区间内的分数损失");
    }
    best.score = fixed_score_;
    best.cost = fixed_cost_;
    for (int i = 0; i < n_; ++i) {
        std::int64_t add = 0;
        if (!mul_checked(items_[i].weight, best.counts[i], add) || !add_checked(best.score, add, best.score)) {
            return fail(XaccStatus::invalid_input, "分数溢出");
        }
        if (!mul_checked(items_[i].cost, best.counts[i], add) || !add_checked(best.cost, add, best.cost)) {
            return fail(XaccStatus::invalid_input, "难度系数溢出");
        }
    }
    best.distance = static_cast<XaccScoreUnit>(std::llabs(best.score - target_units_));

    XaccReverseResult result = build_result(best, !budget, XaccSolver::legacy_map_dp, states, 0);
    const std::string legacy_note = "旧版 map DP：同损失只保留最低成本判定，不保证全局最优";
    result.note = result.note.empty() ? legacy_note : result.note + "；" + legacy_note;
    return result;
}

}  // namespace

XaccReverseResult xacc_reverse_search(const XaccModel& model, const XaccTarget& target, XaccCount total,
                                      const std::map<std::string, long long>& fixed_counts, bool xperfect,
                                      const XaccReverseOptions& options) {
    Search search(model, target, total, fixed_counts, xperfect, options);
    XaccReverseResult result = search.run();
    if (verbose_enabled()) {
        trace("结果: " + std::string(xacc_status_text(result.status)) + "，求解器 " +
              xacc_solver_text(result.solver) + "，分数 " + num(result.score_units) + "，相差 " +
              num(result.distance_units) + " 个分数单位，难度系数合计 " + num(result.cost_units));
        std::string counts_text;
        for (const std::pair<const std::string, long long>& entry : result.counts) {
            if (entry.second == 0) continue;
            if (!counts_text.empty()) counts_text += ", ";
            counts_text += entry.first + "=" + num(entry.second);
        }
        trace("组合: " + (counts_text.empty() ? std::string("(空)") : counts_text));
        trace("统计: 节点 " + num(result.nodes) + "，剪枝 " + num(result.pruned) + "，上界更新 " +
              num(result.incumbent_updates) + "，状态 " + num(result.states) + "，格数 " + num(result.cells) +
              "，用时 " + format_fixed(result.elapsed_ms, 3) + " ms");
        if (!result.message.empty()) trace("提示: " + result.message);
        if (!result.note.empty()) trace("备注: " + result.note);
    }
    return result;
}

}  // namespace tuf
