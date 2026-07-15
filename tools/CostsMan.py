import os
import json
from typing import Dict

DF_JD_COSTS = {
    "perfect": 0.0,
    "failMiss": 1.0,   
    "tooEarly": 1.5,   
    "early": 50.0,
    "late": 50.0,
    "ePerfect": 100.0,
    "lPerfect": 100.0,
}

file = "costs.json"
def load() -> Dict[str, float]:
    if os.path.exists(file):
        try:
            with open(file, "r", encoding="utf-8") as f:
                costs = json.load(f)
                return {k: float(costs.get(k, DF_JD_COSTS[k])) for k in DF_JD_COSTS}
        except Exception as e:
            print(f"读取配置文件失败!使用默认值: {e}")
            return DF_JD_COSTS.copy()
    else:
        save(DF_JD_COSTS)
        return DF_JD_COSTS.copy()

def save(costs: Dict[str, float]):
    try:
        with open(file, "w", encoding="utf-8") as f:
            json.dump(costs, f, indent=4, ensure_ascii=False)
    except Exception as e:
        print(f"保存配置文件失败: {e}")