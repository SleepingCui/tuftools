#include "jsoncli.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "api.hpp"
#include "console.hpp"
#include "info.hpp"
#include "numfmt.hpp"
#include "pyjson.hpp"
#include "tools.hpp"
#include "version.hpp"
#include "xacc_model.hpp"
#include "xacc_solver.hpp"

namespace tuf {
namespace {

// ---------------------------------------------------------------- protocol ---

constexpr long long kProtocolVersion = 1;
constexpr long long kDefaultPassLimit = 100;
constexpr long long kMaxSolutionsLimit = 4096;

// An operation-level failure: turns into {"ok":false,"error":{...}}.  `extra`
// items are merged into the error object (for example the candidate list of an
// ambiguous player search).
class OpError : public std::runtime_error {
public:
    OpError(std::string code, const std::string& message)
        : std::runtime_error(message), code_(std::move(code)) {}

    const std::string& code() const { return code_; }

    Json extra;

private:
    std::string code_;
};

// Sends every std::cout write to stderr while alive, so operation code (and
// defensive API logging) can never corrupt the response document.
class StdoutRedirect {
public:
    StdoutRedirect() : previous_(std::cout.rdbuf(std::cerr.rdbuf())) {}
    ~StdoutRedirect() { std::cout.rdbuf(previous_); }

    StdoutRedirect(const StdoutRedirect&) = delete;
    StdoutRedirect& operator=(const StdoutRedirect&) = delete;

private:
    std::streambuf* previous_;
};

[[noreturn]] void fail_input(const std::string& message) {
    throw OpError("invalid_input", message);
}

std::string solver_code(XaccSolver solver);

std::string join_strings(const std::vector<std::string>& values, const std::string& separator) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) out += separator;
        out += value;
    }
    return out;
}

// Whole numbers are emitted as JSON integers so that "total": 1234 does not
// show up as 1234.0 for consumers that do not like float-typed counters.
Json num_json(double value) {
    if (std::isfinite(value) && value == std::floor(value) && std::fabs(value) <= 9007199254740992.0) {
        return Json(static_cast<long long>(value));
    }
    return Json(value);
}

// ------------------------------------------------------------- value casts ---

double value_number(const Json& value, const std::string& label) {
    if (value.is_number()) return value.as_double();
    if (value.is_string()) {
        if (const auto parsed = try_parse_double(value.as_string())) return *parsed;
        fail_input("参数 \"" + label + "\" 不是有效数字: could not convert string to float: '" +
                   value.as_string() + "'");
    }
    fail_input("参数 \"" + label + "\" 必须是数字");
}

long long value_integer(const Json& value, const std::string& label) {
    if (value.is_int()) return value.as_int();
    if (value.is_number()) {
        const double number = value.as_double();
        if (!std::isfinite(number) || number != std::floor(number) || std::fabs(number) > 9.0e15) {
            fail_input("参数 \"" + label + "\" 必须是整数");
        }
        return static_cast<long long>(number);
    }
    if (value.is_string()) {
        if (const auto parsed = try_parse_int(value.as_string())) return *parsed;
        fail_input("参数 \"" + label + "\" 不是有效整数: invalid literal for int() with base 10: '" +
                   value.as_string() + "'");
    }
    fail_input("参数 \"" + label + "\" 必须是整数");
}

bool value_flag(const Json& value, const std::string& label) {
    if (value.is_bool()) return value.as_bool();
    if (value.is_number()) return value.as_double() != 0.0;
    if (value.is_string()) {
        const std::string text = lower_ascii(trim(value.as_string()));
        if (text == "true" || text == "1" || text == "yes" || text == "y") return true;
        if (text == "false" || text == "0" || text == "no" || text == "n" || text.empty()) return false;
        fail_input("参数 \"" + label + "\" 必须是布尔值");
    }
    fail_input("参数 \"" + label + "\" 必须是布尔值");
}

std::string value_text(const Json& value, const std::string& label) {
    if (value.is_string()) return value.as_string();
    if (value.is_bool() || value.is_array() || value.is_object()) {
        fail_input("参数 \"" + label + "\" 必须是字符串");
    }
    return value.key_text();
}

// ---------------------------------------------------------------- request ---

const std::vector<std::string>& op_names();

// Read-only view over one request object with alias-aware typed getters.
class Request {
public:
    Request(const Json& doc, std::string op) : doc_(doc), op_(std::move(op)) {}

    const std::string& op() const { return op_; }
    Json get(const std::string& key) const { return doc_.get(key); }
    bool present(const std::string& key) const { return !doc_.get(key).is_null(); }

    // First key that is present wins; the aliases keep the protocol forgiving.
    std::optional<Json> pick(std::initializer_list<const char*> keys) const {
        for (const char* key : keys) {
            const Json value = doc_.get(key);
            if (!value.is_null()) return value;
        }
        return std::nullopt;
    }

    [[noreturn]] void missing(const std::string& label) const {
        throw OpError("invalid_input",
                      "操作 " + (op_.empty() ? std::string("(未指定)") : op_) + " 缺少必需参数 \"" + label + "\"");
    }

    std::optional<std::string> text_opt(std::initializer_list<const char*> keys) const {
        const auto found = pick(keys);
        if (!found) return std::nullopt;
        return value_text(*found, *keys.begin());
    }

