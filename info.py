import json
from concurrent.futures import ThreadPoolExecutor

import requests

import api
import ui
from api import ApiError, BASE_URL


#: 排名指标。顺序即玩家详情表的行序。
#: 每项为 (显示名, 分数键, 排名键)。
RANK_METRICS = [
    ("排位分", "rankedScore", "rankedScoreRank"),
    ("总分", "totalScoreV2", "totalScoreV2Rank"),
    ("无瑕分", "ppScore", "ppScoreRank"),
    ("首通分", "wfScore", "wfScoreRank"),
    ("首杀分", "wfPPScore", "wfPPScoreRank"),
    ("12K分", "score12K", "score12KRank"),
    ("全局分", "generalScore", "generalScoreRank"),
]

#: 详情面板用 (显示名, 分数键)。
DETAIL_METRICS = [(label, score_key) for label, score_key, _ in RANK_METRICS]

#: 搜索方式菜单，玩家查询和 PP 计算器的排名变化共用。
SEARCH_MENU = [
    ("1", "名称"),
    ("2", "Discord 用户名"),
    ("3", "玩家 ID"),
    ("b", "返回"),
]


def search(query):
    url = f"{BASE_URL}/v3/players/search?query={query}"
    ui.debug(url)
    data = api.fetchapi(url)
    return data.get("results", [])


def get_player(pid):
    url = f"{BASE_URL}/v3/players/{pid}"
    ui.debug(url)
    return api.fetchapi(url)


def fetch_single_rank(player, sort_by, scope="global"):
    score = player.get(sort_by)
    if score is None or score == 0:
        return "?"

    filters = {
        sort_by: [score, 999999999]
    }

    if scope == "country":
        # 用 .get 而不是 player["country"]：搜索接口返回的条目可能没有 country，
        # 直接下标会 KeyError（并发时在 future.result() 处炸）。
        # 拿不到国家就返回 "?"——不能退回不带动过滤条件的查询，
        # 那会拿全球排名冒充国家排名。
        country = player.get("country")
        if not country:
            return "?"
        filters["country"] = country

    url = f"{BASE_URL}/v3/players/leaderboard?query=&sortBy={sort_by}&order=desc&offset=0&limit=1&showBanned=hide&filters={requests.utils.quote(json.dumps(filters))}"
    ui.debug(url)
    try:
        return api.fetchapi(url).get("count", "?")
    except ApiError:
        return "?"


def fetch_many(jobs):
    """并发执行一批 fetch_single_rank，返回值顺序与 jobs 一致。

    jobs 每项为 (player, sort_by, scope)。
    """
    if not jobs:
        return []
    with ThreadPoolExecutor(max_workers=min(14, len(jobs))) as pool:
        return list(pool.map(lambda job: fetch_single_rank(*job), jobs))


def get_all_ranks(player):
    """并发拉取所有排名。

    7 个指标 × (全球, 国家) 最多 14 个互相独立的请求，串行做要等十几秒，
    这里一次性提交到线程池。player 上已带排名的指标跳过全球查询。
    """
    ranks_result = {}
    jobs = []
    keys = []

    for _, score_key, rank_key in RANK_METRICS:
        cached = player.get(rank_key)
        if cached is not None:
            ranks_result[f"{score_key}_global"] = cached
        else:
            keys.append(f"{score_key}_global")
            jobs.append((player, score_key, "global"))

    for _, score_key, _ in RANK_METRICS:
        keys.append(f"{score_key}_country")
        jobs.append((player, score_key, "country"))

    for key, rank in zip(keys, fetch_many(jobs)):
        ranks_result[key] = rank

    return ranks_result


def details(player, ranks):
    ui.player_panel(player, ranks, DETAIL_METRICS)


def choose_player(results):
    """让用户从搜索结果里挑一个。返回玩家 dict，用户取消时返回 ui.BACK。"""
    if len(results) == 1:
        return results[0]

    rows = [
        [
            str(idx),
            p.get("name") or "?",
            p.get("country") or "?",
            ui.fmt_score(p.get("rankedScore")),
        ]
        for idx, p in enumerate(results, start=1)
    ]
    ui.table(
        [("#", "right"), "玩家", "国家", ("排位分", "right")],
        rows,
        title=f"找到 {len(results)} 个玩家",
    )

    choice = ui.ask(
        "选择玩家",
        cast=int,
        allow_back=True,
        validate=lambda v: None if 1 <= v <= len(results) else f"请输入 1-{len(results)}，或 b 返回",
        hint="(b 返回)",
    )
    if choice is ui.BACK:
        return ui.BACK
    return results[choice - 1]


def resolve_player(mode: str) -> "dict | None | ui.BackType":
    """按搜索方式取回玩家。返回玩家 dict、ui.BACK（用户取消）或 None（未找到）。"""
    if mode == "3":
        pid = ui.ask("玩家 ID", allow_back=True)
        if pid is ui.BACK:
            return ui.BACK
        try:
            return get_player(pid)
        except ApiError:
            return None

    if mode == "1":
        query = ui.ask("玩家名", allow_back=True)
    else:
        query = ui.ask("Discord 用户名", allow_back=True)
        if query is not ui.BACK:
            query = f"@{query}"

    if query is ui.BACK:
        return ui.BACK

    results = search(query)
    if not results:
        return None
    return choose_player(results)


def run(player):
    if "rankedScoreRank" not in player or "totalScoreV2" not in player:
        player = get_player(player["id"])

    with ui.spinner("查询排名..."):
        ranks = get_all_ranks(player)

    ui.blank()
    details(player, ranks)
    ui.blank()
    ui.api_stats(*api.take_stats())


def handle_player_lookup():
    while True:
        mode = ui.menu("选择搜索类型", SEARCH_MENU)
        if mode == "b":
            break

        try:
            player = resolve_player(mode)
        except ApiError as e:
            ui.error(str(e))
            ui.blank()
            ui.api_stats(*api.take_stats())
            continue

        if player is ui.BACK:
            continue
        if player is None:
            ui.error("未找到玩家")
            ui.blank()
            ui.api_stats(*api.take_stats())
            continue

        run(player)
