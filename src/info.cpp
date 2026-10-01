#include "info.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>

#include "api.hpp"
#include "console.hpp"
#include "numfmt.hpp"

namespace tuf {

namespace {

std::string metric_rank(const std::map<std::string, RankVal>& ranks, const std::string& key) {
    auto it = ranks.find(key);
    return it == ranks.end() ? std::string("?") : it->second.str();
}

// East Asian Wide / Fullwidth ranges (approximation of unicodedata.east_asian_width in "WF").
bool is_wide_codepoint(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
           (cp >= 0xA960 && cp <= 0xA97F) || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE10 && cp <= 0xFE19) || (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF) ||
           (cp >= 0x20000 && cp <= 0x3FFFD);
}

struct Utf8Unit {
    size_t offset = 0;
    size_t length = 1;
    uint32_t codepoint = 0;
};

// Decode UTF-8; invalid bytes are reported as single narrow characters.
std::vector<Utf8Unit> utf8_units(const std::string& text) {
    std::vector<Utf8Unit> units;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = c;
        size_t extra = 0;
        if (c >= 0xF0) {
            cp = c & 0x07u;
            extra = 3;
        } else if (c >= 0xE0) {
            cp = c & 0x0Fu;
            extra = 2;
        } else if (c >= 0xC0) {
            cp = c & 0x1Fu;
            extra = 1;
        }

        bool valid = extra > 0;
        if (i + extra >= text.size()) valid = false;
        if (valid) {
            for (size_t k = 1; k <= extra; ++k) {
                const unsigned char next = static_cast<unsigned char>(text[i + k]);
                if ((next & 0xC0u) != 0x80u) {
                    valid = false;
                    break;
                }
                cp = (cp << 6) | (next & 0x3Fu);
            }
        }

        Utf8Unit unit;
        unit.offset = i;
        if (valid) {
            unit.length = extra + 1;
            unit.codepoint = cp;
        } else {
            unit.length = 1;
            unit.codepoint = c;
        }
        units.push_back(unit);
        i += unit.length;
    }
    return units;
}

size_t codepoint_count(const std::string& text) { return utf8_units(text).size(); }

std::string repeat(const std::string& unit, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) out += unit;
    return out;
}

}  // namespace

int display_width(const std::string& text) {
    int width = 0;
    for (const Utf8Unit& unit : utf8_units(text)) width += is_wide_codepoint(unit.codepoint) ? 2 : 1;
    return width;
}

std::string cut_width(const std::string& text, int width) {
    const std::vector<Utf8Unit> units = utf8_units(text);
    if (display_width(text) <= width) return text;

    std::string out;
    int used = 0;
    for (const Utf8Unit& unit : units) {
        const int w = is_wide_codepoint(unit.codepoint) ? 2 : 1;
        if (used + w > width - 1) break;
        out.append(text, unit.offset, unit.length);
        used += w;
    }
    return out + "…";
}

std::string pad_width(const std::string& text, int width) {
    const int pad = width - display_width(text);
    return text + repeat(" ", pad > 0 ? pad : 0);
}

std::string right_justify(const std::string& text, int width) {
    const int len = static_cast<int>(codepoint_count(text));
    const int pad = width - len;
    return repeat(" ", pad > 0 ? pad : 0) + text;
}

// ---------------------------------------------------------------- search ----

Json player_search(const std::string& query) {
    const std::string url = BASE_URL + "/v3/players/search?query=" + query;
    const Json data = fetchapi_sync(url);
    const Json& results = data.get("results");
    if (results.is_array()) return results;
    return Json::array();
}

Json get_player(const std::string& pid) { return fetchapi_sync(BASE_URL + "/v3/players/" + pid); }

std::string rank_url(const Json& player, const std::string& sort_by, const std::string& scope) {
    const Json& score = player.get(sort_by);
    if (score.is_null() || (score.is_number() && score.as_double() == 0.0)) return std::string();

    Json::Array range;
    range.push_back(score);
    range.push_back(Json(999999999));

    Json filters = Json::object();
    filters.set(sort_by, Json::array(range));
    if (scope == "country") filters.set("country", player.get("country"));

    return BASE_URL + "/v3/players/leaderboard?query=&sortBy=" + sort_by +
           "&order=desc&offset=0&limit=1&showBanned=hide&filters=" + url_quote(filters.dump());
}

RankVal rank_count(const Json& response) {
    if (!response.is_object()) return RankVal::unknown();
    const Json& count = response.get("count");
    if (!count.is_number()) return RankVal::unknown();
    return RankVal::from_json(count);
}

