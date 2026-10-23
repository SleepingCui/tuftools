// Ports of ppcalc.py and acccalc.py (the interactive menu handlers).
#pragma once

#include <string>

namespace tuf {

// ppcalc.calculate_rank_changes(calculated_score, selected_level_id)
void calculate_rank_changes(double calculated_score, const std::string& selected_level_id = "");

// ppcalc.handle_pp_calc()
void handle_pp_calc();

// acccalc.handle_acc_calc()
void handle_acc_calc();

}  // namespace tuf
