import time

import ui
from tools.CostsMan import load, save
from tools.XACCTools import JD_WEIGHTS, calc, reverse


JD_KEYS = ["failMiss", "tooEarly", "early", "ePerfect", "perfect", "lPerfect", "late"]


def _validate_seven_ints(value):
    parts = value.split()
    if len(parts) != 7:
        return f"需要 7 个数字，收到 {len(parts)} 个"
    try:
        counts = [int(p) for p in parts]
    except ValueError:
        return "请全部输入整数"
    if any(count < 0 for count in counts):
        return "判定数量不能为负数"
    return None


def run1():
    ui.info("输入格式: failMiss tooEarly early ePerfect perfect lPerfect late")

    values = ui.ask("判定数据", validate=_validate_seven_ints, hint="(7 个整数)")
    judgements = [int(x) for x in values.split()]

    rows = [
        [key, str(count), str(JD_WEIGHTS[key])]
        for key, count in zip(JD_KEYS, judgements)
    ]
    ui.blank()
    ui.table(["判定", ("数量", "right"), ("权重", "right")], rows, title="判定构成")
    ui.blank()
    ui.panel(None, [("XACC", f"{calc(judgements) * 100}%", "tuf.good")], columns=1)


def run2():
    total = ui.ask("物量", cast=int, validate=lambda v: None if v > 0 else "物量必须大于 0")
    target_acc = ui.ask(
        "XACC",
        cast=float,
        validate=lambda v: None if 0 <= v <= 100 else "XACC 范围是 0 ~ 100",
        hint="(如 98.5)",
    )

    ui.blank()
    ui.info("以下各项直接回车表示不固定")
    fixed_counts = {}
    for key in JD_KEYS:
        value = ui.ask(f"固定 {key} 的数量", cast=int, allow_empty=True)
        if value is not None:
            fixed_counts[key] = value

    with ui.spinner("推算判定中，这可能需要一些时间..."):
        t_start = time.perf_counter()
        result = reverse(target_acc, total, fixed_counts)
        elapsed = (time.perf_counter() - t_start) * 1000

    ui.blank()
    if result is None:
        ui.error("无法达成该目标 XACC")
    else:
        rows = [
            [key, str(result[key]), "(Locked)" if key in fixed_counts else "", str(JD_WEIGHTS[key])]
            for key in JD_KEYS
        ]
        ui.table(["判定", ("数量", "right"), "", ("权重", "right")], rows, title="推算结果")

        actual_acc = sum(result[k] * JD_WEIGHTS[k] for k in JD_KEYS) / total * 100
        ui.blank()
        ui.panel(None, [("XACC", f"{actual_acc}%", "tuf.good")], columns=1)

    ui.info(f"耗时 {elapsed:.0f} ms")


def run3():
    current_costs = load()

    ui.blank()
    ui.info("当前各判定难度系数（数值越低，算法越倾向于用它凑分）")
    ui.table(["判定", ("系数", "right")], [[k, str(v)] for k, v in current_costs.items()])

    ui.blank()
    ui.info("输入新系数，直接回车保持不变")
    modified = False
    for key in list(current_costs.keys()):
        value = ui.ask(key, default=current_costs[key], cast=float)
        if value != current_costs[key]:
            current_costs[key] = value
            modified = True

    ui.blank()
    if modified:
        save(current_costs)
        ui.success("保存成功")
    else:
        ui.info("未修改")


def handle_acc_calc():
    while True:
        choice = ui.menu(
            "XACC 计算器",
            [
                ("1", "根据判定计算 XACC"),
                ("2", "根据 XACC 推算判定"),
                ("3", "自定义难度系数"),
                ("b", "返回主菜单"),
            ],
        )

        if choice == "1":
            run1()
        elif choice == "2":
            run2()
        elif choice == "3":
            run3()
        elif choice == "b":
            break