    std::string text(std::initializer_list<const char*> keys, bool required = false) const {
        if (const auto found = text_opt(keys)) return *found;
        if (required) missing(*keys.begin());
        return std::string();
    }

    std::optional<double> number_opt(std::initializer_list<const char*> keys) const {
        const auto found = pick(keys);
        if (!found) return std::nullopt;
        return value_number(*found, *keys.begin());
    }

    double number(std::initializer_list<const char*> keys, bool required, double fallback = 0.0) const {
        if (const auto found = number_opt(keys)) return *found;
        if (required) missing(*keys.begin());
        return fallback;
    }

    std::optional<long long> integer_opt(std::initializer_list<const char*> keys) const {
        const auto found = pick(keys);
        if (!found) return std::nullopt;
        return value_integer(*found, *keys.begin());
    }

    long long integer(std::initializer_list<const char*> keys, bool required,
                      long long fallback = 0) const {
        if (const auto found = integer_opt(keys)) return *found;
        if (required) missing(*keys.begin());
        return fallback;
    }

    bool flag(std::initializer_list<const char*> keys, bool fallback) const {
        if (const auto found = pick(keys)) return value_flag(*found, *keys.begin());
        return fallback;
    }

private:
    const Json& doc_;
    std::string op_;
};

// ---------------------------------------------------------------- helpers ---

// Wraps anything the API layer throws into a protocol error.  Templated on the
// callable's return type so it also guards non-Json results (rank maps, passes).
template <typename Function>
auto api_call(const std::string& label, Function&& function) -> decltype(function()) {
    try {
        return function();
    } catch (const OpError&) {
        throw;
    } catch (const std::exception& e) {
        throw OpError("api", label + ": " + e.what());
    }
}

std::string difficulty_error_code(const std::string& message) {
    if (message.rfind("未知难度", 0) == 0) return "unknown_difficulty";
    return "config";
}

Json rank_json(const RankVal& value) {
    if (value.numeric) return num_json(value.num);
    if (value.text == "?") return Json();
    return Json(value.text);
}

Json ranks_json(const std::map<std::string, RankVal>& ranks) {
    Json out = Json::object();
    for (const auto& item : ranks) out.set(item.first, rank_json(item.second));
    return out;
}

// Same lookup order as the interactive menus: the "level" wrapper, or the
// response itself when it already carries the level id.
Json fetch_level(const std::string& id) {
    const Json response = fetchapi_sync(BASE_URL + "/v2/database/levels/byId/" + url_quote(id));
    Json level = response.get("level");
    if (level.is_null() && response.get("id").key_text() == id) level = response;
    if (level.is_null()) throw OpError("api", "API 返回内容中没有谱面详情");
    return level;
}

XaccModel load_model(std::string* note) {
    XaccModel model;
    std::string error;
    std::string config_note;
    if (!xacc_model_load(model, error, &config_note)) throw OpError("config", "读取配置失败: " + error);
    if (note != nullptr) *note = config_note;
    return model;
}

// Tokens accepted by "solver" (the Chinese display names are aliases on top).
std::string solver_names() {
    std::vector<std::string> names;
    for (const XaccSolver solver : xacc_solver_all()) names.push_back(solver_code(solver));
    return join_strings(names, ", ");
}

std::string judgement_keys_text(const std::vector<std::string>& keys) {
    return join_strings(keys, ", ");
}

// Stable machine readable spellings; the Chinese display strings stay in
// "status"/"solver" for humans, these are what scripts should switch on.
std::string status_code(XaccStatus status) {
    switch (status) {
        case XaccStatus::ok: return "ok";
        case XaccStatus::invalid_input: return "invalid_input";
        case XaccStatus::invalid_config: return "invalid_config";
        case XaccStatus::out_of_range: return "out_of_range";
        case XaccStatus::unreachable: return "unreachable";
        case XaccStatus::budget_exhausted: return "budget_exhausted";
    }
    return "unknown";
}

std::string solver_code(XaccSolver solver) {
    switch (solver) {
        case XaccSolver::automatic: return "automatic";
        case XaccSolver::exact_dp: return "exact_dp";
        case XaccSolver::branch_and_bound: return "branch_and_bound";
        case XaccSolver::legacy_map_dp: return "legacy_map_dp";
    }
    return "unknown";
}

// Validates a judgement-count map against the active key set.  All keys default
// to zero so callers only spell out what they care about.
std::map<std::string, long long> parse_counts(const Json& value, const std::vector<std::string>& keys,
                                              const std::string& label) {
    std::map<std::string, long long> counts;
    for (const std::string& key : keys) counts[key] = 0;
    if (value.is_null()) return counts;
    if (!value.is_object()) fail_input("参数 \"" + label + "\" 必须是对象");
    for (const auto& item : value.items()) {
        if (std::find(keys.begin(), keys.end(), item.first) == keys.end()) {
            fail_input("未知判定 \"" + item.first + "\"（可用: " + judgement_keys_text(keys) + "）");
        }
        const long long count = value_integer(item.second, item.first);
        if (count < 0) fail_input("判定数量不能为负数: " + item.first);
        counts[item.first] = count;
    }
    return counts;
}

