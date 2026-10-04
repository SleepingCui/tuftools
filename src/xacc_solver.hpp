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
    // budget ran out.  The menu fills these from XaccModel, so these are only the
    // fallback for callers that build the options themselves.
    long long max_nodes = kDefaultMaxNodes;
    long long max_dp_cells = kDefaultMaxDpCells;
    double max_seconds = kDefaultMaxSeconds;
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

// ------------------------------------------------------ equivalent set ------
//
// Every combination that ties the optimum on the ordering used by the search
// (distance, then cost, then non-base count).  These all yield the same printed
// XACC and the same difficulty total, so the search picks one arbitrarily and
// hides the rest -- this exposes them.  The lexicographic tie-break that picks
// the *reported* winner is deliberately not applied here.
//
// Capped at `max_solutions`; `truncated` reports whether more existed.
struct XaccEquivalentSet {
    XaccStatus status = XaccStatus::ok;
    std::string message;
    bool ok() const { return status == XaccStatus::ok; }

    XaccScoreUnit score_units = 0;
    XaccCostUnit cost_units = 0;
    XaccScoreUnit distance_units = 0;
    std::vector<std::map<std::string, long long>> solutions;
    bool truncated = false;
};

XaccEquivalentSet xacc_equivalents(const XaccModel& model, const XaccTarget& target, XaccCount total,
                                   const std::map<std::string, long long>& fixed_counts, bool xperfect,
                                   long long max_solutions = 64,
                                   const XaccReverseOptions& options = XaccReverseOptions());

}  // namespace tuf
