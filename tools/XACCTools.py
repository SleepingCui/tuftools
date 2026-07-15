import math
from typing import Dict, List, Optional
from tools.CostsMan import load


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
    
    tolerant_percent = 0.005
    min_score = ((target_acc - tolerant_percent) / 100.0) * total
    max_score = ((target_acc + tolerant_percent) / 100.0) * total

    rem_notes = total
    fixed_score = 0.0
    fixed_cost = 0.0
    result = {k: 0 for k in keys}
    
    for k in keys:
        if k in fixed_counts:
            val = fixed_counts[k]
            if val > rem_notes:
                return None
            result[k] = val
            rem_notes -= val
            fixed_score += val * JD_WEIGHTS[k]
            fixed_cost += val * costs_dict[k]

    if rem_notes < 0:
        return None

    if rem_notes == 0:
        if min_score - 1e-9 <= fixed_score <= max_score + 1e-9:
            return result
        return None

    free_keys = [k for k in keys if k not in fixed_counts]
    if not free_keys:
        return None
    
    base_key = max(free_keys, key=lambda k: JD_WEIGHTS[k])
    base_w = JD_WEIGHTS[base_key]
    base_c = costs_dict[base_key]
    full_base_score = fixed_score + rem_notes * base_w
    full_base_cost = fixed_cost + rem_notes * base_c
    
    loss_items = []
    for k in free_keys:
        if k == base_key: continue
        w_loss = base_w - JD_WEIGHTS[k]         
        c_diff = costs_dict[k] - base_c        
        if w_loss > 0: 
            loss_items.append({"key": k, "w_loss": w_loss, "c_diff": c_diff})
    loss_items.sort(key=lambda x: (x["c_diff"], -x["w_loss"]))

    best_total_cost = float('inf')
    best_loss_layout = None

    if full_base_score < min_score - 1e-9:
        return None

    if min_score - 1e-9 <= full_base_score <= max_score + 1e-9:
        for k in free_keys:
            result[k] += rem_notes if k == base_key else 0
        return result

    target_loss_min = full_base_score - max_score
    target_loss_max = full_base_score - min_score

    n_items = len(loss_items)
    if n_items == 0:
        return None

    def solve_loss():
        nonlocal best_total_cost, best_loss_layout
        
        for i in range(n_items):
            item = loss_items[i]
            v_min = max(0, math.ceil((target_loss_min - 1e-9) / item["w_loss"]))
            v_max = min(rem_notes, math.floor((target_loss_max + 1e-9) / item["w_loss"]))
            
            for v in range(v_min, v_max + 1):
                cur_loss = v * item["w_loss"]
                if target_loss_min - 1e-9 <= cur_loss <= target_loss_max + 1e-9:
                    added_cost = v * item["c_diff"]
                    if added_cost < best_total_cost:
                        best_total_cost = added_cost
                        best_loss_layout = {item["key"]: v}

        if n_items >= 2:
            for i in range(n_items):
                item1 = loss_items[i]
                limit1 = min(rem_notes, math.floor((target_loss_max + 1e-9) / item1["w_loss"]))
                for v1 in range(limit1 + 1):
                    loss_left_min = target_loss_min - v1 * item1["w_loss"]
                    loss_left_max = target_loss_max - v1 * item1["w_loss"]
                    
                    for j in range(i + 1, n_items):
                        item2 = loss_items[j]

                        v2_min = max(0, math.ceil((loss_left_min - 1e-9) / item2["w_loss"]))
                        v2_max = min(rem_notes - v1, math.floor((loss_left_max + 1e-9) / item2["w_loss"]))
                        
                        for v2 in range(v2_min, v2_max + 1):
                            total_loss = v1 * item1["w_loss"] + v2 * item2["w_loss"]
                            if target_loss_min - 1e-9 <= total_loss <= target_loss_max + 1e-9:
                                added_cost = v1 * item1["c_diff"] + v2 * item2["c_diff"]
                                if added_cost < best_total_cost:
                                    best_total_cost = added_cost
                                    best_loss_layout = {item1["key"]: v1, item2["key"]: v2}

    solve_loss()

    if best_loss_layout is None:
        return None

    used_notes = 0
    for k, v in best_loss_layout.items():
        result[k] += v
        used_notes += v
        
    result[base_key] += (rem_notes - used_notes)
    return result

def calc(judgements: List[int]) -> float:
    if len(judgements) != 7 or sum(judgements) == 0: 
        return 0.0
    total = sum(judgements)
    keys = ["failMiss", "tooEarly", "early", "ePerfect", "perfect", "lPerfect", "late"]
    weighted_sum = sum(judgements[i] * JD_WEIGHTS[keys[i]] for i in range(7))
    return weighted_sum / total