"""统一的终端呈现层。

这是项目里唯一调用 print() / input() 的模块。业务模块只调用这里的语义化原语，
由本模块在 Rich 与纯文本两套后端之间切换：

- rich  : 圆角面板、彩色表格、加载动画
- plain : 与改造前基本一致的纯文本输出

后端由 init() 在启动时确定一次（见 main.py 的 --ui/--plain/--rich）。
plain 后端同时是 rich 缺失时的兜底，保证装不上依赖也还能用。
"""

from __future__ import annotations

import difflib
import os
import sys
import threading
from contextlib import contextmanager
from typing import Any, Callable, Iterable, Literal, Sequence

try:
    from rich import box as _box
    from rich.console import Console
    from rich.panel import Panel
    from rich.table import Table
    from rich.text import Text
    from rich.theme import Theme

    _RICH_INSTALLED = True
except ImportError:  # pragma: no cover - 依赖缺失时的降级路径
    _RICH_INSTALLED = False


__all__ = [
    "BACK",
    "init",
    "is_rich",
    "banner",
    "menu",
    "ask",
    "confirm",
    "panel",
    "table",
    "player_panel",
    "success",
    "error",
    "warn",
    "info",
    "spinner",
    "api_stats",
    "rule",
    "blank",
    "debug",
    "suggest",
    "fmt_score",
    "fmt_rank",
]


class BackType:
    """BACK 哨兵的类型。

    用专门的单例类型而不是裸 object()，这样类型检查器能在调用方把
    `dict | None | BackType` 收窄成 `dict | None`。
    """

    _instance: "BackType | None" = None

    def __new__(cls) -> "BackType":
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    def __repr__(self) -> str:
        return "BACK"


#: ask(allow_back=True) 在用户输入 b 时返回的哨兵值。
#: 用它而不是 None，是因为 None 本身也可能是合法返回值。
BACK = BackType()

#: 列对齐方式，与 rich 的 JustifyMethod 一致。
Justify = Literal["default", "left", "center", "right", "full"]


# 所有装饰字形都限定在 GBK 可编码的字符集内（√ × ※ · — ▲ ▼），因为中文 Windows
# 控制台的默认编码是 GBK，✔ ✖ ⚠ ❯ 这类字形会显示成 ? 。制表符 ╭╮╰╯─│ 同样在
# GBK 内，所以边框可以安全使用。


THEME = Theme(
    {
        "tuf.title": "bold cyan",
        "tuf.accent": "cyan",
        "tuf.muted": "dim",
        "tuf.label": "bold",
        "tuf.value": "default",
        "tuf.good": "green",
        "tuf.bad": "red",
        "tuf.warn": "yellow",
    }
)

# --- 模块状态 -------------------------------------------------------------

_use_rich = False
_debug = False
#: 是否允许播放加载动画。只在输出真的是终端时才播：--rich 会让 rich 把输出
#: 当作终端，但管道/重定向下逐帧的动画不会被覆盖，只会堆成一长串。
_animate = False
_console: "Console | None" = None
_lock = threading.Lock()


def init(mode: str = "auto", debug: bool = False) -> None:
    """选择后端。mode ∈ {"auto", "rich", "plain"}。"""
    global _use_rich, _debug, _animate, _console

    _debug = debug
    _animate = sys.stdout.isatty() and not debug
    _force_utf8()

    if mode == "plain":
        _use_rich = False
    elif mode == "rich":
        _use_rich = _RICH_INSTALLED
    else:  # auto：非终端或 NO_COLOR 时退回纯文本
        _use_rich = (
            _RICH_INSTALLED
            and sys.stdout.isatty()
            and not os.environ.get("NO_COLOR")
        )

    if _use_rich:
        # mode == "rich" 时强制输出 ANSI，这样 `tuftools --rich | less -R` 才有颜色。
        # safe_box=False 是必需的：中文 Windows 的区域编码是 GBK，rich 会因此判定
        # 为 legacy 终端并把圆角边框全部替换成 ASCII（+--+）。而 GBK 其实完全装得下
        # 制表符，这个降级是误判，关掉即可。
        # markup=False 是安全保证：玩家名、异常信息、URL 这些都是不可控文本，
        # 一旦被当成标记解析，轻则显示错乱（[b]x[/b] 变成加粗的 x），
        # 重则 MarkupError 直接把程序打崩（名字里带 [/] 就会）。
        # 本模块所有样式一律通过 Text(style=...) 施加，不依赖标记字符串。
        _console = Console(
            theme=THEME,
            markup=False,
            highlight=False,
            emoji=False,
            safe_box=False,
            force_terminal=True if mode == "rich" else None,
        )
    else:
        _console = None


