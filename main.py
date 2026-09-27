import argparse
from api import set_proxies, set_verbose
from info import handle_player_lookup
from ppcalc import handle_pp_calc
from acccalc import handle_acc_calc

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--proxy", type=str, help="http proxy URL")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    args = parser.parse_args()
    if args.proxy:
        set_proxies(args.proxy)
    set_verbose(args.verbose)

    print("=== TUF Tools ===")
    print("Github: github.com/sleepingcui/tuftools TUF: tuforums.com")
    
    while True:
        print("\n选择功能系统")
        print("1. 玩家数据查询")
        print("2. PP计算器")
        print("3. XACC计算器")
        print("q. 退出")

        choice = input("\n> ").strip()

        if choice == "1":
            handle_player_lookup()
        elif choice == "2":
            handle_pp_calc()
        elif choice == "3":
            handle_acc_calc()
        elif choice == "q":
            print("exit")
            break
        else:
            print("无效选择")

if __name__ == "__main__":
    main()