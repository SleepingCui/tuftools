import requests
import json
from api import fetchapi, stats, BASE_URL

def search(query):
    url = f"{BASE_URL}/v3/players/search?query={query}"
    print(url)
    data = fetchapi(url)
    return data.get("results", [])

def get_player(pid):
    url = f"{BASE_URL}/v3/players/{pid}"
    print(url)
    return fetchapi(url)

def fetch_single_rank(player, sort_by, scope="global"):
    score = player.get(sort_by)
    if score is None or score == 0: return "?"

    filters = {
        sort_by: [score, 999999999]
    }
    
    if scope == "country": filters["country"] = player["country"]

    url = f"{BASE_URL}/v3/players/leaderboard?query=&sortBy={sort_by}&order=desc&offset=0&limit=1&showBanned=hide&filters={requests.utils.quote(json.dumps(filters))}"
    print(url)
    try:
        return fetchapi(url).get("count", "?")
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
    
    for score_key, rank_key in metrics.items():
        if rank_key in player and player[rank_key] is not None:
            g_rank = player[rank_key]
        else:
            g_rank = fetch_single_rank(player, sort_by=score_key, scope="global")     
        c_rank = fetch_single_rank(player, sort_by=score_key, scope="country")
        ranks_result[f"{score_key}_global"] = g_rank
        ranks_result[f"{score_key}_country"] = c_rank
        
    return ranks_result

def details(player, ranks):
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
    details(player, ranks)
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