// Fixed (locked) judgement counts: only the keys the caller spelled out are
// locked, so no zero fill happens here.
std::map<std::string, long long> parse_fixed(const Json& value, const std::vector<std::string>& keys) {
    std::map<std::string, long long> fixed;
    if (value.is_null()) return fixed;
    if (!value.is_object()) fail_input("参数 \"fixed\" 必须是对象");
    for (const auto& item : value.items()) {
        if (std::find(keys.begin(), keys.end(), item.first) == keys.end()) {
            fail_input("未知判定 \"" + item.first + "\"（可用: " + judgement_keys_text(keys) + "）");
        }
        const long long count = value_integer(item.second, item.first);
        if (count < 0) fail_input("固定数量不能为负数: " + item.first);
        fixed[item.first] = count;
    }
    return fixed;
}

// Judgement counts passed positionally, ordered like jd_keys(xperfect).
std::map<std::string, long long> parse_judgements(const Json& value, const std::vector<std::string>& keys) {
    if (!value.is_array()) fail_input("参数 \"judgements\" 必须是数组");
    const std::vector<Json> elements = value.elements();
    if (elements.size() != keys.size()) {
        fail_input("参数 \"judgements\" 需要 " + std::to_string(keys.size()) + " 个数字（顺序: " +
                   judgement_keys_text(keys) + "）");
    }
    std::map<std::string, long long> counts;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const long long count = value_integer(elements[index], "judgements[" + std::to_string(index) + "]");
        if (count < 0) fail_input("判定数量不能为负数: " + keys[index]);
        counts[keys[index]] = count;
    }
    return counts;
}

std::string target_text_of(const Json& value) {
    std::string text = value_text(value, "xacc");
    text = trim(text);
    if (!text.empty() && text.back() == '%') text = trim(text.substr(0, text.size() - 1));
    if (text.empty()) fail_input("参数 \"xacc\" 不能为空");
    return text;
}

// ------------------------------------------------------------- shared op IO ---

// Echo of the locked counts, keys in the caller's own map order.
Json fixed_json(const std::map<std::string, long long>& fixed) {
    Json out = Json::object();
    for (const auto& item : fixed) out.set(item.first, item.second);
    return out;
}

Json counts_json(const std::map<std::string, long long>& counts, const std::vector<std::string>& keys) {
    Json out = Json::object();
    for (const std::string& key : keys) {
        const auto found = counts.find(key);
        out.set(key, static_cast<long long>(found == counts.end() ? 0 : found->second));
    }
    return out;
}

Json passes_json(const PassList& passes, long long limit) {
    const long long total = static_cast<long long>(passes.rows.size());
    long long count = total;
    if (limit > 0 && limit < total) count = limit;
    Json::Array rows;
    rows.reserve(static_cast<std::size_t>(count));
    for (long long index = 0; index < count; ++index) {
        rows.push_back(passes.rows[static_cast<std::size_t>(index)]);
    }
    Json out = Json::object();
    out.set("total", total);
    out.set("count", count);
    out.set("truncated", count < total);
    out.set("rows", Json::array(rows));
    return out;
}

// --------------------------------------------------------------- ops: meta ---

Json ops_array() {
    Json::Array array;
    for (const std::string& name : op_names()) array.push_back(Json(name));
    return Json::array(array);
}

Json op_version(const Request&) {
    Json out = Json::object();
    out.set("protocol", kProtocolVersion);
    out.set("name", std::string("tuftools"));
    out.set("version", std::string(TUF_VERSION));
    out.set("build_time", std::string(TUF_BUILD_TIME));
    out.set("api_base", BASE_URL);
    out.set("ops", ops_array());
    return out;
}

Json op_cache_clear(const Request&) {
    Json out = Json::object();
    if (!cache_enabled()) {
        out.set("cleared", false);
        out.set("skipped", true);
        out.set("reason", std::string("cache disabled (--no-cache)"));
        return out;
    }
    std::string error;
    if (!cache_clear(error)) throw OpError("cache", "缓存清空失败: " + error);
    out.set("cleared", true);
    out.set("skipped", false);
    out.set("reason", Json());
    return out;
}

// ------------------------------------------------------------- ops: player ---

Json full_player(const std::string& id) {
    const Json player = api_call("查询玩家", [&] { return get_player(id); });
    if (player.is_null() || (player.is_object() && player.items().empty())) {
        throw OpError("not_found", "未找到玩家: " + id);
    }
    return player;
}

struct ResolvedPlayer {
    Json player;
    std::string matched_by;
};

ResolvedPlayer resolve_player(const Request& req) {
    const std::string id = req.text({"id", "playerId", "player_id"});
    if (!id.empty()) return {full_player(id), "id"};

    std::string query;
    std::string matched_by;
    if (const auto discord = req.text_opt({"discord", "discordUsername"})) {
        query = "@" + *discord;
        matched_by = "discord";
    } else if (const auto name = req.text_opt({"query", "name"})) {
        query = *name;
        matched_by = "name";
    } else {
        throw OpError("invalid_input", "操作 " + req.op() + " 需要 id、query/name 或 discord 之一");
    }
    if (trim(query).empty()) throw OpError("invalid_input", "搜索关键词不能为空");

    const Json results = api_call("搜索玩家", [&] { return player_search(query); });
    const long long total = static_cast<long long>(results.size());
    if (total == 0) throw OpError("not_found", "未找到玩家: " + query);

    // A single hit, or an exact (case-insensitive) name hit, is unambiguous.
    long long chosen = -1;
    if (total == 1) {
        chosen = 0;
    } else if (matched_by == "name") {
        const std::string wanted = lower_ascii(trim(query));
        for (long long index = 0; index < total; ++index) {
            const Json item = results[static_cast<std::size_t>(index)];
            if (lower_ascii(trim(item.get("name").as_string())) == wanted) {
                chosen = index;
                break;
            }
        }
    }
    if (chosen < 0) {
        Json::Array candidates;
        for (long long index = 0; index < total && index < 10; ++index) {
            const Json item = results[static_cast<std::size_t>(index)];
            Json entry = Json::object();
            entry.set("index", index);
            entry.set("id", item.get("id"));
            entry.set("name", item.get("name"));
            entry.set("country", item.get("country"));
            entry.set("rankedScore", item.get("rankedScore"));
            candidates.push_back(entry);
        }
        OpError error("ambiguous", "匹配到多个玩家（共 " + std::to_string(total) + " 个），请改用 id 精确指定");
        error.extra = Json::object();
        error.extra.set("query", query);
        error.extra.set("candidates", Json::array(candidates));
        throw error;
    }
    return {results[static_cast<std::size_t>(chosen)], matched_by};
}

