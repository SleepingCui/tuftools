// Ports of ppcalc.py and acccalc.py (the interactive menu handlers).
#pragma once

#include <optional>
#include <string>

#include "pyjson.hpp"

namespace tuf {

// Weighted ranked score (best pass per level, top-20 with a 0.9 decay) after
// injecting one candidate score.  Returns nullopt and fills `error` when the
// stored passes cannot reproduce the player's current rankedScore.  Shared by
// the interactive rank preview and the JSON mode's rank.preview operation.
std::optional<double> ranked_score_after_candidate(const Json& player, double candidate_score,
                                                   const std::string& candidate_level_id,
                                                   double& added_score, std::string& error);

// ppcalc.calculate_rank_changes(calculated_score, selected_level_id)
void calculate_rank_changes(double calculated_score, const std::string& selected_level_id = "");

// ppcalc.handle_pp_calc()
void handle_pp_calc();

// acccalc.handle_acc_calc()
void handle_acc_calc();

// Search for a level, show its metadata, and optionally download its chart file.
void handle_level_lookup();

}  // namespace tuf
