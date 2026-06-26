from typing import Dict, List, Tuple
from api import fetchapi, stats, BASE_URL
import json

class TUFScoreCalculator:  
    XACC_DEFAULT = {
        "cutoff": 0.95,
        "topMultiplier": 5.51289781,
        "poleOffset": 0.0054017154
    }
    
    JUDGEMENT_WEIGHTS = {
        "miss": 0.2, "early": 0.4, "ePerfect": 0.75, "perfect": 1.0, "lPerfect": 0.75, "late": 0.4
    }
    
    JUDGEMENT_ORDER = ["miss", "early", "ePerfect", "perfect", "lPerfect", "late"]
    
    def __init__(self, base_url: str = "https://api.tuforums.com"):
        self.base_url = base_url
    
    def get_level_data(self, level_id: int) -> Dict:
        url = f"{self.base_url}/v2/database/levels/{level_id}"
        return fetchapi(url)
    
    def calculate_accuracy(self, judgements: List[int]) -> float:
        if not judgements or sum(judgements) == 0:
            return 0.0
        
        total = sum(judgements)
        weights = self.JUDGEMENT_WEIGHTS
        
        weighted_sum = judgements[0] * weights["miss"] + judgements[1] * weights["early"] + judgements[2] * weights["ePerfect"] + judgements[3] * weights["perfect"] + judgements[4] * weights["lPerfect"] + judgements[5] * weights["late"]

        return round(weighted_sum / total, 4)
    
    def get_xacc_curve(self, level_data: Dict) -> Dict:
        level = level_data.get("level", level_data)
        xacc_curve = level.get("xaccCurve")
        if xacc_curve:
            return {
                "poleOffset": xacc_curve.get("poleOffset", self.XACC_DEFAULT["poleOffset"]),
                "topMultiplier": xacc_curve.get("topMultiplier", self.XACC_DEFAULT["topMultiplier"])
            }
        xacc_meta = level.get("xaccCurveMeta")
        if xacc_meta:
            try:
                meta = json.loads(xacc_meta) if isinstance(xacc_meta, str) else xacc_meta
                return {
                    "poleOffset": meta.get("poleOffset", self.XACC_DEFAULT["poleOffset"]),
                    "topMultiplier": meta.get("topMultiplier", self.XACC_DEFAULT["topMultiplier"])
                }
            except:
                pass
        return self.XACC_DEFAULT.copy()
    
    def get_base_score(self, level_data: Dict, accuracy: float) -> Tuple[float, str]:
        level = level_data.get("level", level_data)
        difficulty = level.get("difficulty", {})
        
        if accuracy >= 0.9999:
            pp_base = level.get("ppBaseScore", 0)
            if pp_base: return pp_base, "ppBaseScore"
        
        base = level.get("baseScore", 0)
        if base: return base, "baseScore"
        
        diff_base = difficulty.get("baseScore", 0)
        if diff_base: return diff_base, "difficulty.baseScore"
        
        return 1000, "default"
    
    def calculate_score_multiplier(self, accuracy: float, base_score: float, xacc_curve: Dict) -> float:
        cutoff = self.XACC_DEFAULT["cutoff"]
        top_mult = xacc_curve.get("topMultiplier", self.XACC_DEFAULT["topMultiplier"])
        pole_offset = xacc_curve.get("poleOffset", self.XACC_DEFAULT["poleOffset"])
        
        acc_pct = accuracy * 100
        if acc_pct < cutoff * 100: return 1.0
        if acc_pct >= 99.999:
            if base_score > 0: return max(1.0, -2100 / (base_score + 262.5) + 14)
            return 1.0
        
        span = 1 - cutoff
        normalized = max(0, min(1, (accuracy - cutoff) / span))
        
        t = 1 - cutoff
        G = top_mult
        E = pole_offset
        
        A = (G - 1) * E * (t + E) / t
        B = 1 - A / (t + E)
        
        denom = t * (normalized - 1) - E
        if abs(denom) < 1e-14: return 1.0
        
        z = max(0, min(1, (B - A / denom - 1) / (G - 1)))
        return max(1.0, 1 + (G - 1) * z)
    
    def calculate_speed_modifier(self, speed: float, is_marathon: bool = False) -> float:
        if is_marathon:
            if speed <= 1: return 1.0
            return max(0, 2 - speed)
        
        if speed <= 1: return 1.0
        elif speed < 1.1: return -3.5 * speed + 4.5
        elif speed < 1.5: return 0.65
        elif speed < 2: return 0.7 * speed - 0.4
        else: return 1.0
    
    def calculate_score(self, level_data: Dict, judgements: List[int], speed: float = 1.0, is_no_hold_tap: bool = False) -> Dict:
        level = level_data.get("level", level_data)
        difficulty = level.get("difficulty", {})
        
        accuracy = self.calculate_accuracy(judgements)
        accuracy_pct = accuracy * 100
        
        base_score, base_source = self.get_base_score(level_data, accuracy)
        xacc_curve = self.get_xacc_curve(level_data)
        multiplier = self.calculate_score_multiplier(accuracy, base_score, xacc_curve)
        
        is_marathon = difficulty.get("name") == "Marathon"
        speed_mod = self.calculate_speed_modifier(speed, is_marathon)
        
        final_score = max(0, base_score * multiplier * speed_mod)
        if is_no_hold_tap: final_score *= 0.95
        
        return {
            "accuracy": accuracy,
            "accuracy_pct": round(accuracy_pct, 2),
            "score": round(final_score, 2),
            "base_score": base_score,
            "base_source": base_source,
            "multiplier": round(multiplier, 4),
            "speed_mod": round(speed_mod, 4),
            "xacc_curve": xacc_curve,
            "judgements": {
                "miss": judgements[0], "early": judgements[1], "ePerfect": judgements[2],
                "perfect": judgements[3], "lPerfect": judgements[4], "late": judgements[5]
            }
        }