Json op_player_search(const Request& req) {
    std::string query;
    std::string by;
    if (const auto discord = req.text_opt({"discord", "discordUsername"})) {
        query = "@" + *discord;
        by = "discord";
    } else if (const auto name = req.text_opt({"query", "name", "q"})) {
        query = *name;
        by = "name";
    } else {
        req.missing("query");
    }
    if (trim(query).empty()) fail_input("搜索关键词不能为空");

    const Json results = api_call("搜索玩家", [&] { return player_search(query); });
    Json out = Json::object();
    out.set("query", query);
    out.set("by", by);
    out.set("count", static_cast<long long>(results.size()));
    out.set("results", results);
    return out;
}

Json op_player_get(const Request& req) {
    const std::string id = req.text({"id", "playerId", "player_id"});
    if (id.empty()) req.missing("id");
    Json out = Json::object();
    out.set("player", full_player(id));
    return out;
}

Json op_player_details(const Request& req) {
    const ResolvedPlayer resolved = resolve_player(req);
    Json player = resolved.player;

    // Search hits are partial: mirror run_player() and refetch the full record
    // before reading ranks or passes.
    if (!player.has("rankedScoreRank") || !player.has("totalScoreV2")) {
        const std::string id = player.get("id").key_text();
        if (!id.empty() && id != "null") player = full_player(id);
    }

    const bool include_ranks = req.flag({"includeRanks", "ranks"}, true);
    const bool include_passes = req.flag({"includePasses", "passes"}, true);

    Json out = Json::object();
    out.set("matched_by", resolved.matched_by);
    out.set("player", player);
    out.set("ranks", Json());
    out.set("passes", Json());

    if (include_ranks) {
        out.set("ranks", ranks_json(api_call("查询排名", [&] { return get_all_ranks(player); })));
    }
    if (include_passes) {
        long long limit = req.integer({"passLimit", "pass_limit"}, false, kDefaultPassLimit);
        if (limit < 0) fail_input("passLimit 不能为负数");
        const long long cap = req.integer({"passesFetchCap", "fetch_cap"}, false, 64);
        const Json name = player.get("name");
        if (name.is_string() && !name.as_string().empty()) {
            const PassList passes = api_call("查询通关记录", [&] {
                return fetch_all_player_passes(name.as_string(), player.get("id"), cap);
            });
            out.set("passes", passes_json(passes, limit));
        }
    }
    return out;
}

// -------------------------------------------------------------- ops: level ---

const std::set<std::string>& level_sort_keys() {
    static const std::set<std::string> keys = {"CLEARS", "DOWNLOADS", "LIKES", "FAVORITES", "BPM",
                                               "TILECOUNT", "DURATION", "BASESCORE", "ID"};
    return keys;
}

std::string normalize_level_sort(const Request& req) {
    std::string sort = upper_ascii(trim(req.text({"sort"}, false)));
    std::string base = sort;
    std::string suffix;
    if (base.size() > 5 && base.compare(base.size() - 5, 5, "_DESC") == 0) {
        suffix = "_DESC";
        base.resize(base.size() - 5);
    } else if (base.size() > 4 && base.compare(base.size() - 4, 4, "_ASC") == 0) {
        suffix = "_ASC";
        base.resize(base.size() - 4);
    }
    const std::set<std::string>& keys = level_sort_keys();
    if (keys.find(base) == keys.end()) {
        std::vector<std::string> names(keys.begin(), keys.end());
        fail_input("参数 \"sort\" 无效: \"" + sort + "\"（可用: " + join_strings(names, ", ") +
                   "，可附加 _DESC/_ASC 后缀）");
    }
    if (suffix.empty()) {
        const std::string order = upper_ascii(trim(req.text({"order"}, false)));
        if (order == "ASC") {
            suffix = "_ASC";
        } else if (order == "DESC" || order.empty()) {
            suffix = "_DESC";
        } else {
            fail_input("参数 \"order\" 必须是 asc 或 desc");
        }
    }
    return base + suffix;
}

