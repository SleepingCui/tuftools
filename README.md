# TUF Tools

一个查询 TUF 信息的小工具

支持：

- 查询玩家信息，排名，国家排名
- PP分计算并给出排名变化
- 计算XACC
- 根据XACC推算最优判定

## 构建

### Windows（PowerShell）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1               # CMake，Release（默认）
powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1 mingw         # 直接调 g++，不经过 CMake
powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1 cmake -Debug  # Debug 构建
powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1 -Clean        # 先清空 build/
```


### macOS / Linux（GCC / Clang）

```bash

#   Debian/Ubuntu：sudo apt install cmake g++ libcurl4-openssl-dev
#   Fedora：       sudo dnf install cmake gcc-c++ libcurl-devel

./build.sh                 # CMake，Release（默认）
./build.sh cmake --debug   # CMake，Debug
./build.sh direct          # 直接调 g++/clang++，不经过 CMake
./build.sh --clean         # 先清空 build/
```



## 使用

```text
tuftools [--proxy VAR] [--verbose]
```

`--proxy` 指定 HTTP(S) 代理，`--verbose` / `-v` 打印每个请求的详情。



## 目录结构

| 文件 | 原 Python 文件 | 作用 |
| --- | --- | --- |
| [src/main.cpp](src/main.cpp) | `main.py` | 命令行参数（argparse）与主菜单 |
| [src/menus.cpp](src/menus.cpp) / [src/menus.hpp](src/menus.hpp) | `ppcalc.py`, `acccalc.py` | PP 计算器与 XACC 计算器菜单 |
| [src/info.cpp](src/info.cpp) / [src/info.hpp](src/info.hpp) | `info.py` | 玩家搜索、详情、排名查询、通关谱面列表 |
| [src/tools.cpp](src/tools.cpp) / [src/tools.hpp](src/tools.hpp) | `tools/*.py` | 分数计算器、难度库管理、XACC 反解 |
| [src/api.cpp](src/api.cpp) / [src/api.hpp](src/api.hpp) | `api.py` | 请求计数、verbose 日志、`stats()` |
| [src/http.cpp](src/http.cpp) / [src/http.hpp](src/http.hpp) | `requests` | HTTP GET 客户端（Windows 用 WinHTTP，macOS/Linux 用 libcurl） |
| [src/apppaths.cpp](src/apppaths.cpp) / [src/apppaths.hpp](src/apppaths.hpp) | — | 定位数据目录（exe 同目录 / 用户数据目录 / `TUFTOOLS_DATA_DIR`） |
| [src/pyjson.hpp](src/pyjson.hpp) | `json`, `urllib.parse` | 基于 `nlohmann::ordered_json` 的 Python 语义薄适配层 |
| [src/numfmt.hpp](src/numfmt.hpp) | f-string 格式化 | `format_fixed`、`trim_fixed`、`format_g` |
| [src/console.hpp](src/console.hpp) | `input()` | UTF-8 控制台设置与输入封装 |

## 来源

[TUF](https://tuforums.com/)

[TUF API](https://api.tuforums.com/docs/)

第三方库：[nlohmann/json](third_party/nlohmann/json.hpp) v3.11.3（MIT）、[p-ranav/argparse](third_party/argparse/argparse.hpp) v3.1（MIT）。
