from typing import List

JUDGEMENT_WEIGHTS = {
    "miss": 0.2,
    "early": 0.4,
    "ePerfect": 0.75,
    "perfect": 1.0,
    "lPerfect": 0.75,
    "late": 0.4,
}


def calcacc(judgements: List[int]) -> float:
    if len(judgements) != 6 or sum(judgements) == 0: return 0.0
    total = sum(judgements)
    weighted_sum = judgements[0] * JUDGEMENT_WEIGHTS["miss"] + judgements[1] * JUDGEMENT_WEIGHTS["early"] + judgements[2] * JUDGEMENT_WEIGHTS["ePerfect"] + judgements[3] * JUDGEMENT_WEIGHTS["perfect"] + judgements[4] * JUDGEMENT_WEIGHTS["lPerfect"] + judgements[5] * JUDGEMENT_WEIGHTS["late"]
    return weighted_sum / total

def handle_acc_calc():
    print("输入判定:")
    print("格式: miss early EPerfect perfect LPerfect late")

    values = input("\n判定数据: ").strip().split()

    if len(values) != 6:
        print("错误：需要输入6个数字")
        return

    try:
        judgements = [int(x) for x in values]
    except ValueError:
        print("错误：请输入整数")
        return
    
    print(f"XACC: {calcacc(judgements) * 100}%")