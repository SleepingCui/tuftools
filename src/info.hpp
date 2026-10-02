// Port of info.py: player search / rank lookup / pass listing plus the
// terminal width helpers used by the pass table.
#pragma once

#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "pyjson.hpp"

namespace tuf {

// A rank value can be a number coming from the API, or "?" when unknown.
struct RankVal {
    bool numeric = false;
    double num = 0.0;
    std::string text = "?";

    static RankVal unknown() { return RankVal{}; }
    static RankVal from_number(double value) {
        RankVal out;
        out.numeric = true;
        out.num = value;
        return out;
    }
    static RankVal from_json(const Json& value) {
        if (value.is_number()) return from_number(value.as_double());
        if (value.is_null()) return unknown();
        RankVal out;
        out.text = py_text(value);
        return out;
    }

    std::string str() const {
        if (!numeric) return text;
        if (num == static_cast<double>(static_cast<long long>(num))) return std::to_string(static_cast<long long>(num));
        return py_float_str(num);
    }
    bool is_zero() const { return numeric && num == 0.0; }
};

const int PASS_DISPLAY_LIMIT = 16;

Json player_search(const std::string& query);
Json get_player(const std::string& pid);

std::string rank_url(const Json& player, const std::string& sort_by, const std::string& scope = "global");
RankVal rank_count(const Json& response);
std::map<std::string, RankVal> fetch_ranks(const std::map<std::string, std::string>& jobs);
RankVal fetch_single_rank(const Json& player, const std::string& sort_by, const std::string& scope = "global");
std::map<std::string, RankVal> get_all_ranks(const Json& player);

Json choose_player(const Json& results);

struct PassList {
    std::vector<Json> rows;
    long long total = 0;
};

void details(const Json& player, const std::map<std::string, RankVal>& ranks, const PassList* passes = nullptr);
void run_player(const Json& player);
void handle_player_lookup();

// Resolves the difficulty name of a pass row: prefers level.difficulty.name from
// the passes payload itself, falls back to the /v2/database/difficulties table.
std::string difficulty_name_of(const Json& level);

int display_width(const std::string& text);
std::string cut_width(const std::string& text, int width);
std::string pad_width(const std::string& text, int width);
std::string right_justify(const std::string& text, int width);

PassList fetch_player_passes(const std::string& name, const Json& player_id, long long limit = PASS_DISPLAY_LIMIT,
                             long long fetch_cap = 64);
// Pass table. Columns are sized from their own content and 谱面 soaks up the
// remaining `width` columns, so the layout adapts to the terminal.
void print_passes_to(std::ostream& out, const std::vector<Json>& passes, long long total, int width);
void print_passes(const std::vector<Json>& passes, long long total = 0);

}  // namespace tuf
