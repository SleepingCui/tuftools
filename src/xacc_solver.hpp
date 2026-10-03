// Exact XACC reverse search.
//
// The previous implementation converted each weight with `round(weight * 20)`
// and ran a std::map DP over absolute score loss, so it disagreed with the
// forward calculator for weights that are not multiples of 0.05 and its state
// space grew with the total loss (a 100k note run at 0% XACC meant ~2,000,000
// loss units).  This file keeps the same objective -- closest score to the
// target, then cheapest, then fewest non-base judgements -- but on integer
// units, with three solvers:
//
//   * exact_dp         dense array DP, provably optimal, small inputs only
//   * branch_and_bound depth first search with interval / congruence / LP cost
//                      pruning and an explicit node and time budget
//   * legacy_map_dp    the previous map DP, kept selectable for comparison
//
// See tgb.md sections 4 to 6.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "xacc_model.hpp"

namespace tuf {

struct XaccReverseOptions {
    XaccSolver solver = XaccSolver::automatic;
    // Observable budgets (tgb.md 5E).  Exceeding one never changes the answer
    // silently: the result is either flagged as approximate or reports that the
    // budget ran out.
    long long max_nodes = 20000000;
    long long max_dp_cells = 8000000;
    double max_seconds = 5.0;
};

struct XaccReverseResult {
    XaccStatus status = XaccStatus::ok;
    std::string message;

    // Full judgement distribution over the mode's keys; fixed judgements keep
    // the counts the caller asked for.
    std::map<std::string, long long> counts;
    XaccScoreUnit score_units = 0;
    double actual_acc = 0.0;
    bool exact = false;    // true when the score prints as the typed target
    bool optimal = true;   // false when a budget cut the search short
    XaccScoreUnit distance_units = 0;  // |score - target| in score units
    XaccCostUnit cost_units = 0;

    XaccSolver solver = XaccSolver::automatic;  // solver that actually ran
    long long nodes = 0;
    long long pruned = 0;
    long long incumbent_updates = 0;
    long long states = 0;
    long long cells = 0;
    double elapsed_ms = 0.0;
    std::string note;

    bool ok() const { return status == XaccStatus::ok; }
};

XaccReverseResult xacc_reverse_search(const XaccModel& model, const XaccTarget& target, XaccCount total,
                                      const std::map<std::string, long long>& fixed_counts, bool xperfect,
                                      const XaccReverseOptions& options = XaccReverseOptions());

}  // namespace tuf
