// Judgement weights for the XACC tools (port of tools/XACCTools.py).
//
// These weights ARE the definition of XACC.  XACC is an accuracy defined by the
// game: every judgement is worth a fixed fraction of a perfect note, so
//     XACC = sum(count[j] * weight[j]) / total notes.
// They are a fixed rule, not a setting.  Changing one would silently change what
// every XACC percentage in this tool means, and a chart's XACC would stop matching
// the game.  So they are compiled in here: there is no weights.json any more and
// nothing reads them from xacc.json either.
//
// What the user IS meant to tune is the difficulty coefficient (cost) of each
// judgement -- see jd_cost_keys() / default_jd_costs() in tools.cpp and xacc.json.
// Costs never enter a forward XACC calculation; they only tell the reverse search
// which of several equally-accurate judgement splits to prefer.
//
// XPerfect: the game reports a single perfect note as +perfect, -perfect or
// xperfect.  All three are worth exactly perfect's weight, so a run scores the same
// in both modes; the reverse search still tells them apart through their own costs.

#include "tools.hpp"

namespace tuf {

// ------------------------------------------------------------- judgement ----
const std::vector<std::string>& jd_keys(bool xperfect) {
    static const std::vector<std::string> keys = {"failMiss", "tooEarly", "early", "ePerfect",
                                                  "perfect",  "lPerfect", "late"};
    static const std::vector<std::string> xperfect_keys = {"failMiss", "tooEarly",  "early",    "ePerfect",
                                                           "+perfect", "xperfect", "-perfect", "lPerfect",
                                                           "late"};
    return xperfect ? xperfect_keys : keys;
}

// ---------------------------------------------------------------- weights ---
// Fixed game rule.  The key set (and its order) is jd_cost_keys().
const std::map<std::string, double>& fixed_jd_weights() {
    static const std::map<std::string, double> weights = {
        {"perfect", 1.0},  {"failMiss", 0.0},  {"tooEarly", 0.2}, {"early", 0.4},  {"late", 0.4},
        {"ePerfect", 0.75}, {"lPerfect", 0.75},
        // XPerfect sub-judgements: identical to perfect, on purpose.
        {"+perfect", 1.0}, {"xperfect", 1.0}, {"-perfect", 1.0},
    };
    return weights;
}

}  // namespace tuf
