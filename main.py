import argparse

import ui
from acccalc import handle_acc_calc
from api import set_proxies
from info import handle_player_lookup
from ppcalc import handle_pp_calc


MENU = [
    ("1", "玩家数据查询"),
    ("2", "PP 计算器"),
    ("3", "XACC 计算器"),
    ("q", "退出"),
]


def parse_args():
    parser = argparse.ArgumentParser(
        prog="tuftools",
        description="TUF 玩家数据查询 / PP 分计算 / XACC 推算",
    )
    parser.add_argument("--proxy", type=str, help="http proxy URL")

    backend = parser.add_mutually_exclusive_group()
    backend.add_argument(
        "--plain",
        action="store_true",
        help="纯文本输出（等价于 --ui plain）",
    )
    backend.add_argument(
        "--rich",
        action="store_true",
        help="强制彩色输出（等价于 --ui rich），管道下也保留 ANSI 颜色",
    )
    parser.add_argument(
        "--ui",
        choices=["auto", "rich", "plain"],
        default="auto",
        help="界面后端。auto 会在输出不是终端或设置了 NO_COLOR 时自动退回纯文本（默认）",
    )
    parser.add_argument("--debug", action="store_true", help="输出 API 请求 URL 等诊断信息")
    return parser.parse_args()


def main():
    args = parse_args()

    mode = "plain" if args.plain else "rich" if args.rich else args.ui
    ui.init(mode, debug=args.debug)

    if args.proxy:
        set_proxies(args.proxy)

    ui.banner()

    try:
        while True:
            choice = ui.menu("选择功能系统", MENU)

            if choice == "1":
                handle_player_lookup()
            elif choice == "2":
                handle_pp_calc()
            elif choice == "3":
                handle_acc_calc()
            elif choice == "q":
                ui.info("已退出")
                break
    except KeyboardInterrupt:
        ui.blank()
        ui.info("已中断，退出")
    except EOFError:
        ui.blank()
        ui.info("输入结束，退出")


if __name__ == "__main__":
    main()