def _force_utf8() -> None:
    """把标准输出切到 UTF-8。

    中文 Windows 下 stdout 被重定向时编码是 GBK，而 ✔ ✖ 这类字形 GBK 装不下，
    print 会直接抛 UnicodeEncodeError 崩掉。errors="replace" 保证最坏情况只是
    显示成 ? ，不会中断程序。
    """
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:
            continue
        try:
            reconfigure(encoding="utf-8", errors="replace")
        except (ValueError, OSError):
            pass


def is_rich() -> bool:
    return _use_rich


# --- 内部工具 -------------------------------------------------------------


def _out(*renderables: Any) -> None:
    if _console is not None:
        _console.print(*renderables)
    else:
        print(*renderables)


def _read_line(prompt: str, rich_prompt: "Text | None" = None) -> str:
    """读一行输入。Ctrl+C 时 KeyboardInterrupt 会向上抛，由 main 统一收尾。"""
    if _console is not None:
        return _console.input(rich_prompt if rich_prompt is not None else prompt)
    return input(prompt)


def _style(text: str, style: str) -> "Text | str":
    return Text(text, style=style) if _console is not None else text


def _chunk(seq: Sequence[Any], size: int) -> Iterable[Sequence[Any]]:
    for i in range(0, len(seq), size):
        yield seq[i : i + size]


def _cell(value: Any) -> tuple[str, str | None]:
    """单元格可以是 str，也可以是 (str, style)。"""
    if isinstance(value, tuple):
        return str(value[0]), value[1]
    return str(value), None


# --- 数值格式化 -----------------------------------------------------------


def fmt_score(value: Any, digits: int = 2) -> str:
    if value is None or value == "":
        return "—"
    try:
        return f"{float(value):,.{digits}f}"
    except (TypeError, ValueError):
        return str(value)


def fmt_rank(value: Any) -> str:
    if value is None or value == "":
        return "?"
    try:
        return f"#{int(value):,}"
    except (TypeError, ValueError):
        return str(value)


# --- 基础原语 -------------------------------------------------------------


def banner() -> None:
    if _console is None:
        print("=== TUF Tools ===")
        print("Github: github.com/sleepingcui/tuftools TUF: tuforums.com")
        return

    body = Text(justify="center")
    body.append("github.com/sleepingcui/tuftools", style="tuf.accent")
    body.append("   ·   ", style="tuf.muted")
    body.append("tuforums.com", style="tuf.accent")
    _console.print(
        Panel(
            body,
            title=Text("TUF Tools", style="tuf.title"),
            box=_box.ROUNDED,
            border_style="tuf.accent",
            padding=(0, 2),
            expand=False,
        )
    )


def menu(title: str, items: Sequence[Sequence[str]]) -> str:
    """渲染菜单并返回被选中的 key。

    items 每项为 (key, label) 或 (key, label, hint)。
    非法输入会重绘菜单后重问，不抛异常。
    """
    options = [(i[0], i[1], i[2] if len(i) > 2 else None) for i in items]
    keys = {o[0] for o in options}

    while True:
        blank()
        if title:
            _out(_style(title, "tuf.title"))

        if _console is None:
            for key, label, hint in options:
                line = f"{key}. {label}"
                if hint:
                    line += f"   {hint}"
                print(line)
        else:
            for key, label, hint in options:
                line = Text()
                line.append(f"{key:>2}  ", style="tuf.accent")
                line.append(label, style="tuf.value")
                if hint:
                    line.append(f"   {hint}", style="tuf.muted")
                _console.print(line, highlight=False)

        raw = _read_line("> ").strip()
        if raw in keys:
            return raw
        # 字母键不区分大小写，输 Q 和 q 效果一样
        if raw.lower() in keys:
            return raw.lower()
        error("无效选择" if not raw else f"无效选择：{raw}")


def ask(
    label: str,
    default: Any = None,
    cast: "Callable[[str], Any] | None" = None,
    choices: Sequence[str] | None = None,
    allow_back: bool = False,
    allow_empty: bool = False,
    validate: "Callable[[Any], str | None] | None" = None,
    hint: str | None = None,
) -> Any:
    """收集一个值，校验失败时重问而不是抛异常。

    - default      : 输入为空时的返回值
    - cast         : 类型转换（如 int / float）
    - choices      : 限定取值集合
    - allow_back   : 允许输入 b，返回 BACK 哨兵
    - allow_empty  : 允许留空，此时返回 None（用于"跳过这一项"）
    - validate     : 返回错误字符串表示不通过
    """
    suffix = f" (默认 {default})" if default is not None else ""
    if hint:
        suffix += f" {hint}"

    while True:
        if _console is None:
            prompt = f"{label}{suffix}: "
            raw = _read_line(prompt).strip()
        else:
            rich_prompt = Text()
            rich_prompt.append(f"{label}{suffix}", style="tuf.label")
            rich_prompt.append(": ", style="tuf.label")
            raw = _read_line("", rich_prompt).strip()

        if raw == "" and default is not None:
            return default

        if allow_back and raw.lower() == "b":
            return BACK

        if raw == "":
            if allow_empty:
                return None
            error("不能为空")
            continue

        if choices is not None and raw not in choices:
            error(f"必须是 {'/'.join(map(str, choices))} 之一")
            continue

        value = raw
        if cast is not None:
            try:
                value = cast(raw)
            except (TypeError, ValueError):
                error(f"无法解析为{'整数' if cast is int else '数字'}：{raw}")
                continue

        if validate is not None:
            message = validate(value)
            if message:
                error(message)
                continue

        return value


