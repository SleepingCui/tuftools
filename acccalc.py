from typing import Dict, List, Optional
from tools.XACCTools import calc, reverse, JD_WEIGHTS
from tools.CostsMan import load, save
def run1():
    print("输入格式: failMiss tooEarly early EPerfect perfect LPerfect late")

    values = input("判定数据: ").strip().split()

    if len(values) != 7:
        print("需要输入7个数字")
        return

    try:
        judgements = [int(x) for x in values]
    except ValueError:
        print("请输入整数")
        return
    print()
    print(f"XACC: {calc(judgements) * 100}%")


def run2():
    try:
        total = int(input("物量: ").strip())
        target_acc = float(input("XACC: ").strip())
    except ValueError as e:
        print(e)
        return

    if target_acc < 0.0 or target_acc > 100.0:
        print(f"XACC 范围是 0.0% ~ 100.0%")
        return

    keys = ["failMiss", "tooEarly", "early", "ePerfect", "perfect", "lPerfect", "late"]
    fixed_counts = {}
    for k in keys:
        val = input(f"固定 {k} 的数量 (直接回车表示不固定): ").strip()
        if val:
            try:
                fixed_counts[k] = int(val)
            except ValueError:
                print(f"已跳过固定 {k}")

    result = reverse(target_acc, total, fixed_counts)

    print()

    if result is None:
        print("无法达成该目标 XACC")
    else:
        for k in keys:
            status = "(Locked)" if k in fixed_counts else ""
            print(f" {k:<10}: {result[k]} {status}")
        
        weighted_sum = sum(result[k] * JD_WEIGHTS[k] for k in keys)
        calc_acc = (weighted_sum / total) * 100
        print(f"\n XACC: {calc_acc}%")


def run3():
    current_costs = load()
    
    print("当前各判定难度系数 (数值越低，算法越倾向于用它凑分):")
    for k, v in current_costs.items():
        print(f"  {k:<10}: {v}")
    
    print("\n请输入新系数 (直接回车保持不变):")
    modified = False
    for k in current_costs.keys():
        val = input(f"{k} [{current_costs[k]}]: ").strip()
        if val:
            try:
                current_costs[k] = float(val)
                modified = True
            except ValueError as e:
                print(f"输入无效，{k} 保持原值: {e}")
    
    print()
    if modified:
        save(current_costs)
        print("保存成功")
    else:
        print("Not modified")


def handle_acc_calc():
    print()
    while True:
        print()
        print("1. 根据判定计算 XACC")
        print("2. 根据 XACC 推算判定")
        print("3. 自定义难度系数")
        print("b. 返回主菜单")
        
        choice = input("> ").strip()
        
        if choice == "1":
            run1()
        elif choice == "2":
            run2()
        elif choice == "3":
            run3()
        elif choice == "b":
            break
        else:
            print("无效选择")