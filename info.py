import json
import unicodedata
from urllib.parse import quote

from api import fetchapi_sync, fetchall_sync, stats, BASE_URL

PASS_DISPLAY_LIMIT = 16

def search(query):
    url = f"{BASE_URL}/v3/players/search?query={query}"
    data = fetchapi_sync(url)
    return data.get("results", [])

def get_player(pid):
    url = f"{BASE_URL}/v3/players/{pid}"
    return fetchapi_sync(url)

def rank_url(player, sort_by, scope="global"):
    """构造排名查询URL；分数为空时无需查询，返回 None。"""
    score = player.get(sort_by)
    if score is None or score == 0: return None

    filters = {
        sort_by: [score, 999999999]
    }

    if scope == "country": filters["country"] = player["country"]

    return f"{BASE_URL}/v3/players/leaderboard?query=&sortBy={sort_by}&order=desc&offset=0&limit=1&showBanned=hide&filters={quote(json.dumps(filters))}"

def rank_count(response):
    return response.get("count", "?") if isinstance(response, dict) else "?"

def fetch_ranks(jobs: dict):
    if not jobs: return {}

    responses = fetchall_sync(list(jobs.values()))
    return {key: rank_count(resp) for key, resp in zip(jobs.keys(), responses)}

def fetch_single_rank(player, sort_by, scope="global"):
    url = rank_url(player, sort_by, scope)
    if url is None: return "?"

    try:
        return rank_count(fetchapi_sync(url))
    except:
        return "?"

def get_all_ranks(player):
    metrics = {
        "rankedScore": "rankedScoreRank",
        "totalScoreV2": "totalScoreV2Rank",
        "ppScore": "ppScoreRank",
        "wfScore": "wfScoreRank",
        "wfPPScore": "wfPPScoreRank",
        "score12K": "score12KRank",
        "generalScore": "generalScoreRank"
    }

    ranks_result = {}
    jobs = {}

    for score_key, rank_key in metrics.items():
        g_rank = player.get(rank_key)
        if g_rank is not None:
            ranks_result[f"{score_key}_global"] = g_rank
        else:
            url = rank_url(player, sort_by=score_key, scope="global")
            if url: jobs[f"{score_key}_global"] = url
            else: ranks_result[f"{score_key}_global"] = "?"

        url = rank_url(player, sort_by=score_key, scope="country")
        if url: jobs[f"{score_key}_country"] = url
        else: ranks_result[f"{score_key}_country"] = "?"

    ranks_result.update(fetch_ranks(jobs))
    return ranks_result

def details(player, ranks, passes=None, passes_total=0):
    discord = player.get("discord")
    td = player.get("topDiff")

    print("\n" + "=" * 70)
    print(f"名称: {player.get('name')}")
    print(f"ID: {player.get('id')}")
    print(f"Discord: {discord.get('username') if discord else '?'}")
    print(f"国家: {player.get('country')}")
    print()
    print(f"全球排名 (排位分): {ranks.get('rankedScore_global')}")
    print(f"全球排名 (总分): {ranks.get('totalScoreV2_global')}")
    print(f"全球排名 (无暇分): {ranks.get('ppScore_global')}")
    print(f"全球排名 (首通分): {ranks.get('wfScore_global')}")
    print(f"全球排名 (首杀分): {ranks.get('wfPPScore_global')}")
    print(f"全球排名 (12K分): {ranks.get('score12K_global')}")
    print(f"全球排名 (全局分): {ranks.get('generalScore_global')}")
    print()
    print(f"国家排名 (排位分): {ranks.get('rankedScore_country')}")
    print(f"国家排名 (总分): {ranks.get('totalScoreV2_country')}")
    print(f"国家排名 (无暇分): {ranks.get('ppScore_country')}")
    print(f"国家排名 (首通分): {ranks.get('wfScore_country')}")
    print(f"国家排名 (首杀分): {ranks.get('wfPPScore_country')}")
    print(f"国家排名 (12K分): {ranks.get('score12K_country')}")
    print(f"国家排名 (全局分): {ranks.get('generalScore_country')}")
    print()
    print(f"排位分: {player.get('rankedScore')}")
    print(f"全局分: {player.get('generalScore')}")
    print(f"总分: {player.get('totalScoreV2')}")
    print(f"无暇分: {player.get('ppScore')}")
    print(f"首通分: {player.get('wfScore')}")
    print(f"首杀分: {player.get('wfPPScore')}")
    print(f"12K分: {player.get('score12K')}")
    print()
    print(f"平均XACC: {player.get('averageXacc') * 100}%")
    print(f"U级通关数: {player.get('universalPassCount')}")
    print(f"总通关数: {player.get('totalPasses')}")
    print(f"世界首通数: {player.get('worldsFirstCount')}")
    print(f"世界首杀数: {player.get('worldsFirstPPCount')}")
    if td: 
        print(f"最高通关难度: {td.get('name')} ({td.get('sortOrder')})")

    if passes is None:
        passes, passes_total = fetch_player_passes(player.get("name"), player.get("id"))
    print_passes(passes, passes_total)

