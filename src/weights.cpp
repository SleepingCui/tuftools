// Judgement weights for the XACC tools (port of tools/XACCTools.py).
//
// This file is the single place where judgement weights live.
//
//   * jd_keys()          -- the judgement list of each mode (forward input order).
//   * default_jd_weights -- the built-in weights, used when weights.json is absent.
//   * jd_weights()       -- what the calculators actually use: read from
//                           weights.json in the data directory (beside costs.json).
//
// weights.json is optional.  When it is missing it is written with the built-in
// defaults, and when a key is missing from it the built-in default is used, so
// the tool always starts.  Editing it needs no rebuild -- just restart the tool
// (costs.json behaves the same way).

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "apppaths.hpp"
#include "tools.hpp"

namespace tuf {

namespace {
constexpr const char* WEIGHTS_FILE = "weights.json";
}  // namespace

// ------------------------------------------------------------- judgement ----
// XPerfect: the single `perfect` judgement is reported by the game as +perfect,
// -perfect or xperfect.  Their XACC weight defaults to perfect's, so a run scores
// exactly the same in both modes; only the reverse search distinguishes them
// (they carry their own costs, see jd_cost_keys()).
const std::vector<std::string>& jd_keys(bool xperfect) {
    static const std::vector<std::string> keys = {"failMiss", "tooEarly", "early", "ePerfect",
                                                  "perfect",  "lPerfect", "late"};
    static const std::vector<std::string> xperfect_keys = {"failMiss", "tooEarly",  "early",    "ePerfect",
                                                           "+perfect", "xperfect", "-perfect", "lPerfect",
                                                           "late"};
    return xperfect ? xperfect_keys : keys;
}

// ---------------------------------------------------------------- weights ---
// The key set (and its order) is jd_cost_keys(); weights.json uses exactly the
// same keys, so both files stay in step and costs/weights are edited the same way.
const std::map<std::string, double>& default_jd_weights() {
    static const std::map<std::string, double> weights = {
        {"perfect", 1.0},   {"failMiss", 0.0}, {"tooEarly", 0.2}, {"early", 0.4},
        {"late", 0.4},      {"ePerfect", 0.75}, {"lPerfect", 0.75},
        // XPerfect sub-judgements.  Keep these at perfect's weight (1.0) or an
        // XPerfect run stops matching the same run in normal mode.
        {"+perfect", 0.9},  {"xperfect", 1.0}, {"-perfect", 0.9},
    };
    return weights;
}

std::map<std::string, double> jd_weights_load() {
    const std::map<std::string, double>& defaults = default_jd_weights();
    std::ifstream file(data_file(WEIGHTS_FILE), std::ios::binary);
    if (!file.good()) {
        jd_weights_save(defaults);
        return defaults;
    }
    try {
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const Json parsed = Json::parse(buffer.str());
        std::map<std::string, double> weights;
        for (const std::string& key : jd_cost_keys()) {
            weights[key] = parsed.get(key).is_number() ? parsed.get(key).as_double() : defaults.at(key);
        }
        return weights;
    } catch (const std::exception& e) {
        std::cout << "读取权重配置失败!使用默认值: " << e.what() << std::endl;
        return defaults;
    }
}

void jd_weights_save(const std::map<std::string, double>& weights) {
    try {
        Json root = Json::object();
        for (const std::string& key : jd_cost_keys()) {
            auto it = weights.find(key);
            root.set(key, it == weights.end() ? default_jd_weights().at(key) : it->second);
        }
        std::ofstream file(data_file(WEIGHTS_FILE), std::ios::binary | std::ios::trunc);
        file << root.dump(4);
    } catch (const std::exception& e) {
        std::cout << "保存权重配置失败: " << e.what() << std::endl;
    }
}

std::map<std::string, double> jd_weights(bool xperfect) {
    // Both modes share one table: XPerfect only adds three more keys to read.
    (void)xperfect;
    return jd_weights_load();
}

}  // namespace tuf