std::map<std::string, RankVal> fetch_ranks(const std::map<std::string, std::string>& jobs) {
    std::map<std::string, RankVal> ranks;
    if (jobs.empty()) return ranks;

    std::vector<std::string> urls;
    urls.reserve(jobs.size());
    for (const auto& job : jobs) urls.push_back(job.second);

    const std::vector<FetchResult> responses = fetchall_sync(urls);
    size_t index = 0;
    for (const auto& job : jobs) {
        ranks[job.first] = (index < responses.size() && responses[index].ok) ? rank_count(responses[index].data)
                                                                            : RankVal::unknown();
        ++index;
    }
    return ranks;
}

RankVal fetch_single_rank(const Json& player, const std::string& sort_by, const std::string& scope) {
    const std::string url = rank_url(player, sort_by, scope);
    if (url.empty()) return RankVal::unknown();
    try {
        return rank_count(fetchapi_sync(url));
    } catch (const std::exception&) {
        return RankVal::unknown();
    }
}

std::map<std::string, RankVal> get_all_ranks(const Json& player) {
    static const std::vector<std::pair<std::string, std::string>> metrics = {
        {"rankedScore", "rankedScoreRank"}, {"totalScoreV2", "totalScoreV2Rank"}, {"ppScore", "ppScoreRank"},
        {"wfScore", "wfScoreRank"},         {"wfPPScore", "wfPPScoreRank"},     {"score12K", "score12KRank"},
        {"generalScore", "generalScoreRank"},
    };

    std::map<std::string, RankVal> ranks_result;
    std::map<std::string, std::string> jobs;

    for (const auto& metric : metrics) {
        const std::string& score_key = metric.first;
        const Json& g_rank = player.get(metric.second);
        if (!g_rank.is_null()) {
            ranks_result[score_key + "_global"] = RankVal::from_json(g_rank);
        } else {
            const std::string url = rank_url(player, score_key, "global");
            if (!url.empty()) jobs[score_key + "_global"] = url;
            else ranks_result[score_key + "_global"] = RankVal::unknown();
        }

        const std::string country_url = rank_url(player, score_key, "country");
        if (!country_url.empty()) jobs[score_key + "_country"] = country_url;
        else ranks_result[score_key + "_country"] = RankVal::unknown();
    }

    const std::map<std::string, RankVal> fetched = fetch_ranks(jobs);
    for (const auto& kv : fetched) ranks_result[kv.first] = kv.second;
    return ranks_result;
}

Json choose_player(const Json& results) {
    if (results.size() == 1) return results[0];

    std::cout << "\n找到多个玩家:\n" << std::endl;
    for (size_t i = 0; i < results.size(); ++i) {
        const Json& player = results[i];
        std::cout << "[" << (i + 1) << "] " << player.get("name").as_string() << " (ID="
                  << player.get("id").key_text() << ", Country=" << player.get("country").key_text()
                  << ", RankedScore=" << format_fixed(player.get("rankedScore").number_or(0.0), 2) << ")" << std::endl;
    }

    while (true) {
        const std::optional<long long> choice = try_parse_int(read_line("\n选择玩家: "));
        if (choice.has_value() && *choice >= 1 && *choice <= static_cast<long long>(results.size())) {
            return results[static_cast<size_t>(*choice - 1)];
        }
        std::cout << "选择无效。" << std::endl;
    }
}

// --------------------------------------------------------------- details ----

