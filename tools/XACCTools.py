import sys
import numpy as np
from typing import Dict, List, Optional
from scipy.optimize import milp, Bounds, LinearConstraint
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
    keys = ["failMiss", "tooEarly", "early", "ePerfect", "perfect", "lPerfect", "late"]
    n_vars = len(keys)
    
    costs_dict = load()
    c = np.array([costs_dict[k] for k in keys], dtype=float)
    integrality = np.ones(n_vars)
    
    lower_bounds = np.zeros(n_vars)
    upper_bounds = np.ones(n_vars) * total
    for i, k in enumerate(keys):
        if k in fixed_counts:
            lower_bounds[i] = fixed_counts[k]
            upper_bounds[i] = fixed_counts[k]
            
    bounds = Bounds(lower_bounds, upper_bounds)
    tolerant_percent = 0.005 
    
    min_acc_percent = target_acc - tolerant_percent
    max_acc_percent = target_acc + tolerant_percent
    min_weighted_sum = (min_acc_percent / 100.0) * total
    max_weighted_sum = (max_acc_percent / 100.0) * total
    
    A = np.zeros((2, n_vars))
    A[0, :] = 1.0
    A[1, :] = [JD_WEIGHTS[k] for k in keys]
    
    lb = np.array([total, min_weighted_sum])
    ub = np.array([total, max_weighted_sum])
    
    constraints = LinearConstraint(A, lb, ub)
    
    try:
        res = milp(c=c, bounds=bounds, constraints=constraints, integrality=integrality)
    except Exception:
        return None
        
    if res.success:
        solution = {keys[i]: int(round(val)) for i, val in enumerate(res.x)}
        return solution
    else:
        return None
    
def calc(judgements: List[int]) -> float:
    if len(judgements) != 7 or sum(judgements) == 0: 
        return 0.0
    total = sum(judgements)
    
    keys = ["failMiss", "tooEarly", "early", "ePerfect", "perfect", "lPerfect", "late"]
    weighted_sum = sum(judgements[i] * JD_WEIGHTS[keys[i]] for i in range(7))
    return weighted_sum / total