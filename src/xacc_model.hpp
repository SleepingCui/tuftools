// XACC model: the exact integer contract behind the XACC calculator and the
// XACC reverse search.
//
// The old code kept weights/costs as doubles and converted weights with
// `round(weight * 20)` inside the reverse search only.  That produced two
// disagreeing answers for any weight that is not a multiple of 0.05 (the
// forward calculator used the double, the reverse search used the rounded
// integer).  Everything here is exact: one scale per quantity, integer units
// end to end, and one place that validates the configuration.
//
// See tgb.md (sections 4 and 6) for the reasoning.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tuf {

using XaccCount = std::int64_t;
using XaccScoreUnit = std::int64_t;
using XaccCostUnit = std::int64_t;

// Why a reverse search produced no usable answer.  tgb.md 4.2 requires the
// caller to tell "bad input" from "physically unreachable" from "gave up"
// instead of printing one generic "cannot be done".
enum class XaccStatus {
    ok,
    invalid_input,
    invalid_config,
    out_of_range,
    unreachable,
    budget_exhausted,
};

const char* xacc_status_text(XaccStatus status);

// Which reverse solver to run.  `legacy_map_dp` is the previous std::map DP,
// kept selectable so its answers can be compared and so there is a fallback.
enum class XaccSolver {
    automatic,
    exact_dp,
    branch_and_bound,
    legacy_map_dp,
};

const char* xacc_solver_text(XaccSolver solver);
bool xacc_solver_from_text(const std::string& text, XaccSolver& out);
const std::vector<XaccSolver>& xacc_solver_all();

// Overflow-checked integer helpers.  They avoid __int128 so the MSVC build works
// too; every multiplication of a user-supplied magnitude goes through these.
bool mul_checked(std::int64_t a, std::int64_t b, std::int64_t& out);
bool add_checked(std::int64_t a, std::int64_t b, std::int64_t& out);
std::int64_t gcd_i64(std::int64_t a, std::int64_t b);
// a / b rounded towards positive infinity, for b > 0.
std::int64_t ceil_div_i64(std::int64_t a, std::int64_t b);
// a / b rounded to nearest, for b > 0.
std::int64_t round_div_i64(std::int64_t a, std::int64_t b);

// Score unit:  weight = weight_units / score_scale.  An XACC run of `total`
// notes scores sum(count_i * weight_units_i) / (total * score_scale).
// Cost unit: cost = cost_units / cost_scale; only ever compared, never divided.
struct XaccModel {
    XaccScoreUnit score_scale = 1000;
    XaccCostUnit cost_scale = 1000;
    std::vector<std::string> order;  // every key, in costs.json / menu order
    std::map<std::string, XaccScoreUnit> weight_units;
    std::map<std::string, XaccCostUnit> cost_units;

    bool has(const std::string& key) const { return weight_units.find(key) != weight_units.end(); }
    XaccScoreUnit weight(const std::string& key) const;
    XaccCostUnit cost(const std::string& key) const;
};

// Configuration file (schemaVersion 2), holding both tables as integers.  When
// only the legacy costs.json / weights.json exist they are migrated -- but only
// if every value lands exactly on the configured precision; anything ambiguous
// is reported by key instead of being silently rounded.  Returns false and
// fills `error` when the configuration cannot be used.
bool xacc_model_load(XaccModel& model, std::string& error, std::string* note = nullptr);
bool xacc_model_save(const XaccModel& model, std::string& error);
// Decimal value -> units.  `exact` reports whether the value lands on the unit
// grid; the migration path refuses inexact values, the editor rounds and says so.
bool xacc_units_from_value(double value, std::int64_t scale, std::int64_t& out, bool& exact);

// The XACC the user typed, kept as a rational: value = numerator / 10^decimals
// (percent).  "99.28" -> decimals 2, numerator 9928.  Parsed from the raw text
// so no double ever sees the target.
struct XaccTarget {
    int decimals = 2;
    std::int64_t numerator = 0;
};

bool xacc_parse_target(const std::string& text, XaccTarget& out, std::string& error);

// Accept window [slo, shi] in score units: every integer score unit that prints
// as the typed percent at the typed precision.  Half open on the high side.
bool xacc_target_window(const XaccTarget& target, const XaccModel& model, XaccCount total,
                        XaccScoreUnit& slo, XaccScoreUnit& shi, std::string& error);

// Score unit nearest to the typed target; the distance tie-break uses this.
bool xacc_target_units(const XaccTarget& target, const XaccModel& model, XaccCount total,
                       XaccScoreUnit& out, std::string& error);

// One formatter for both directions (tgb.md 6.4): the menu no longer recomputes
// a result with its own rule.
double xacc_acc_percent(XaccScoreUnit score_units, XaccCount total, const XaccModel& model);
std::string xacc_format_acc(XaccScoreUnit score_units, XaccCount total, const XaccModel& model,
                            int decimals);
std::string xacc_format_weight(const XaccModel& model, const std::string& key);
std::string xacc_format_cost(const XaccModel& model, const std::string& key);

}  // namespace tuf
