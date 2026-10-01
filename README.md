# TUF Tools

一个查询 TUF 信息的小工具（C++17 实现，单文件可执行程序）。

支持：

- 查询玩家信息，排名，国家排名
- PP分计算并给出排名变化
- 计算XACC
- 根据XACC推算最优判定

## 构建

需要 C++17 编译器（开发环境为 MinGW-w64 g++ / GCC 15.2.0），Windows 上链接系统自带的 `winhttp`。无需包管理器，也无需联网：两个第三方头文件库已经 vendored 在 [third_party/](third_party/README.md)。

### 推荐：`build.ps1`（默认走 CMake）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1               # CMake，Release（默认）
powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1 mingw         # 直接调 g++，不经过 CMake
powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1 cmake -Debug  # Debug 构建
powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1 -Clean        # 先清空 build/
```

第一个位置参数选后端（`cmake` 或 `mingw`，省略即 `cmake`）；`-ExecutionPolicy Bypass` 是因为脚本未签名。脚本会自动寻找 `cmake` 与 `g++`：CMake 优先使用 `Ninja`，没有 Ninja 时用 `MinGW Makefiles`（配合 `g++` 同目录的 `mingw32-make.exe`）。

### 直接调用 CMake

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### 直接一行 g++

```powershell
g++ -std=c++17 -O2 -Wall -static -Ithird_party -o build/tuftools.exe src/*.cpp -lwinhttp
```

三种方式产物统一为 `build/tuftools.exe`（CMake 用 `RUNTIME_OUTPUT_DIRECTORY` 保证了这一点，多配置生成器也一样）。`-static` 让产物不依赖 `libwinpthread-1.dll` / `libstdc++-6.dll`，方便直接分发单文件 exe，CMake 在 MinGW 下会自动加上。CI（[.github/workflows/build.yml](.github/workflows/build.yml)）用的是 MSYS2 里的一行 g++。

## 使用

```text
tuftools [--proxy VAR] [--verbose]
```

`--proxy` 指定 HTTP(S) 代理，`--verbose` / `-v` 打印每个请求的详情。

> **请在仓库根目录运行。** `difficulties.json` 与 `costs.json` 按当前工作目录读写（与 Python 版本行为一致）；缺少 `difficulties.json` 时会尝试从 `api.tuforums.com` 下载。

## 目录结构

| 文件 | 原 Python 文件 | 作用 |
| --- | --- | --- |
| [src/main.cpp](src/main.cpp) | `main.py` | 命令行参数（argparse）与主菜单 |
| [src/menus.cpp](src/menus.cpp) / [src/menus.hpp](src/menus.hpp) | `ppcalc.py`, `acccalc.py` | PP 计算器与 XACC 计算器菜单 |
| [src/info.cpp](src/info.cpp) / [src/info.hpp](src/info.hpp) | `info.py` | 玩家搜索、详情、排名查询、通关谱面列表 |
| [src/tools.cpp](src/tools.cpp) / [src/tools.hpp](src/tools.hpp) | `tools/*.py` | 分数计算器、难度库管理、XACC 反解 |
| [src/api.cpp](src/api.cpp) / [src/api.hpp](src/api.hpp) | `api.py` | 请求计数、verbose 日志、`stats()` |
| [src/http.cpp](src/http.cpp) / [src/http.hpp](src/http.hpp) | `requests` | WinHTTP GET 客户端 |
| [src/pyjson.hpp](src/pyjson.hpp) | `json`, `urllib.parse` | 基于 `nlohmann::ordered_json` 的 Python 语义薄适配层 |
| [src/numfmt.hpp](src/numfmt.hpp) | f-string 格式化 | `format_fixed`、`trim_fixed`、`format_g` |
| [src/console.hpp](src/console.hpp) | `input()` | UTF-8 控制台设置与输入封装 |

## 与 Python 版本的差异说明

* 数值与字符串按 Python 的 `repr` / `str` 规则格式化（`py_float_str`、`py_round`、`py_repr`），输出尽量与 Python 版一致。
* JSON 文件按 4 空格（`costs.json`）与 2 空格（`difficulties.json`）缩进写出，并保留键顺序。
* XACC 反解移植了 Python 的动态规划实现（含可选的「固定判定数量」）。
* stdin 读到 EOF 时干净退出而不是死循环，便于管道喂输入（`"..." | tuftools.exe`）。

## 来源

[TUF](https://tuforums.com/)

[TUF API](https://api.tuforums.com/docs/)

第三方库：[nlohmann/json](third_party/nlohmann/json.hpp) v3.11.3（MIT）、[p-ranav/argparse](third_party/argparse/argparse.hpp) v3.1（MIT）。
