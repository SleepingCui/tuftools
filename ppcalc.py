
from api import stats
from tools.TUFScoreCalculator import TUFScoreCalculator
import info



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
        
    old_country_rank = info.fetch_single_rank(player, "rankedScore", "country")
    
    future_player = {
        "country": player.get("country"),
        "rankedScore": new_score
    }
    
    new_global_rank = info.fetch_single_rank(future_player, "rankedScore", "global")
    new_country_rank = info.fetch_single_rank(future_player, "rankedScore", "country")
    
    def rank_delta(old_rank, new_rank):
        try:
            return f"+{int(old_rank) - int(new_rank)}"
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
    
    level_id = input("\n关卡ID: ").strip()
    if not level_id.isdigit():
        print("错误: 请输入有效的ID")
        return
    level_id = int(level_id)
    
    print(f"\n正在获取关卡 {level_id} 的数据...")
    try:
        level_data = calculator.get_level_data(level_id)
        level = level_data.get("level", {})
        print(f"歌曲: {level.get('song', 'Unknown')}")
        print(f"艺术家: {level.get('artist', 'Unknown')}")
        print(f"难度: {level.get('difficulty', {}).get('name', 'Unknown')}")
        print(f"物量: {level.get('tilecount', 0)}")
        print(f"基础PP分: {level.get('ppBaseScore', 'N/A')}")
        print(f"基础分: {level.get('baseScore', 'N/A')}")
    except Exception as e:
        print(f"获取失败: {e}")
        return
    
    print("\n输入判定:")
    print("格式: miss early EPerfect perfect LPerfect late")
    
    judgement_input = input("\n判定数据: ").strip().split()
    if len(judgement_input) != 6:
        print("错误: 需要输入6个值")
        return
    
    try:
        judgements = [int(x) for x in judgement_input]
    except ValueError:
        print("错误: 请输入有效的数字")
        return
    
    speed_input = input("\n速度倍率 (默认1.0): ").strip()
    speed = float(speed_input) if speed_input else 1.0
    
    no_hold = input("\n是否禁用Hold+Tap? (y/n, 默认n): ").strip().lower() == 'y'
    
    print("正在计算...")
    
    result = calculator.calculate_score(level_data, judgements, speed, no_hold)
    
    print(f"XACC: {result['accuracy_pct']}%")
    print(f"基础分: {result['base_score']} ({result['base_source']})")
    print(f"分数倍率: {result['multiplier']}x")
    print(f"速度修正: {result['speed_mod']}x")
    print(f"PP分: {result['score']}")
    
    for key, value in result['judgements'].items():
        print(f"{key}: {value}")
        
    calc_rank = input("\n是否计算排名变化? (y/n, 默认n): ").strip().lower()
    if calc_rank == 'y':
        calculate_rank_changes(result['score'])
        
    print()
    stats()