def confirm(label: str, default: bool = False) -> bool:
    hint = "Y/n" if default else "y/N"
    while True:
        if _console is None:
            raw = _read_line(f"{label} ({hint}): ").strip().lower()
        else:
            prompt = Text()
            prompt.append(f"{label} ", style="tuf.label")
            prompt.append(f"({hint})", style="tuf.muted")
            prompt.append(": ", style="tuf.label")
            raw = _read_line("", prompt).strip().lower()

        if raw == "":
            return default
        if raw in ("y", "yes"):
            return True
        if raw in ("n", "no"):
            return False
        error("请输入 y 或 n")


def panel(
    title: str | None,
    pairs: Sequence[Sequence[Any]],
    columns: int = 1,
    subtitle: str | None = None,
) -> None:
    """键值面板。pairs 每项为 (label, value) 或 (label, value, style)。

    columns 只在 rich 后端生效；plain 后端始终一行一个键值，
    与改造前的输出保持一致。
    """
    if _console is None:
        if title:
            print(title)
        for item in pairs:
            print(f"{item[0]}: {item[1]}")
        return

    grid = Table.grid(padding=(0, 2))
    for _ in range(columns * 2):
        grid.add_column()

    for group in _chunk(pairs, columns):
        cells: list[Any] = []
        for item in group:
            label, value = item[0], item[1]
            style = item[2] if len(item) > 2 else "tuf.value"
            cells.append(Text(str(label), style="tuf.label"))
            cells.append(value if isinstance(value, Text) else Text(str(value), style=style))
        for _ in range(columns - len(group)):
            cells.append(Text(""))
            cells.append(Text(""))
        grid.add_row(*cells)

    _console.print(
        Panel(
            grid,
            # 这里曾经是 f"[tuf.title]{title}[/tuf.title]"。标题里可能带玩家名等
            # 不可控文本，插值进标记字符串会被当标记解析甚至抛 MarkupError，
            # 必须是 Text 对象。
            title=Text(str(title), style="tuf.title") if title else None,
            subtitle=Text(str(subtitle), style="tuf.muted") if subtitle else None,
            box=_box.ROUNDED,
            border_style="tuf.accent",
            padding=(0, 1),
            expand=False,
        )
    )


def table(
    columns: Sequence[Any],
    rows: Sequence[Sequence[Any]],
    title: str | None = None,
) -> None:
    """表格。columns 每项为表头字符串或 (表头, 对齐)。

    单元格为 str，或 (str, style) 元组；style 只在 rich 后端生效。
    """
    headers: list[str] = []
    aligns: list[Justify] = []
    for col in columns:
        if isinstance(col, tuple):
            headers.append(str(col[0]))
            aligns.append(col[1])
        else:
            headers.append(str(col))
            aligns.append("left")

    if _console is None:
        if title:
            print(title)
        if not headers:
            return
        widths = [len(h) for h in headers]
        for row in rows:
            for i in range(len(headers)):
                text = _cell(row[i])[0] if i < len(row) else ""
                widths[i] = max(widths[i], len(text))

        def render(values: Sequence[str]) -> str:
            parts = []
            for i, value in enumerate(values):
                parts.append(value.rjust(widths[i]) if aligns[i] == "right" else value.ljust(widths[i]))
            return "  ".join(parts).rstrip()

        print(render(headers))
        print("  ".join("-" * w for w in widths))
        for row in rows:
            values = [_cell(row[i])[0] if i < len(row) else "" for i in range(len(headers))]
            print(render(values))
        return

    # 标题自己画而不用 Table 的 title：rich 会给带标题的表格补上下空行。
    if title:
        _out(_style(title, "tuf.title"))

    # box=None：SIMPLE_HEAD 之类的边框会在表格上下各留一行纯空白（还带行尾空格），
    # 表头本身就是加粗彩色的，分隔线是多余的。
    rich_table = Table(
        box=None,
        header_style="tuf.title",
        padding=(0, 1),
        pad_edge=False,
    )
    for header, align in zip(headers, aligns):
        rich_table.add_column(header, justify=align)

    for row in rows:
        cells = []
        for i in range(len(headers)):
            text, style = _cell(row[i]) if i < len(row) else ("", None)
            cells.append(Text(text, style=style or "tuf.value"))
        rich_table.add_row(*cells)

    _console.print(rich_table)