def choose_player(results):
    if len(results) == 1: return results[0]

    print("\n找到多个玩家:\n")
    for idx, player in enumerate(results, start=1):
        print(f"[{idx}] {player['name']} (ID={player['id']}, Country={player['country']}, RankedScore={player['rankedScore']:.2f})")

    while True:
        try:
            choice = int(input("\n选择玩家: "))
            if 1 <= choice <= len(results):
                return results[choice - 1]
        except:
            pass
        print("选择无效。")

def run(player):
    if "rankedScoreRank" not in player or "totalScoreV2" not in player:
        player = get_player(player["id"])
        
    ranks = get_all_ranks(player)
    passes, passes_total = fetch_player_passes(player.get("name"), player.get("id"))
    details(player, ranks, passes, passes_total)
    print()
    stats()

def handle_player_lookup():
    while True:
        print("\n选择搜索类型")
        print("1. 名称")
        print("2. Discord 用户名")
        print("3. 玩家 ID")
        print("b. 返回主菜单")

        mode = input("\n> ")
        player = None

        if mode == "1":
            name = input("玩家名:").strip()
            results = search(name)
            if not results:
                print("未找到玩家")
                stats()
                continue
            player = choose_player(results)
            run(player)

        elif mode == "2":
            username = input("Discord 用户名:").strip()
            results = search(f"@{username}")
            if not results:
                print("未找到玩家")
                stats()
                continue
            player = choose_player(results)
            run(player)

        elif mode == "3":
            pid = input("玩家 ID:").strip()
            try:
                player = get_player(pid)
            except:
                player = None

            if not player:
                print("未找到玩家")
                stats()
                continue
            run(player)

        elif mode == "b":
            break
        else:
            print("搜索类型无效")



_difficulty_names = None

def difficulty_name(sort_order):
    global _difficulty_names
    if _difficulty_names is None:
        _difficulty_names = {}
        try:
            for item in fetchapi_sync(f"{BASE_URL}/v2/database/difficulties"):
                if item.get("sortOrder") is not None:
                    _difficulty_names[str(item["sortOrder"])] = item.get("name")
        except Exception:
            pass

    if sort_order is None:
        return "-"
    return _difficulty_names.get(str(sort_order)) or str(sort_order)

def display_width(text):
    return sum(2 if unicodedata.east_asian_width(ch) in "WF" else 1 for ch in str(text))

def cut_width(text, width):
    text = str(text)
    if display_width(text) <= width: return text

    out, used = "", 0
    for ch in text:
        w = 2 if unicodedata.east_asian_width(ch) in "WF" else 1
        if used + w > width - 1: break
        out += ch
        used += w
    return out + "…"

def fetch_player_passes(name, player_id, limit=PASS_DISPLAY_LIMIT, fetch_cap=64):
    if not name or player_id is None: return [], 0

    collected, total = [], 0
    for offset in range(0, 256, fetch_cap):
        url = f"{BASE_URL}/v2/database/passes?query={quote(str(name))}&limit={fetch_cap}&offset={offset}"
        try:
            data = fetchapi_sync(url)
        except Exception:
            break

        if not isinstance(data, dict): break
        total = data.get("count") or total

        rows = data.get("results") or []
        for row in rows:
            if row.get("playerId") == player_id:
                collected.append(row)
                if len(collected) >= limit: return collected, total
        if len(rows) < fetch_cap: break

    return collected, total

def pad_width(text, width):
    return str(text) + " " * max(0, width - display_width(text))

def print_passes(passes, total=0):
    if not passes: return

    print()
    print(f"通关谱面 (按分数降序，前 {len(passes)} 个" + (f"，共匹配 {total} 条):" if total else "):"))
    print("   " + f"{'分数':>11} {'ACC':>9} " + pad_width("难度", 7)
          + pad_width("谱面", 35) + "标签")
    for idx, row in enumerate(passes, start=1):
        level = row.get("level") or {}
        song = level.get("song") or f"谱面 {row.get('levelId')}"
        artist = level.get("artist")
        if artist and artist not in str(song): song = f"{song} - {artist}"

        score = row.get("scoreV2")
        score_text = f"{score:.2f}".rstrip("0").rstrip(".") if isinstance(score, (int, float)) else "-"
        acc = row.get("accuracy")
        acc_text = f"{acc * 100:.4f}%" if isinstance(acc, (int, float)) else "-"

        tags = []
        if row.get("isWorldsFirst"): tags.append("世界首通")
        if row.get("isWorldsFirstPP"): tags.append("世界首杀")
        if row.get("isNoHoldTap"): tags.append("NoHoldTap")
        speed = row.get("speed")
        if isinstance(speed, (int, float)) and abs(speed - 1) > 1e-9: tags.append(f"{speed:g}x")
        if row.get("isDuplicate"): tags.append("重复")
        if row.get("isHidden"): tags.append("隐藏")
        if row.get("isXPerfectMode"): tags.append("XPerfect")
        if row.get("isWrongJudgement"): tags.append("判定异常")
        if row.get("is12K"): tags.append("12K")
        elif row.get("is16K"): tags.append("16K")

        print(f"{idx:>3} {score_text:>10} {acc_text:>9} "
              + pad_width(difficulty_name(level.get("diffId")), 7)
              + pad_width(cut_width(song, 34), 35) + " ".join(tags))
