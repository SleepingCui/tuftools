from typing import Dict, Tuple
from api import fetchapi, BASE_URL
from tools.DiffMan import DifficultyManager

class TUFScoreCalculator:  
    JUDGEMENT_WEIGHTS = {
        "miss": 0.2, "early": 0.4, "ePerfect": 0.75, "perfect": 1.0, "lPerfect": 0.75, "late": 0.4
    }
    
    
    def __init__(self):
        self.diffman = DifficultyManager()
        self.difficulties = self.diffman.load()

    def build_level_data(self, difficulty_name: str, marathon: bool = False, tilecount: int = 0):
        difficulty_name = difficulty_name.upper()

        return {
            "level": {
                "song": "",
                "artist": "",
                "tilecount": tilecount,
                "ppBaseScore": 0,
                "baseScore": 0,
                "difficulty": {
                    "name": "Marathon" if marathon else difficulty_name,
                    "baseScore": self.diffman.get_base_score(difficulty_name)
                },
                "xaccCurve": None,
                "xaccCurveMeta": None
            }
        }

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
    
    def calculate_score_multiplier(self, accuracy: float, base_score: float, xacc_curve: Dict = None) -> float:
        """XaccMtp(x) from the PP formula; accuracy is a fraction from 0 to 1."""
        if accuracy < 0.95 or accuracy > 1:
            return 1.0
        if accuracy == 1:
            return -2100 / (base_score + 262.5) + 14 if base_score else 1.0
        return -0.027 / (accuracy - 1.0054) + 0.513
    
    def calculate_speed_modifier(self, speed: float, is_marathon: bool = False) -> float:
        if is_marathon:
            if speed == 0 or speed == 1: return 1.0
            if speed < 1: return 0.0
            return max(0, 2 - speed)
        
        if speed == 0 or speed == 1: return 1.0
        elif speed < 1: return 0.0
        elif speed < 1.1: return -3.5 * speed + 4.5
        elif speed < 1.5: return 0.65
        elif speed < 2: return 0.7 * speed - 0.4
        else: return 1.0

    @staticmethod
    def calculate_empty_tap_modifier(empty_taps: int, tilecount: int) -> float:
        """ScoreV2Mtp(am), where am=max(0, m-floor(t/315))."""
        adjusted_empty_taps = max(0, empty_taps - tilecount // 315)
        if empty_taps == 0:
            return 1.1
        if adjusted_empty_taps == 0:
            return 1.0
        if adjusted_empty_taps == 1:
            return 0.9
        if adjusted_empty_taps <= 25.5:
            return 0.9 - 0.2 * ((adjusted_empty_taps - 1) / 24.5) ** 0.7
        if adjusted_empty_taps <= 50:
            return 0.5 + 0.2 * ((50 - adjusted_empty_taps) / 24.5) ** 0.7
        return 0.5
    
    # def calculate_score(self, level_data: Dict, judgements: List[int], speed: float = 1.0, is_no_hold_tap: bool = False) -> Dict:
    #     level = level_data.get("level", level_data)
    #     difficulty = level.get("difficulty", {})
        
    #     accuracy = self.calculate_accuracy(judgements)
    #     accuracy_pct = accuracy * 100
        
    #     base_score, base_source = self.get_base_score(level_data, accuracy)
    #     xacc_curve = self.get_xacc_curve(level_data)
    #     multiplier = self.calculate_score_multiplier(accuracy, base_score, xacc_curve)
        
    #     is_marathon = difficulty.get("name") == "Marathon"
    #     speed_mod = self.calculate_speed_modifier(speed, is_marathon)
        
    #     final_score = max(0, base_score * multiplier * speed_mod)
    #     if is_no_hold_tap: final_score *= 0.95
        
    #     return {
    #         "accuracy": accuracy,
    #         "accuracy_pct": round(accuracy_pct, 2),
    #         "score": final_score,
    #         "base_score": base_score,
    #         "base_source": base_source,
    #         "multiplier": round(multiplier, 4),
    #         "speed_mod": round(speed_mod, 4),
    #         "xacc_curve": xacc_curve,
    #         "judgements": {
    #             "miss": judgements[0], "early": judgements[1], "ePerfect": judgements[2],
    #             "perfect": judgements[3], "lPerfect": judgements[4], "late": judgements[5]
    #         }
    #     }
    def calculate_score(self, level_data: Dict, accuracy_pct: float, misses: int = 1, speed: float = 1.0, is_no_hold_tap: bool = False) -> Dict:
        level = level_data.get("level", level_data)
        difficulty = level.get("difficulty", {})
        
        accuracy = accuracy_pct / 100
        
        base_score, base_source = self.get_base_score(level_data, accuracy)
        multiplier = self.calculate_score_multiplier(accuracy, base_score)
        
        is_marathon = difficulty.get("name") == "Marathon"
        speed_mod = self.calculate_speed_modifier(speed, is_marathon)
        
        tilecount = int(level.get("tilecount", 0) or 0)
        empty_tap_mod = self.calculate_empty_tap_modifier(misses, tilecount)
        default_setting_mod = 0.9 if is_no_hold_tap else 1.0
        final_score = base_score * multiplier * speed_mod * empty_tap_mod * default_setting_mod
        
        return {
            "accuracy": accuracy,
            "accuracy_pct": round(accuracy_pct, 2),
            "score": final_score,
            "base_score": base_score,
            "base_source": base_source,
            "multiplier": round(multiplier, 4),
            "speed_mod": round(speed_mod, 4),
            "empty_tap_mod": round(empty_tap_mod, 4),
            "default_setting_mod": default_setting_mod,
            "tilecount": tilecount
        }
