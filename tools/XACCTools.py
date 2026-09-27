import math
from typing import Dict, List, Optional
from tools.CostsMan import load
from api import log


JD_WEIGHTS = {
    "failMiss": 0.0,  
    "tooEarly": 0.2,   
    "early": 0.4,
    "ePerfect": 0.75,
    "perfect": 1.0,
    "lPerfect": 0.75,
    "late": 0.4,
}

def reverse(target_acc: float, total: int, fixed_counts: Dict[str, int]) -> Optional[Dict[str, int]]:
    costs_dict = load()
    keys = ["failMiss", "tooEarly", "early", "ePerfect", "perfect", "lPerfect", "late"]
    weight_units = {k: round(JD_WEIGHTS[k] * 20) for k in keys}
    tolerant_percent = 0.005
    eps = 1e-9

    def fail(reason: str) -> Optional[Dict[str, int]]:
        log(f"[XACC/reverse] 无解: {reason}")
        return None

    log(f"[XACC/reverse] 开始: target={target_acc:.6f}%, total={total}, fixed={fixed_counts}")

    if total < 0 or not 0.0 <= target_acc <= 100.0:
        return fail("total 或 target_acc 超出范围")

    result = {k: 0 for k in keys}
    rem_notes = total
    fixed_score_units = 0
    fixed_cost = 0.0

    for k in keys:
        if k not in fixed_counts:
            continue
        val = fixed_counts[k]
        if not isinstance(val, int) or val < 0 or val > rem_notes:
            return fail(f"固定数量无效: {k}={val}")
        result[k] = val
        rem_notes -= val
        fixed_score_units += val * weight_units[k]
        fixed_cost += val * costs_dict[k]

    free_keys = [k for k in keys if k not in fixed_counts]
    if rem_notes and not free_keys:
        return fail("剩余物量大于 0，但没有可用的非固定判定")

    min_score = (target_acc - tolerant_percent) / 100.0 * total
    max_score = (target_acc + tolerant_percent) / 100.0 * total
    log(
        f"[XACC/reverse] fixed_score={fixed_score_units / 20:.4f}, "
        f"remaining={rem_notes}, target_range=[{min_score:.6f}, {max_score:.6f}]"
    )

    if not rem_notes:
        actual = fixed_score_units / 20.0
        if min_score - eps <= actual <= max_score + eps:
            log(f"[XACC/reverse] 固定判定已满足目标: score={actual:.6f}")
            return result
        return fail(f"固定判定分数 {actual:.6f} 不在目标区间")

    # 同权重的判定没有分数损失，直接选成本最低的作为基准，避免DP产生大量等价状态
    max_weight = max(weight_units[k] for k in free_keys)
    base_candidates = [k for k in free_keys if weight_units[k] == max_weight]
    base_key = min(base_candidates, key=lambda k: costs_dict[k])
    base_cost = costs_dict[base_key]
    base_score_units = fixed_score_units + rem_notes * max_weight

    # 把目标分数区间转换成允许的整数 loss 区间
    loss_min = math.ceil((base_score_units / 20.0 - max_score) * 20.0 - eps)
    loss_max = math.floor((base_score_units / 20.0 - min_score) * 20.0 + eps)
    loss_min = max(0, loss_min)
    log(
        f"[XACC/reverse] base={base_key}, base_score={base_score_units / 20:.4f}, "
        f"loss_range=[{loss_min}, {loss_max}] (单位=0.05)"
    )
    if loss_min > loss_max:
        return fail("最高可达分数也低于目标区间")

    loss_items = [
        (k, max_weight - weight_units[k], costs_dict[k] - base_cost)
        for k in free_keys
        if weight_units[k] < max_weight
    ]
    log(f"[XACC/reverse] loss_items={[(k, loss, cost) for k, loss, cost in loss_items]}")
    if not loss_items:
        if loss_min <= 0 <= loss_max:
            result[base_key] += rem_notes
            log(f"[XACC/reverse] 无需替换，全部使用 {base_key}")
            return result
        return fail("没有可用于降低分数的判定")

    # states[count][loss] = (额外成本, 各替代判定数量)
    # count 是替换 base_key 的数量，保证不会使用超过剩余物量
    states = [dict() for _ in range(rem_notes + 1)]
    states[0][0] = (0.0, (0,) * len(loss_items))
    progress_step = max(1, rem_notes // 10)
    for count in range(rem_notes):
        if not states[count]:
            continue
        for loss, (cost, layout) in states[count].items():
            for i, (_, item_loss, item_cost) in enumerate(loss_items):
                new_loss = loss + item_loss
                if new_loss > loss_max:
                    continue
                new_layout = list(layout)
                new_layout[i] += 1
                new_layout = tuple(new_layout)
                new_cost = cost + item_cost
                old = states[count + 1].get(new_loss)
                if old is None or new_cost < old[0] - eps:
                    states[count + 1][new_loss] = (new_cost, new_layout)
        if (count + 1) % progress_step == 0 or count + 1 == rem_notes:
            state_count = sum(len(layer) for layer in states[:count + 2])
            log(f"[XACC/reverse] DP {count + 1}/{rem_notes}, states={state_count}")

    best = None
    for count in range(rem_notes + 1):
        for loss, (extra_cost, layout) in states[count].items():
            if loss_min <= loss <= loss_max:
                candidate = (fixed_cost + (rem_notes - count) * base_cost + sum(
                    n * costs_dict[loss_items[i][0]] for i, n in enumerate(layout)
                ), count, loss, layout)
                if best is None or candidate[0] < best[0] - eps:
                    best = candidate

    if best is None:
        return fail("DP 没有找到落在目标区间内的分数损失")

    _, used, _, layout = best
    for i, amount in enumerate(layout):
        result[loss_items[i][0]] += amount
    result[base_key] += rem_notes - used
    final_score = sum(result[k] * JD_WEIGHTS[k] for k in keys)
    log(
        f"[XACC/reverse] 完成: loss={best[2]}, used_replacements={used}, "
        f"score={final_score:.6f}, cost={best[0]:.6f}, result={result}"
    )
    return result

def calc(judgements: List[int]) -> float:
    if len(judgements) != 7 or sum(judgements) == 0: 
        return 0.0
    total = sum(judgements)
    keys = ["failMiss", "tooEarly", "early", "ePerfect", "perfect", "lPerfect", "late"]
    weighted_sum = sum(judgements[i] * JD_WEIGHTS[keys[i]] for i in range(7))
    return weighted_sum / total
