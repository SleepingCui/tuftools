import json
import os

from api import fetchapi, BASE_URL


class DifficultyManager:
    file = "difficulties.json"

    def __init__(self):
        self.difficulties = self.load()

    def update(self):
        raw = fetchapi(f"{BASE_URL}/v2/database/difficulties")

        result = [
            {
                "name": item["name"],
                "baseScore": item["baseScore"]
            }
            for item in raw
        ]

        with open(self.file, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)

        self.difficulties = result
        return result

    def load(self):
        if not os.path.exists(self.file):
            print("下载难度数据...")
            return self.update()

        with open(self.file, "r", encoding="utf-8") as f:
            return json.load(f)

    def get_base_score(self, difficulty_name):
        difficulty_name = difficulty_name.upper()

        for item in self.difficulties:
            if item["name"].upper() == difficulty_name:
                return item["baseScore"]

        raise ValueError(f"未知难度: {difficulty_name}")