def player_panel(player: dict, ranks: dict, metrics: Sequence[tuple[str, str]]) -> None:
    """玩家详情：头部信息面板 + 指标表 + 统计行。

    metrics 为 (显示名, 分数键) 序列，顺序即表格行序；
    排名从 ranks 的 f"{分数键}_global" / f"{分数键}_country" 取。
    """
    name = player.get("name") or "?"
    pid = player.get("id")
    discord = player.get("discord") or {}
    country = player.get("country") or "?"
    avg_xacc = player.get("averageXacc")
    top_diff = player.get("topDiff") or {}

    header = [
        ("国家", country),
        ("Discord", discord.get("username") or "?"),
    ]
    if avg_xacc is not None:
        header.append(("平均 XACC", f"{avg_xacc * 100:.2f}%"))
    if top_diff:
        header.append(("最高通关", f"{top_diff.get('name')} ({top_diff.get('sortOrder')})"))

    panel(f"{name}  ·  #{pid}", header, columns=2)

    rows = []
    for label, score_key in metrics:
        rows.append(
            [
                label,
                fmt_score(player.get(score_key)),
                fmt_rank(ranks.get(f"{score_key}_global")),
                fmt_rank(ranks.get(f"{score_key}_country")),
            ]
        )
    table(["指标", ("分数", "right"), ("全球排名", "right"), ("国家排名", "right")], rows)

    stats = [
        ("U级通关", player.get("universalPassCount")),
        ("总通关", player.get("totalPasses")),
        ("世界首通", player.get("worldsFirstCount")),
        ("世界首杀", player.get("worldsFirstPPCount")),
    ]
    stats_text = _style("  ·  ".join(f"{k} {v}" for k, v in stats), "tuf.muted")
    _out(stats_text)


# --- 消息 -----------------------------------------------------------------


def _message(glyph: str, style: str, message: str) -> None:
    if _console is None:
        print(message)
        return
    line = Text()
    line.append(f"{glyph} ", style=style)
    line.append(message, style="tuf.value")
    _console.print(line, highlight=False)


def success(message: str) -> None:
    _message("√", "tuf.good", message)


def error(message: str) -> None:
    _message("×", "tuf.bad", message)


def warn(message: str) -> None:
    _message("※", "tuf.warn", message)


def info(message: str) -> None:
    _message("·", "tuf.accent", message)


def blank() -> None:
    if _console is not None:
        _console.print()
    else:
        print()


def rule(title: str | None = None) -> None:
    if _console is not None:
        _console.rule(f"[tuf.muted]{title}[/tuf.muted]" if title else "", style="tuf.muted")
    else:
        print("-" * 70)


def debug(message: str) -> None:
    """诊断信息，只在 --debug 时输出。"""
    if not _debug:
        return
    with _lock:
        _out(_style(message, "tuf.muted"))


@contextmanager
def spinner(text: str):
    """慢操作的加载反馈。没有真终端时只把文字打一次，不做动画。

    --debug 时也不做动画：调试日志可能来自排名查询的线程池，
    和 status 的活动区域抢同一块屏幕会互相覆盖。
    """
    if _console is None or not _animate:
        print(text)
        yield
        return
    with _console.status(Text(text, style="tuf.muted"), spinner="dots"):
        yield


def api_stats(count: int, cumulative_ms: float, wall_ms: float = 0.0) -> None:
    """API 调用统计。

    并发查询下，各请求耗时之和会大于用户实际等待的墙钟时间，
    所以两个数都给出来，避免看起来自相矛盾。
    """
    if count == 0:
        return  # 本次没有发请求，打一行 "0 请求" 只是噪音

    if wall_ms > 0:
        line = f"{count} 请求 · 累计 {cumulative_ms:,.0f} ms · 实际等待 {wall_ms:,.0f} ms"
    else:
        line = f"{count} 请求 · 累计 {cumulative_ms:,.0f} ms"

    if _console is None:
        wall = f" (wall {wall_ms:.2f} ms)" if wall_ms > 0 else ""
        print(f"used {count} requests, {cumulative_ms:.2f} ms{wall}")
        print("\n" + "=" * 70)
        return
    _out(_style(line, "tuf.muted"))
    _out(_style("=" * 70, "tuf.muted"))


# --- 拼写建议 -------------------------------------------------------------


def suggest(text: str, candidates: Iterable[str], n: int = 3, cutoff: float = 0.5) -> list[str]:
    """从候选里挑出与 text 最接近的几个，用于输入纠错提示。"""
    return difflib.get_close_matches(text, list(candidates), n=n, cutoff=cutoff)
