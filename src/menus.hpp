// Ports of ppcalc.py and acccalc.py (the interactive menu handlers).
#pragma once

namespace tuf {

// ppcalc.calculate_rank_changes(calculated_score)
void calculate_rank_changes(double calculated_score);

// ppcalc.handle_pp_calc()
void handle_pp_calc();

// acccalc.handle_acc_calc()
void handle_acc_calc();

}  // namespace tuf