Json op_level_search(const Request& req) {
    const std::string query = trim(req.text({"query", "q", "name"}, true));
    if (query.empty()) fail_input("搜索关键词不能为空");
    const std::string sort = normalize_level_sort(req);
    long long limit = req.integer({"limit"}, false, 10);
    long long offset = req.integer({"offset"}, false, 0);
    if (limit <= 0) fail_input("limit 必须大于 0");
    if (limit > 100) limit = 100;
    if (offset < 0) fail_input("offset 不能为负数");

    // Same shortcut as the interactive search: an all-digit query is a level id.
    if (std::all_of(query.begin(), query.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        const Json level = api_call("查询谱面", [&] { return fetch_level(query); });
        Json::Array rows;
        rows.push_back(level);
        Json out = Json::object();
        out.set("query", query);
        out.set("sort", Json());
        out.set("by_id", true);
        out.set("limit", limit);
        out.set("offset", offset);
        out.set("count", 1);
        out.set("hasMore", false);
        out.set("results", Json::array(rows));
        return out;
    }

    const std::string url = BASE_URL + "/v2/database/levels?query=" + url_quote(query) + "&sort=" +
                            url_quote(sort) + "&limit=" + std::to_string(limit) +
                            "&offset=" + std::to_string(offset);
    const Json data = api_call("查询谱面", [&] { return fetchapi_sync(url); });
    const Json::Array rows = data.get("results").elements();
    Json out = Json::object();
    out.set("query", query);
    out.set("sort", sort);
    out.set("by_id", false);
    out.set("limit", limit);
    out.set("offset", offset);
    out.set("count", static_cast<long long>(rows.size()));
    out.set("hasMore", data.get("hasMore").truthy());
    out.set("results", Json::array(rows));
    return out;
}

Json op_level_get(const Request& req) {
    const std::string id = req.text({"id", "levelId", "level_id"});
    if (id.empty()) req.missing("id");
    Json out = Json::object();
    out.set("id", id);
    out.set("level", api_call("查询谱面", [&] { return fetch_level(id); }));
    return out;
}

// ---------------------------------------------------------------- ops: pp ---

Json op_pp_calc(const Request& req) {
    TUFScoreCalculator calculator;
    const bool marathon = req.flag({"marathon", "isMarathon"}, false);
    const std::string level_id = req.text({"levelId", "level_id"});
    const auto base_score = req.number_opt({"baseScore", "base_score"});
    const auto difficulty = req.text_opt({"difficulty", "diff"});

    Json level_data;
    Json level_info;
    if (!level_id.empty()) {
        Json level = api_call("查询谱面", [&] { return fetch_level(level_id); });
        if (marathon) {
            Json difficulty_obj = level.get("difficulty");
            if (!difficulty_obj.is_object()) difficulty_obj = Json::object();
            difficulty_obj.set("name", std::string("Marathon"));
            level.set("difficulty", difficulty_obj);
        }
        const Json tiles = level.get("tilecount");
        if (!tiles.is_number() || tiles.as_int() <= 0) throw OpError("api", "API 未返回有效砖块数。");
        level_data = Json::object();
        level_data.set("level", level);
        level_info = Json::object();
        level_info.set("id", level.get("id"));
        level_info.set("song", level.get("song"));
        level_info.set("artist", level.get("artist"));
        level_info.set("difficulty", level.get("difficulty").get("name"));
        level_info.set("tilecount", tiles.as_int());
        level_info.set("baseScore", level.get("baseScore"));
        level_info.set("ppBaseScore", level.get("ppBaseScore"));
    } else {
        if (!base_score && !difficulty) {
            fail_input("需要提供 levelId、difficulty 或 baseScore 之一");
        }
        const long long tilecount = req.integer({"tilecount", "tiles"}, true);
        if (tilecount <= 0) fail_input("砖块数必须大于 0");
        if (base_score && (!std::isfinite(*base_score) || *base_score < 0.0)) {
            fail_input("BaseScore 必须是非负数字");
        }
        const std::string difficulty_name = difficulty ? upper_ascii(trim(*difficulty)) : std::string();
        try {
            if (!difficulty_name.empty() && !calculator.diffman().downloaded()) calculator.diffman().update();
            level_data = calculator.build_level_data(difficulty_name, marathon, tilecount, base_score);
        } catch (const OpError&) {
            throw;
        } catch (const std::exception& e) {
            throw OpError(difficulty_error_code(e.what()), e.what());
        }
    }

    std::optional<double> accuracy = req.number_opt({"xacc", "accuracy", "acc"});
    if (!accuracy) req.missing("xacc");
    if (!std::isfinite(*accuracy) || *accuracy < 0.0) fail_input("XACC 必须是非负数字");
    const long long misses = req.integer({"misses"}, false, 0);
    if (misses < 0) fail_input("空敲数不能为负数");
    const double speed = req.number({"speed"}, false, 1.0);
    if (!std::isfinite(speed) || speed <= 0.0) fail_input("速度倍率必须大于 0");
    const bool no_hold_tap = req.flag({"noHoldTap", "no_hold_tap", "noHold"}, false);

    Json result = calculator.calculate_score(level_data, *accuracy, misses, speed, no_hold_tap);
    result.set("level_id", level_id.empty() ? Json() : Json(level_id));
    result.set("level", level_info);
    return result;
}

// -------------------------------------------------------------- ops: xacc ---

Json op_xacc_calc(const Request& req) {
    const bool xperfect = req.flag({"xperfect", "xPerfect"}, false);
    std::string config_note;
    const XaccModel model = load_model(&config_note);
    const std::vector<std::string>& keys = jd_keys(xperfect);

    std::map<std::string, long long> counts;
    if (req.present("counts")) {
        counts = parse_counts(req.get("counts"), keys, "counts");
    } else if (req.present("judgements")) {
        counts = parse_judgements(req.get("judgements"), keys);
    } else {
        fail_input("需要提供 counts（对象）或 judgements（数组）");
    }

    XaccCount total = 0;
    XaccScoreUnit weighted = 0;
    for (const std::string& key : keys) {
        const long long count = counts[key];
        if (!add_checked(total, count, total)) throw OpError("overflow", "物量超出可计算范围");
        XaccScoreUnit term = 0;
        if (!mul_checked(model.weight(key), count, term)) throw OpError("overflow", "加权分超出可计算范围");
        if (!add_checked(weighted, term, weighted)) throw OpError("overflow", "加权分超出可计算范围");
    }
    if (total <= 0) fail_input("物量为 0，无法计算 XACC");

    const double xacc = xacc_acc_percent(weighted, total, model);
    Json out = Json::object();
    out.set("xperfect", xperfect);
    out.set("total", total);
    out.set("weighted", num_json(static_cast<double>(weighted) / static_cast<double>(model.score_scale)));
    out.set("score_units", weighted);
    out.set("score_scale", model.score_scale);
    out.set("xacc", num_json(xacc));
    out.set("xacc_text", py_float_str(xacc));
    out.set("counts", counts_json(counts, keys));
    if (!config_note.empty()) out.set("config_note", config_note);
    return out;
}

struct ReverseInput {
    XaccModel model;
    XaccTarget target;
    XaccCount total = 0;
    std::map<std::string, long long> fixed;
    bool xperfect = false;
    XaccReverseOptions options;
    std::string config_note;
    std::string target_text;
};

ReverseInput parse_reverse_input(const Request& req) {
    ReverseInput input;
    input.xperfect = req.flag({"xperfect", "xPerfect"}, false);
    input.total = req.integer({"total", "tilecount", "tiles"}, true);
    if (input.total <= 0) fail_input("物量必须大于 0");
    input.model = load_model(&input.config_note);

    const auto xacc_value = req.pick({"xacc", "accuracy", "acc"});
    if (!xacc_value) req.missing("xacc");
    input.target_text = target_text_of(*xacc_value);
    std::string error;
    if (!xacc_parse_target(input.target_text, input.target, error)) {
        throw OpError("invalid_input", "XACC 目标无法解析: " + error);
    }

    if (req.present("fixed")) input.fixed = parse_fixed(req.get("fixed"), jd_keys(input.xperfect));

    input.options.max_nodes = input.model.max_nodes;
    input.options.max_dp_cells = input.model.max_dp_cells;
    input.options.max_seconds = input.model.max_seconds;
    if (const auto solver = req.text_opt({"solver"})) {
        XaccSolver parsed = XaccSolver::automatic;
        if (!xacc_solver_from_text(*solver, parsed)) {
            fail_input("参数 \"solver\" 无效: \"" + *solver + "\"（可用: " + solver_names() + "）");
        }
        input.options.solver = parsed;
    }
    if (const auto nodes = req.integer_opt({"maxNodes", "max_nodes"})) {
        if (*nodes <= 0) fail_input("maxNodes 必须大于 0");
        input.options.max_nodes = *nodes;
    }
    if (const auto cells = req.integer_opt({"maxDpCells", "max_dp_cells"})) {
        if (*cells <= 0) fail_input("maxDpCells 必须大于 0");
        input.options.max_dp_cells = *cells;
    }
    if (const auto seconds = req.number_opt({"maxSeconds", "max_seconds"})) {
        if (!std::isfinite(*seconds) || *seconds <= 0.0) fail_input("maxSeconds 必须大于 0");
        input.options.max_seconds = *seconds;
    }
    return input;
}

// An impossible precision window is an answer, not a malformed request: check it
// up front so the caller gets found:false instead of an invalid_input error.
bool target_window_ok(const ReverseInput& input, std::string& message) {
    XaccScoreUnit low = 0;
    XaccScoreUnit high = 0;
    return xacc_target_window(input.target, input.model, input.total, low, high, message);
}

Json op_xacc_reverse(const Request& req) {
    const ReverseInput input = parse_reverse_input(req);
    XaccReverseResult result;
    std::string window_error;
    if (!target_window_ok(input, window_error)) {
        result.status = XaccStatus::unreachable;
        result.message = window_error;
        result.solver = input.options.solver;
    } else {
        result = xacc_reverse_search(input.model, input.target, input.total, input.fixed, input.xperfect,
                                     input.options);
    }

    const bool found = result.ok();
    const std::string status = xacc_status_text(result.status);
    if (!found && (result.status == XaccStatus::invalid_input || result.status == XaccStatus::invalid_config)) {
        throw OpError(result.status == XaccStatus::invalid_input ? "invalid_input" : "config", result.message);
    }

    Json out = Json::object();
    out.set("found", found);
    out.set("status", status);
    out.set("status_code", status_code(result.status));
    out.set("message", result.message);
    out.set("total", input.total);
    out.set("target", input.target_text);
    out.set("decimals", input.target.decimals);
    out.set("xperfect", input.xperfect);
    out.set("fixed", fixed_json(input.fixed));
    // A failed search reports no counts: all-zero counts would read like a valid
    // (score zero) solution.
    out.set("counts", found ? counts_json(result.counts, jd_keys(input.xperfect)) : Json());
    out.set("score_units", result.score_units);
    out.set("distance_units", result.distance_units);
    out.set("cost_units", result.cost_units);
    if (found) {
        // Same display rule as the interactive calculator: show 6 decimals and
        // fall back to the requested precision when that would lose digits.
        const double achieved = xacc_acc_percent(result.score_units, input.total, input.model);
        std::string achieved_text = trim_fixed(achieved, 6);
        if (decimal_places_of(achieved_text) < input.target.decimals) {
            achieved_text = format_fixed(achieved, input.target.decimals);
        }
        out.set("xacc", num_json(achieved));
        out.set("xacc_text", achieved_text);
    } else {
        out.set("xacc", Json());
        out.set("xacc_text", Json());
    }
    out.set("exact", result.exact);
    out.set("optimal", result.optimal);
    out.set("solver", xacc_solver_text(result.solver));
    out.set("solver_code", solver_code(result.solver));
    out.set("nodes", result.nodes);
    out.set("pruned", result.pruned);
    out.set("incumbent_updates", result.incumbent_updates);
    out.set("states", result.states);
    out.set("cells", result.cells);
    out.set("elapsed_ms", num_json(py_round(result.elapsed_ms, 3)));
    out.set("note", result.note.empty() ? Json() : Json(result.note));
    if (!input.config_note.empty()) out.set("config_note", input.config_note);
    return out;
}

Json op_xacc_equivalents(const Request& req) {
    ReverseInput input = parse_reverse_input(req);
    long long max_solutions = req.integer({"maxSolutions", "max_solutions"}, false, 64);
    if (max_solutions <= 0) fail_input("maxSolutions 必须大于 0");
    if (max_solutions > kMaxSolutionsLimit) max_solutions = kMaxSolutionsLimit;

    XaccEquivalentSet set{};
    std::string window_error;
    if (!target_window_ok(input, window_error)) {
        set.status = XaccStatus::unreachable;
        set.message = window_error;
    } else {
        set = xacc_equivalents(input.model, input.target, input.total, input.fixed, input.xperfect, max_solutions,
                               input.options);
    }
    const bool found = set.ok();
    if (!found && (set.status == XaccStatus::invalid_input || set.status == XaccStatus::invalid_config)) {
        throw OpError(set.status == XaccStatus::invalid_input ? "invalid_input" : "config", set.message);
    }

    const std::vector<std::string>& keys = jd_keys(input.xperfect);
    Json::Array solutions;
    for (const auto& counts : set.solutions) solutions.push_back(counts_json(counts, keys));

    Json out = Json::object();
    out.set("found", found);
    out.set("status", xacc_status_text(set.status));
    out.set("status_code", status_code(set.status));
    out.set("message", set.message);
    out.set("total", input.total);
    out.set("target", input.target_text);
    out.set("decimals", input.target.decimals);
    out.set("xperfect", input.xperfect);
    out.set("fixed", fixed_json(input.fixed));
    out.set("count", static_cast<long long>(solutions.size()));
    out.set("truncated", set.truncated);
    out.set("solutions", Json::array(solutions));
    out.set("score_units", set.score_units);
    out.set("distance_units", set.distance_units);
    out.set("cost_units", set.cost_units);
    out.set("xacc", found ? num_json(xacc_acc_percent(set.score_units, input.total, input.model)) : Json());
    out.set("solver", xacc_solver_text(input.options.solver));
    out.set("solver_code", solver_code(input.options.solver));
    if (!input.config_note.empty()) out.set("config_note", input.config_note);
    return out;
}

Json op_xacc_model(const Request&) {
    std::string config_note;
    const XaccModel model = load_model(&config_note);
    Json weights = Json::object();
    Json costs = Json::object();
    Json::Array order;
    for (const std::string& key : model.order) {
        order.push_back(Json(key));
        weights.set(key, num_json(static_cast<double>(model.weight(key)) /
                                  static_cast<double>(model.score_scale)));
        costs.set(key, num_json(static_cast<double>(model.cost(key)) /
                                static_cast<double>(model.cost_scale)));
    }
    Json out = Json::object();
    out.set("score_scale", model.score_scale);
    out.set("cost_scale", model.cost_scale);
    out.set("max_nodes", model.max_nodes);
    out.set("max_dp_cells", model.max_dp_cells);
    out.set("max_seconds", num_json(model.max_seconds));
    out.set("order", Json::array(order));
    out.set("weights", weights);
    out.set("costs", costs);
    Json::Array solvers;
    for (const XaccSolver solver : xacc_solver_all()) {
        Json entry = Json::object();
        entry.set("code", solver_code(solver));
        entry.set("name", xacc_solver_text(solver));
        solvers.push_back(entry);
    }
    out.set("solvers", Json::array(solvers));
    if (!config_note.empty()) out.set("config_note", config_note);
    return out;
}

// -------------------------------------------------------------- dispatch ---

const std::vector<std::string>& op_names() {
    static const std::vector<std::string> names = {"version",
                                                   "cache.clear",
                                                   "player.search",
                                                   "player.get",
                                                   "player.details",
                                                   "level.search",
                                                   "level.get",
                                                   "pp.calc",
                                                   "xacc.calc",
                                                   "xacc.reverse",
                                                   "xacc.equivalents",
                                                   "xacc.model"};
    return names;
}

Json dispatch(const Json& doc, std::string& op_out) {
    if (!doc.is_object()) throw OpError("invalid_request", "每个请求必须是 JSON 对象");
    const std::string op = doc.get("op").is_string() ? doc.get("op").as_string() : std::string();
    op_out = op;
    if (op.empty()) {
        throw OpError("invalid_request", "请求缺少字符串字段 \"op\"（可用操作: " + join_strings(op_names(), ", ") + "）");
    }

    const Request req(doc, op);
    if (op == "version") return op_version(req);
    if (op == "cache.clear") return op_cache_clear(req);
    if (op == "player.search") return op_player_search(req);
    if (op == "player.get") return op_player_get(req);
    if (op == "player.details") return op_player_details(req);
    if (op == "level.search") return op_level_search(req);
    if (op == "level.get") return op_level_get(req);
    if (op == "pp.calc") return op_pp_calc(req);
    if (op == "xacc.calc") return op_xacc_calc(req);
    if (op == "xacc.reverse") return op_xacc_reverse(req);
    if (op == "xacc.equivalents") return op_xacc_equivalents(req);
    if (op == "xacc.model") return op_xacc_model(req);
    throw OpError("unknown_op", "未知操作: " + op + "（可用操作: " + join_strings(op_names(), ", ") + "）");
}

Json response_for(const Json& doc) {
    const auto started = std::chrono::steady_clock::now();
    Json result;
    std::string op;
    std::string code;
    std::string message;
    Json extra;
    bool ok = true;
    try {
        result = dispatch(doc, op);
    } catch (const OpError& error) {
        ok = false;
        code = error.code();
        message = error.what();
        extra = error.extra;
    } catch (const std::exception& error) {
        ok = false;
        code = "internal";
        message = error.what();
    } catch (...) {
        ok = false;
        code = "internal";
        message = "未知错误";
    }
    const double elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    Json out = Json::object();
    out.set("ok", ok);
    out.set("op", op.empty() ? Json() : Json(op));
    if (doc.is_object() && doc.has("id")) out.set("id", doc.get("id"));
    out.set("elapsed_ms", num_json(py_round(elapsed, 3)));
    if (ok) {
        out.set("result", std::move(result));
    } else {
        Json error = Json::object();
        error.set("code", code);
        error.set("message", message);
        if (extra.is_object()) {
            for (const auto& item : extra.items()) error.set(item.first, item.second);
        }
        out.set("error", error);
    }
    return out;
}

// ------------------------------------------------------------- entry point ---

int emit_error(const JsonCliOptions& options, const std::string& code, const std::string& message) {
    Json error = Json::object();
    error.set("code", code);
    error.set("message", message);
    Json out = Json::object();
    out.set("ok", false);
    out.set("op", Json());
    out.set("error", error);
    std::cout << out.dump(options.pretty ? options.indent : -1) << std::endl;
    return 2;
}

void strip_utf8_bom(std::string& text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
}

bool read_stream(std::istream& stream, std::string& text) {
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    text = buffer.str();
    return true;
}

std::string request_parse_error(const std::string& text, const std::string& what) {
    std::string message = "无法解析 JSON 请求: " + what;
    // A Windows shell that eats the double quotes turns {"op":"x"} into {op:x}
    // before it ever reaches us; point the caller at the input forms that dodge it.
    if (text.find('"') == std::string::npos &&
        (text.find('{') != std::string::npos || text.find('[') != std::string::npos)) {
        message += "（提示：请求里一个双引号都没有，通常是 shell 吃掉了引号。请改用管道/tuftools -j、"
                   "--request-file，或把每个双引号写成 \\\"）";
    }
    return message;
}

}  // namespace

