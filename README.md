# TUF Tools

一个查询 TUF 信息的小工具

支持：

- 查询玩家信息，排名，国家排名
- PP分计算并给出排名变化
- 计算XACC
- 根据XACC推算最优判定
- 非交互 JSON 模式：JSON 进、JSON 出，便于其他脚本/工具调用
- 响应缓存：按 URL 保存响应体并用 ETag / Last-Modified 做条件请求，数据变了自动更新，没变就复用（304）

## Docs

- [项目完整文档](docs/doc.md)
- [XACC 反推算法详解](docs/xacc_reverse.md)

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
tuftools [--proxy VAR] [-v|--verbose] [--dump-json] [--no-cache]
tuftools --json [--json-indent N] [--request JSON | --request-file PATH]
```

`--proxy` 指定 HTTP(S) 代理，`--verbose` / `-v` 打印每个请求的详情，`--dump-json` 配合 `-v` 额外打印响应体，`--no-cache` 本次运行不读写缓存（也不发条件请求头）。缓存只用来做校验：每次查询仍会发一次请求，`304 Not Modified` 时复用本地 body。

`--json` / `-j` 进入非交互 JSON 模式（不打印横幅、不进入菜单）；`--request` 直接在参数里给出请求（`-` 表示读标准输入，隐含 `--json`）；`--request-file` 从文件读请求；`--json-indent N` 用 N 个空格缩进响应（默认 0，即一整行紧凑 JSON）。

JSON 模式（非交互，供脚本/其他工具调用）：

```powershell
# 从标准输入读一个 JSON 请求，向标准输出写一个 JSON 响应（推荐，最不容易被 shell 转义坑到）
'{"op":"xacc.calc","counts":{"perfect":1000,"failMiss":5}}' | .\build\tuftools.exe -j

# 从文件读请求，并缩进输出
.\build\tuftools.exe --request-file req.json --json-indent 2
```

> **PowerShell 5.1 注意**：`--request '{...}'` 里的一对单引号是 PowerShell 自己的，里面的双引号会在传给进程前被吃掉（`{"op":"x"}` 变成 `{op:x}`），JSON 就解析失败了。要么改用管道 / `--request-file`，要么把每个双引号写成 `\"`：`--request '{\"op\":\"player.get\",\"id\":\"9988\"}'`。PowerShell 7.3+ 调整了原生参数传递规则，不再有这个问题。

请求是 `{"op": "...", ...}` 对象（或对象数组，表示批量）；响应是 `{"ok": true, "op": "...", "result": {...}}`，失败时是 `{"ok": false, "error": {"code": "...", "message": "..."}}`。退出码 `0` 表示全部成功，`1` 表示至少一个操作失败，`2` 表示请求文档本身无法处理。stdout 上只有响应 JSON，进度/日志都走 stderr。完整协议见 [docs/doc.md](docs/doc.md) 第 2.4 节。



## 目录结构

| 文件 | 原 Python 文件 | 作用 |
| --- | --- | --- |
| [src/main.cpp](src/main.cpp) | `main.py` | 命令行参数（argparse）与主菜单 |
| [src/jsoncli.cpp](src/jsoncli.cpp) / [src/jsoncli.hpp](src/jsoncli.hpp) | — | 非交互 JSON 模式：请求/响应协议、操作分发、stdout 隔离 |
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
