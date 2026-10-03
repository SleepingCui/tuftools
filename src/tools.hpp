// Ports of tools/CostsMan.py, tools/DiffMan.py, tools/TUFScoreCalculator.py and
// tools/XACCTools.py.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pyjson.hpp"
#include "xacc_model.hpp"

namespace tuf {

// ---------------------------------------------------------------- CostsMan --
// Data files live next to the executable; see apppaths.hpp.
const std::vector<std::string>& jd_cost_keys();
const std::map<std::string, double>& default_jd_costs();
std::map<std::string, double> costs_load();
void costs_save(const std::map<std::string, double>& costs);

// -------------------------------------------------------------- DiffMan -----
class DifficultyManager {
public:
    DifficultyManager();

    static bool file_exists();
    const std::vector<Json>& items() const { return difficulties_; }
    std::vector<Json> load();
    std::vector<Json> update();
    // Throws std::runtime_error (Python: ValueError) for unknown difficulties.
    double get_base_score(const std::string& difficulty_name) const;
    bool downloaded() const { return downloaded_; }

private:
    std::vector<Json> difficulties_;
    bool downloaded_ = false;
};

// ---------------------------------------------------- TUFScoreCalculator ----
const std::vector<std::string>& score_judgement_keys();
const std::map<std::string, double>& score_judgement_weights();

class TUFScoreCalculator {
public:
    TUFScoreCalculator();

    Json build_level_data(const std::string& difficulty_name = std::string(), bool marathon = false,
                          long long tilecount = 0, std::optional<double> base_score = std::nullopt);
    std::pair<double, std::string> get_base_score(const Json& level_data, double accuracy) const;
    double calculate_score_multiplier(double accuracy, double base_score, const Json* xacc_curve = nullptr) const;
    double calculate_speed_modifier(double speed, bool is_marathon = false) const;
    static double calculate_empty_tap_modifier(long long empty_taps, long long tilecount);
    Json calculate_score(const Json& level_data, double accuracy_pct, long long misses = 1, double speed = 1.0,
                         bool is_no_hold_tap = false) const;

    DifficultyManager& diffman() { return diffman_; }

private:
    DifficultyManager diffman_;
    std::vector<Json> difficulties_;
};

// -------------------------------------------------------------- XACCTools ---
// XPerfect splits the single `perfect` judgement into +perfect / -perfect / xperfect.
// All three carry perfect's weight (1.0), so the XACC value of a run is unchanged;
// only the reverse search treats them as distinct judgements, and it prices them
// separately through xacc.json (defaults: xperfect 0, +perfect 10, -perfect 10).
//
// The judgement weights are a FIXED GAME RULE, not configuration: XACC is defined
// by them, so they are compiled in (weights.cpp) and never read from or written to
// a data file.  Only the difficulty coefficients are meant to be tunable.
const std::vector<std::string>& jd_keys(bool xperfect = false);
const std::map<std::string, double>& fixed_jd_weights();

// Compatibility wrappers (they load their own model).  Prefer the model-aware
// overloads below: one operation should load the configuration exactly once.
double xacc_calc(const std::vector<long long>& judgements, bool xperfect = false);
double xacc_calc(const XaccModel& model, const std::vector<long long>& judgements, bool xperfect);
// Returns the XACC ratio in [0, 1].
//
// acc_decimals is the number of decimals the user typed for the target XACC; the
// search window (and therefore the reported result) follows that precision.
std::optional<std::map<std::string, long long>> xacc_reverse(double target_acc, long long total,
                                                             const std::map<std::string, long long>& fixed_counts,
                                                             int acc_decimals = 2, bool xperfect = false);

}  // namespace tuf
