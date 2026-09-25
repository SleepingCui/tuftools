import json
import os

from api import fetchapi, BASE_URL


class DifficultyManager:
    file = "difficulties.json"

    def __init__(self):
        self.difficulties = self.load()

    @classmethod
    def has_cache(cls):
        """本地是否已有难度数据。调用方据此决定要不要显示下载提示。"""
        return os.path.exists(cls.file)

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
            return self.update()

        with open(self.file, "r", encoding="utf-8") as f:
            return json.load(f)

    def difficulty_names(self):
        return [item["name"] for item in self.difficulties]

    def get_base_score(self, difficulty_name):
        difficulty_name = difficulty_name.upper()

        for item in self.difficulties:
            if item["name"].upper() == difficulty_name:
                return item["baseScore"]

        raise ValueError(f"未知难度: {difficulty_name}")