void details(const Json& player, const std::map<std::string, RankVal>& ranks, const PassList* passes) {
    const Json& discord = player.get("discord");
    const Json& top_diff = player.get("topDiff");

    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "名称: " << py_repr(player.get("name")) << std::endl;
    std::cout << "ID: " << py_repr(player.get("id")) << std::endl;
    std::cout << "Discord: " << (discord.is_null() ? std::string("?") : py_repr(discord.get("username"))) << std::endl;
    std::cout << "国家: " << py_repr(player.get("country")) << std::endl;
    std::cout << std::endl;
    std::cout << "全球排名 (排位分): " << metric_rank(ranks, "rankedScore_global") << std::endl;
    std::cout << "全球排名 (总分): " << metric_rank(ranks, "totalScoreV2_global") << std::endl;
    std::cout << "全球排名 (无暇分): " << metric_rank(ranks, "ppScore_global") << std::endl;
    std::cout << "全球排名 (首通分): " << metric_rank(ranks, "wfScore_global") << std::endl;
    std::cout << "全球排名 (首杀分): " << metric_rank(ranks, "wfPPScore_global") << std::endl;
    std::cout << "全球排名 (12K分): " << metric_rank(ranks, "score12K_global") << std::endl;
    std::cout << "全球排名 (全局分): " << metric_rank(ranks, "generalScore_global") << std::endl;
    std::cout << std::endl;
    std::cout << "国家排名 (排位分): " << metric_rank(ranks, "rankedScore_country") << std::endl;
    std::cout << "国家排名 (总分): " << metric_rank(ranks, "totalScoreV2_country") << std::endl;
    std::cout << "国家排名 (无暇分): " << metric_rank(ranks, "ppScore_country") << std::endl;
    std::cout << "国家排名 (首通分): " << metric_rank(ranks, "wfScore_country") << std::endl;
    std::cout << "国家排名 (首杀分): " << metric_rank(ranks, "wfPPScore_country") << std::endl;
    std::cout << "国家排名 (12K分): " << metric_rank(ranks, "score12K_country") << std::endl;
    std::cout << "国家排名 (全局分): " << metric_rank(ranks, "generalScore_country") << std::endl;
    std::cout << std::endl;
    std::cout << "排位分: " << py_repr(player.get("rankedScore")) << std::endl;
    std::cout << "全局分: " << py_repr(player.get("generalScore")) << std::endl;
    std::cout << "总分: " << py_repr(player.get("totalScoreV2")) << std::endl;
    std::cout << "无暇分: " << py_repr(player.get("ppScore")) << std::endl;
    std::cout << "首通分: " << py_repr(player.get("wfScore")) << std::endl;
    std::cout << "首杀分: " << py_repr(player.get("wfPPScore")) << std::endl;
    std::cout << "12K分: " << py_repr(player.get("score12K")) << std::endl;
    std::cout << std::endl;

    const Json& average_xacc = player.get("averageXacc");
    std::cout << "平均XACC: "
              << (average_xacc.is_number() ? py_float_str(average_xacc.as_double() * 100.0) + "%" : std::string("?%"))
              << std::endl;
    std::cout << "U级通关数: " << py_repr(player.get("universalPassCount")) << std::endl;
    std::cout << "总通关数: " << py_repr(player.get("totalPasses")) << std::endl;
    std::cout << "世界首通数: " << py_repr(player.get("worldsFirstCount")) << std::endl;
    std::cout << "世界首杀数: " << py_repr(player.get("worldsFirstPPCount")) << std::endl;
    if (top_diff.truthy()) {
        std::cout << "最高通关难度: " << py_repr(top_diff.get("name")) << " (" << py_repr(top_diff.get("sortOrder"))
                  << ")" << std::endl;
    }

    if (passes == nullptr) {
        const PassList fetched = fetch_player_passes(player.get("name").as_string(), player.get("id"));
        print_passes(fetched.rows, fetched.total);
        return;
    }
    print_passes(passes->rows, passes->total);
}

void run_player(const Json& player_in) {
    Json player = player_in;
    if (!player.has("rankedScoreRank") || !player.has("totalScoreV2")) {
        player = get_player(player.get("id").key_text());
    }

    const std::map<std::string, RankVal> ranks = get_all_ranks(player);
    const PassList passes = fetch_player_passes(player.get("name").as_string(), player.get("id"));
    details(player, ranks, &passes);
    std::cout << std::endl;
    stats();
}

void handle_player_lookup() {
    while (true) {
        std::cout << "\n选择搜索类型" << std::endl;
        std::cout << "1. 名称" << std::endl;
        std::cout << "2. Discord 用户名" << std::endl;
        std::cout << "3. 玩家 ID" << std::endl;
        std::cout << "b. 返回主菜单" << std::endl;

        const std::string mode = read_trimmed("\n> ");

        if (mode == "1") {
            const std::string name = read_trimmed("玩家名:");
            const Json results = player_search(name);
            if (results.size() == 0) {
                std::cout << "未找到玩家" << std::endl;
                stats();
                continue;
            }
            run_player(choose_player(results));
        } else if (mode == "2") {
            const std::string username = read_trimmed("Discord 用户名:");
            const Json results = player_search("@" + username);
            if (results.size() == 0) {
                std::cout << "未找到玩家" << std::endl;
                stats();
                continue;
            }
            run_player(choose_player(results));
        } else if (mode == "3") {
            const std::string pid = read_trimmed("玩家 ID:");
            Json player;
            try {
                player = get_player(pid);
            } catch (const std::exception&) {
                player = Json();
            }
            if (player.is_null() || (player.is_object() && player.items().empty())) {
                std::cout << "未找到玩家" << std::endl;
                stats();
                continue;
            }
            run_player(player);
        } else if (mode == "b") {
            break;
        } else {
            std::cout << "搜索类型无效" << std::endl;
        }
    }
}

// --------------------------------------------------------- difficulty map ---

