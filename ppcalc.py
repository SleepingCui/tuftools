from tools.TUFScoreCalculator import TUFScoreCalculator
from tools.DiffMan import DifficultyManager
from api import stats
import info
import os

def calculate_rank_changes(calculated_score: float):
    print("选择查找玩家的方式:")
    print("1. 按玩家名称")
    print("2. 按玩家唯一 ID")
    p_mode = input("> ").strip()
    
    player = None
    if p_mode == "1":
        name = input("玩家名:").strip()
        results = info.search(name)
        if not results:
            print("未找到玩家")
            return
        player = info.choose_player(results)
    elif p_mode == "2":
        pid = input("ID:").strip()
        try:
            player = info.get_player(pid)
        except:
            print("未找到玩家")
            return
    else:
        print("无效选择")
        return

    if player and ("rankedScoreRank" not in player or "rankedScore" not in player):
        try:
            player = info.get_player(player["id"])
        except:
            print("无法获取玩家数据")
            return
            
    old_score = player.get("rankedScore", 0)
    new_score = old_score + calculated_score
    
    if player.get("rankedScoreRank") is not None:
        old_global_rank = player["rankedScoreRank"]
    else:
        old_global_rank = info.fetch_single_rank(player, "rankedScore", "global")

    if old_global_rank == 0: old_global_rank = 1
        
    old_country_rank = info.fetch_single_rank(player, "rankedScore", "country")
    if old_country_rank == 0: old_country_rank = 1
    
    future_player = {
        "country": player.get("country"),
        "rankedScore": new_score
    }
    
    new_global_rank = info.fetch_single_rank(future_player, "rankedScore", "global")
    if new_global_rank == 0: new_global_rank = 1
    
    new_country_rank = info.fetch_single_rank(future_player, "rankedScore", "country")
    if new_country_rank == 0: new_country_rank = 1
    
    def rank_delta(old_rank, new_rank):
        try:
            delta = int(old_rank) - int(new_rank)
            return f"+{delta}" if delta >= 0 else f"{delta}"
        except (TypeError, ValueError):
            return "N/A"

    global_delta = rank_delta(old_global_rank, new_global_rank)
    country_delta = rank_delta(old_country_rank, new_country_rank)

    print(f"玩家: {player.get('name')} (ID: {player.get('id')})")
    print(f"排位分数: {old_score:.2f} -> {new_score:.2f} (+{calculated_score:.2f})")
    print(f"全球排名: {old_global_rank} -> {new_global_rank} ({global_delta})")
    print(f"国家排名: {old_country_rank} -> {new_country_rank} ({country_delta})")

def handle_pp_calc():
    calculator = TUFScoreCalculator()
    diffman = DifficultyManager()

    if os.path.exists("difficulties.json"):
        update = input("是否更新难度数据库? (y/N): ").strip().lower()
        if update == "y":
            try:
                diffman.update()
                print("难度数据库更新完成。")
            except Exception as e:
                print(f"更新失败: {e}")
    else:
        print("下载难度数据库...")
        try:
            diffman.update()
            print("下载完成")
        except Exception as e:
            print(f"下载失败: {e}")
            return
            
    difficulty = input("难度: ").strip().upper()
    marathon = input("是否Marathon(y/N): ").strip().lower() == "y"

    level_data = calculator.build_level_data(difficulty_name=difficulty,marathon=marathon)
        
    accuracy = float(input("XACC: ").strip())

    misses_input = input("Miss 数(默认0): ").strip()
    misses = int(misses_input) if misses_input else 0
        
    speed_input = input("速度倍率 (默认1.0): ").strip()
    speed = float(speed_input) if speed_input else 1.0
    
    no_hold = input("是否禁用Hold+Tap? (y/N): ").strip().lower() == 'y'
    
    result = calculator.calculate_score(level_data, accuracy, misses, speed, no_hold)
    
    print(f"XACC: {result['accuracy_pct']}%")
    print(f"基础分: {result['base_score']} ({result['base_source']})")
    print(f"分数倍率: {result['multiplier']}x")
    print(f"速度修正: {result['speed_mod']}x")
    print(f"PP分: {result['score']}")

        
    calc_rank = input("\n是否计算排名变化? (y/N): ").strip().lower()
    if calc_rank == 'y':
        calculate_rank_changes(result['score'])
        
    print()
    stats()