int run_json_cli(const JsonCliOptions& options) {
    std::string text;
    if (!options.request.empty() && options.request != "-") {
        text = options.request;
    } else if (!options.request.empty() && options.request == "-") {
        read_stream(std::cin, text);
    } else if (!options.request_file.empty()) {
        std::ifstream file(options.request_file, std::ios::binary);
        if (!file) return emit_error(options, "io_error", "无法读取请求文件: " + options.request_file);
        read_stream(file, text);
    } else {
        read_stream(std::cin, text);
    }
    strip_utf8_bom(text);
    if (trim(text).empty()) return emit_error(options, "invalid_request", "没有收到 JSON 请求（stdin 为空）");

    Json doc;
    try {
        doc = Json::parse(text);
    } catch (const std::exception& error) {
        return emit_error(options, "invalid_json", request_parse_error(text, error.what()));
    }

    Json output;
    bool all_ok = true;
    bool protocol_error = false;
    {
        StdoutRedirect redirect;
        if (doc.is_array()) {
            Json::Array responses;
            const Json::Array items = doc.elements();
            for (const Json& item : items) {
                const Json response = response_for(item);
                if (!response.get("ok").as_bool()) all_ok = false;
                responses.push_back(response);
            }
            output = Json::array(responses);
        } else if (doc.is_object()) {
            output = response_for(doc);
            if (!output.get("ok").as_bool()) all_ok = false;
        } else {
            protocol_error = true;
        }
        try {
            cache_flush();
        } catch (...) {
            // Cache persistence must never decide the exit status.
        }
    }

    if (protocol_error) {
        return emit_error(options, "invalid_request", "请求必须是 JSON 对象或对象数组");
    }
    std::cout << output.dump(options.pretty ? options.indent : -1) << std::endl;
    std::cout.flush();
    return all_ok ? 0 : 1;
}

}  // namespace tuf