std::string difficulty_name_of(const Json& sort_order) {
    static bool loaded = false;
    static std::map<std::string, std::string> names;

    if (!loaded) {
        loaded = true;
        try {
            const Json list = fetchapi_sync(BASE_URL + "/v2/database/difficulties");
            if (list.is_array()) {
                for (const Json& item : list.elements()) {
                    if (!item.get("sortOrder").is_null()) {
                        names[item.get("sortOrder").key_text()] = item.get("name").as_string();
                    }
                }
            }
        } catch (const std::exception&) {
            // keep whatever was collected (mirrors the Python except-pass)
        }
    }

    if (sort_order.is_null()) return "-";
    auto it = names.find(sort_order.key_text());
    if (it != names.end() && !it->second.empty()) return it->second;
    return sort_order.key_text();
}

// ----------------------------------------------------------------- passes ---

PassList fetch_player_passes(const std::string& name, const Json& player_id, long long limit, long long fetch_cap) {
    PassList out;
    if (name.empty() || player_id.is_null()) return out;

    std::vector<Json> collected;
    long long total = 0;
    for (long long offset = 0; offset < 256; offset += fetch_cap) {
        const std::string url = BASE_URL + "/v2/database/passes?query=" + url_quote(name) + "&limit=" +
                                std::to_string(fetch_cap) + "&offset=" + std::to_string(offset);
        Json data;
        try {
            data = fetchapi_sync(url);
        } catch (const std::exception&) {
            break;
        }

        if (!data.is_object()) break;
        const Json& count = data.get("count");
        if (count.is_number()) total = count.as_int();

        const Json& rows = data.get("results");
        if (!rows.is_array()) break;
        for (const Json& row : rows.elements()) {
            if (row.get("playerId").key_text() == player_id.key_text()) {
                collected.push_back(row);
                if (static_cast<long long>(collected.size()) >= limit) {
                    out.rows = collected;
                    out.total = total;
                    return out;
                }
            }
        }
        if (static_cast<long long>(rows.size()) < fetch_cap) break;
    }

    out.rows = collected;
    out.total = total;
    return out;
}

void print_passes(const std::vector<Json>& passes, long long total) {
    if (passes.empty()) return;

    std::cout << std::endl;
    std::cout << "通关谱面 (按分数降序，前 " << passes.size() << " 个";
    if (total) {
        std::cout << "，共匹配 " << total << " 条):";
    } else {
        std::cout << "):";
    }
    std::cout << std::endl;
    std::cout << "   " << right_justify("分数", 11) << " " << right_justify("ACC", 9) << " " << pad_width("难度", 7)
              << pad_width("谱面", 35) << "标签" << std::endl;

    size_t index = 0;
    for (const Json& row : passes) {
        ++index;
        const Json& level = row.get("level");
        std::string song = level.get("song").as_string();
        if (song.empty()) song = "谱面 " + py_repr(row.get("levelId"));
        const std::string artist = level.get("artist").as_string();
        if (!artist.empty() && song.find(artist) == std::string::npos) song = song + " - " + artist;

        const Json& score = row.get("scoreV2");
        const std::string score_text = score.is_number() ? trim_fixed(score.as_double(), 2) : std::string("-");
        const Json& acc = row.get("accuracy");
        const std::string acc_text = acc.is_number() ? format_fixed(acc.as_double() * 100.0, 4) + "%" : std::string("-");

        std::vector<std::string> tags;
        if (row.get("isWorldsFirst").truthy()) tags.push_back("世界首通");
        if (row.get("isWorldsFirstPP").truthy()) tags.push_back("世界首杀");
        if (row.get("isNoHoldTap").truthy()) tags.push_back("NoHoldTap");
        const Json& speed = row.get("speed");
        if (speed.is_number() && std::abs(speed.as_double() - 1.0) > 1e-9) {
            tags.push_back(format_g(speed.as_double()) + "x");
        }
        if (row.get("isDuplicate").truthy()) tags.push_back("重复");
        if (row.get("isHidden").truthy()) tags.push_back("隐藏");
        if (row.get("isXPerfectMode").truthy()) tags.push_back("XPerfect");
        if (row.get("isWrongJudgement").truthy()) tags.push_back("判定异常");
        if (row.get("is12K").truthy()) tags.push_back("12K");
        else if (row.get("is16K").truthy()) tags.push_back("16K");

        std::string tag_text;
        for (size_t i = 0; i < tags.size(); ++i) {
            if (i) tag_text += " ";
            tag_text += tags[i];
        }

        std::cout << right_justify(std::to_string(index), 3) << " " << right_justify(score_text, 10) << " "
                  << right_justify(acc_text, 9) << " " << pad_width(difficulty_name_of(level.get("diffId")), 7)
                  << pad_width(cut_width(song, 34), 35) << tag_text << std::endl;
    }
}

}  // namespace tuf
