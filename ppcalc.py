import api
import info
import ui
from tools.DiffMan import DifficultyManager
from tools.TUFScoreCalculator import TUFScoreCalculator


def rank_delta(old_rank, new_rank):
    """排名的升降。返回 (文本, 样式)，样式只在 rich 后端生效。"""
    try:
        delta = int(old_rank) - int(new_rank)
    except (TypeError, ValueError):
        return "N/A", "tuf.muted"
    if delta > 0:
        return f"+{delta} ▲", "tuf.good"
    if delta < 0:
        return f"{delta} ▼", "tuf.bad"
    return "±0", "tuf.muted"


def _safe_rank(value):
    """排名的边界修正：API 在极少数情况下返回 0，语义上应为第 1 名。"""
    return 1 if value == 0 else value


def calculate_rank_changes(calculated_score):
    mode = ui.menu("选择查找玩家的方式", info.SEARCH_MENU)
    if mode == "b":
        return

    try:
        player = info.resolve_player(mode)
    except api.ApiError as e:
        ui.error(str(e))
        return

    if isinstance(player, ui.BackType):
        return
    if player is None:
        ui.error("未找到玩家")
        return

    if "rankedScoreRank" not in player or "rankedScore" not in player:
        try:
            player = info.get_player(player["id"])
        except api.ApiError as e:
            ui.error(f"无法获取玩家数据：{e}")
            return

    old_score = player.get("rankedScore", 0)
    new_score = old_score + calculated_score

    cached_global = player.get("rankedScoreRank")
    future_player = {"country": player.get("country"), "rankedScore": new_score}

    with ui.spinner("查询排名变化..."):
        if cached_global is not None:
            old_global_rank = cached_global
            new_global_rank, new_country_rank = info.fetch_many(
                [
                    (future_player, "rankedScore", "global"),
                    (future_player, "rankedScore", "country"),
                ]
            )
        else:
            old_global_rank, new_global_rank, new_country_rank = info.fetch_many(
                [
                    (player, "rankedScore", "global"),
                    (future_player, "rankedScore", "global"),
                    (future_player, "rankedScore", "country"),
                ]
            )
        old_country_rank = info.fetch_single_rank(player, "rankedScore", "country")

    old_global_rank = _safe_rank(old_global_rank)
    new_global_rank = _safe_rank(new_global_rank)
    old_country_rank = _safe_rank(old_country_rank)
    new_country_rank = _safe_rank(new_country_rank)

    ui.blank()
    ui.panel(
        f"{player.get('name')}  ·  #{player.get('id')}",
        [
            ("原排位分", ui.fmt_score(old_score)),
            ("新排位分", ui.fmt_score(new_score), "tuf.good"),
            ("本次增加", f"+{calculated_score:,.2f}", "tuf.good"),
        ],
        columns=1,
    )

    rows = []
    for label, old, new in (
        ("全球排名", old_global_rank, new_global_rank),
        ("国家排名", old_country_rank, new_country_rank),
    ):
        delta_text, style = rank_delta(old, new)
        rows.append([label, ui.fmt_rank(old), ui.fmt_rank(new), (delta_text, style)])
    ui.table(
        ["", ("原排名", "right"), ("新排名", "right"), ("变化", "right")], rows, title="排名变化"
    )


def _difficulty_validator(diffman):
    """难度名校验：不认识时给出最接近的几个候选，而不是抛异常。"""
    names = diffman.difficulty_names()
    known = {name.upper() for name in names}

    def validate(value):
        if value.upper() in known:
            return None
        close = ui.suggest(value, names)
        if close:
            return f"未知难度「{value}」，是不是想要：{' / '.join(close)}？"
        return f"未知难度「{value}」"

    return validate


def _load_difficulties():
    """取回难度库，必要时下载。失败返回 None。"""
    if DifficultyManager.has_cache():
        return DifficultyManager()

    try:
        with ui.spinner("下载难度数据库..."):
            return DifficultyManager()  # 文件不存在时 load() 会触发下载
    except api.ApiError as e:
        ui.error(f"下载失败：{e}")
        return None


def handle_pp_calc():
    diffman = _load_difficulties()
    if diffman is None:
        return

    if DifficultyManager.has_cache() and ui.confirm("是否更新难度数据库", default=False):
        try:
            with ui.spinner("更新难度数据库..."):
                diffman.update()
        except api.ApiError as e:
            ui.error(f"更新失败：{e}")

    difficulty = ui.ask(
        "难度", cast=str.upper, validate=_difficulty_validator(diffman), hint="(如 U15)"
    )
    marathon = ui.confirm("是否 Marathon", default=False)
    tilecount = ui.ask("关卡砖块数 t", default=0, cast=int)
    accuracy = ui.ask(
        "XACC",
        cast=float,
        validate=lambda v: None if 0 <= v <= 100 else "XACC 范围是 0 ~ 100",
        hint="(如 98.5)",
    )
    misses = ui.ask("空敲数 m", default=0, cast=int)
    speed = ui.ask("速度倍率", default=1.0, cast=float)
    no_hold = ui.confirm("是否禁用 Hold+Tap", default=False)

    calculator = TUFScoreCalculator()
    level_data = calculator.build_level_data(
        difficulty_name=difficulty, marathon=marathon, tilecount=tilecount
    )
    result = calculator.calculate_score(level_data, accuracy, misses, speed, no_hold)

    ui.blank()

    # base_source 为 default 表示这个难度在难度库里没有配 baseScore，
    # 此时 1000 是兜底值而非真实数据，必须标出来，否则结果会被当真。
    fallback_base = result["base_source"] == "default"
    if fallback_base:
        ui.warn(f"难度库中没有「{difficulty}」的 baseScore，基础分使用兜底值 1,000")
        ui.blank()
    base_style = "tuf.warn" if fallback_base else "tuf.value"

    ui.panel(
        "计算参数",
        [
            ("难度", "Marathon" if marathon else difficulty),
            ("关卡砖块数", tilecount),
            ("XACC", f"{result['accuracy_pct']}%"),
            ("空敲数", misses),
            ("速度倍率", speed),
            ("禁用 Hold+Tap", "是" if no_hold else "否"),
            ("基础分", f"{result['base_score']:,.2f}", base_style),
            ("分数倍率", f"{result['multiplier']}x"),
            ("速度修正", f"{result['speed_mod']}x"),
            ("空敲修正", f"{result['empty_tap_mod']}x"),
        ],
        columns=2,
    )
    ui.blank()
    ui.panel(
        None,
        [("PP 分", f"{result['score']:,.2f}", "tuf.good")],
        columns=1,
    )

    if ui.confirm("是否计算排名变化", default=False):
        calculate_rank_changes(result["score"])

    ui.blank()
    ui.api_stats(*api.take_stats())
