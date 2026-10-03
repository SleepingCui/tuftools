# XACCReverse 逆推求解

正向：`物量 total` 个音符各带一个判定，判定 \(i\) 的权重为 \(w_i\)（perfect \(=1.0\)，failMiss \(=0.0\)，tooEarly/late \(=0.4\) …）。

$$
\mathrm{XACC}
=
\frac{\displaystyle\sum_i \mathrm{count}_i\cdot w_i}{\mathrm{total}}
\times 100\%
$$

逆推：给定 `total`、目标 XACC（用户输入的十进制文本）、若干**固定**判定的数量，求每个判定的数量。

## 1. 最优判据

四个键，依次比较，全部整数：

1. `distance` = \(\lvert\text{分数} - \text{目标分数}\rvert\)，最小
2. `cost_units` 难度系数合计，最小
3. `non_base` 非基准判定上的音符数，最小
4. 判定表顺序靠前的判定数量更多（确定性 tie-break，见 `counts_prefer_earlier`）

排序实现集中在 `candidate_better`（src/xacc_solver.cpp:101）。旧的 `lex_less`（字典序升序）会把数量推到表尾，导致权重/系数相同的判定在不同求解器下命名不一致，已废弃。

## 2. 整数模型（src/xacc_model.hpp）

旧实现把权重当 double，只在逆推里 `round(w*20)`（1/20 格），对非 0.05 倍数的权重会与正向计算器给出不一致的答案。现在两端共用一套整数单位：

| 量                | 定义                                                                         |
| ---------------- | -------------------------------------------------------------------------- |
| `score_units`    | `score_scale = 1000`，即 \(w=\frac{\mathrm{weight\_units}}{1000}\)，精度 1/1000 |
| `cost_units`     | `cost_scale = 1000`，只做比较，不做除法                                              |
| \(u_i\)          | \(u_i=\mathrm{weight}_i-\mathrm{min\_weight}\)，最小权重判定的 \(u=0\)             |
| `gcd`            | 所有 \(u_i\) 的最大公约数                                                          |
| `ur_i`           | \(ur_i=\frac{u_i}{\mathrm{gcd}}\)，归一化损失步长                                  |
| `base`           | \(u=0\) 中难度系数最小者（默认 perfect 或 xperfect）                                    |
| `base_score_all` | 全部自由音符放在最小权重判定时的分数 = 最低可达分                                                 |
| `target_loss`    | \(target\_units-base\_score\_all\)，自由音符必须补上的分数                             |

分数：

$$
\mathrm{score}
=
\mathrm{fixed\_score}
+
\mathrm{base\_score\_all}'
+
\sum_i \mathrm{take}_i\cdot u_i
$$

等价地：

$$
\mathrm{score}
=
\mathrm{base\_score\_all}
+
\mathrm{gcd}\cdot
\sum_i \mathrm{take}_i\cdot ur_i
$$

所有乘法走 `mul_checked` / `add_checked`（int64 溢出检查，不用 `__int128`，兼容 MSVC）。

## 3. 目标与接受窗口

* 目标从**原始文本**解析成有理数

$$
\frac{\mathrm{numerator}}{10^{\mathrm{decimals}}}
$$

（`"99.92"` → \(9928/10^2\)），全程不经过 double，位数由用户输入决定（`xacc_parse_target`）。

* `xacc_target_window` 给出接受窗口 \([s_{\mathrm{lo}},s_{\mathrm{hi}})\)：所有在用户位数下**打印结果就等于其输入**的分数单位，上界半开。命中 → `exact = true`；未命中 → 返回最接近的组合并提示「无法精确到 N 位小数」。
* `xacc_target_units` 给出距目标最近的分数单位，`distance` 以它为基准。
* 显示统一走 `xacc_format_acc`，菜单不再自己重算。

## 4. 配置 xacc.json

```json
{ "schemaVersion": 2, "scoreScale": 1000, "costScale": 1000, "weights": {…}, "costs": {…} }
```

* **判定权重是游戏规则，不是配置**：固定在编译期的 `fixed_jd_weights()`（src/weights.cpp）。文件里被改过的 `weights` 一律忽略、在 note 里说明并**自动改回**；`weights.json` 已停用（存在则备份为 `weights.json.v1.bak`）。
* 难度系数可用（菜单 3）修改，写入 xacc.json。
* 首次运行若无 xacc.json，则从旧的 `costs.json` 迁移：**值必须精确落在 1/1000 网格上**，否则按 key 报错而不是静默取整。
* 缺键用内建默认补齐；坏 JSON 报错并带出文件内容上下文。

## 5. 三种求解器

`XaccSolver`（src/xacc_model.hpp:41），可用菜单 4 切换（仅当前进程有效）：

| 求解器                | 适用                            | 最优性                  |
| ------------------ | ----------------------------- | -------------------- |
| `exact_dp`         | 小规模，格数 \(\le\) `max_dp_cells` | 可证全局最优               |
| `branch_and_bound` | 大规模                           | 预算内最优；预算耗尽时标注「未证明最优」 |
| `legacy_map_dp`    | 原 `std::map` DP，保留供对比/兜底      | 是（但状态数随总损失增长）        |
| `automatic`        | 默认                            | 见 5.4                |

### 5.1 精确 DP（`run_dense_dp`）

层 \(t\) = 已分配 \(t\) 个自由音符，状态 = 已补的归一化损失

$$
k\in[0,t\cdot\mathrm{max\_ur}]
$$

即稠密数组 `offsets[t] + k`。

每格保留 `(cost, non_base)` 字典序最小者，`choice[]` 存前驱判定下标用于回溯。

最后一层按统一的 `distance → cost → non_base` 判据扫描，并用 `gcd` 把 \(k\) 还原成真实分数；同分的多个 \(k\) 按第 4 键在候选里挑（`kMaxLexTies` 个上限）。

格数：

$$
\mathrm{cells}
=
\sum_{t=1}^{\mathrm{free\_notes}}
\left(t\cdot\mathrm{max\_ur}+1\right)
$$

超过 `max_dp_cells` 直接判定为 `budget_exhausted`（用户指定 DP 时给出所需格数与建议）。

### 5.2 分支定界（`run_branch_and_bound`）

* **分支顺序**：`ur` 降序 → 难度系数升序 → 判定表顺序（`branch_`）。先放步长大的判定，上界收得快。
* **take 顺序**：闭式估计

$$
\mathrm{center}
=
\mathrm{clamp}
\left(
\mathrm{round}\left(\frac{\mathrm{needed\_ur}}{ur_i}\right),
\mathrm{low},
\mathrm{high}
\right)
$$

其中 \(ur_i=0\) 时取 `high`。

再从 `center` 向两侧展开。仍完整覆盖 \([low,high]\)，只改顺序不改完备性——这一条把 XPerfect 3940 物量/99.92 从「2000 万节点耗尽预算」降到 780 万节点证明最优。

* **剪枝**（四个）：

  1. **区间可达**：后缀 `suffix_min` / `suffix_max` 算出剩余能到达的 \([first,last]\)，最近可达距离 `best_guess > incumbent.distance` 即剪；
  2. **同余**：后缀 `suffix_gcd` 给出剩余步长可达的余数类，落不到窗口的整枝剪掉；
  3. **区间收紧**：\(lo(n)\) / \(hi(n)\) 对 \(n\) 都是线性的，由「加上剩余最大/最小步长后仍要够到窗口 \(\pm\) incumbent 距离」反解出 \(n\) 的上下界，直接缩窄 take 范围（溢出则跳过该界，只减弱剪枝不影响正确性）；
  4. **二维 LP 成本下界**：仅在已能命中窗口（`best_guess == 0` 且 incumbent 距离为 0）时才用来比成本，超过 incumbent 成本即剪。

* **支配缓存** `seen_[(position, remaining, accumulated_ur)]`：同一键出现**严格更低**的已成本时更新缓存，使后续更贵的同键路径被剪。

* **初始上界**由 `seed_incumbent` 的「两类型闭式解」给出。

* **预算**：`max_nodes = 20000000`、`max_seconds = 5.0`。命中任一 → `optimal = false`，结果里明确写「预算耗尽：这是预算内找到的最佳组合，未证明最优」；否则写「搜索空间已穷尽（结果已证明最优）」。

### 5.3 旧版 map DP（`run_legacy`）

原实现的 DP 原样保留（base 判定 + 逐种替换的损失/成本差 + 损失窗口），可在菜单里切换对比，也可作为兜底。

### 5.4 自动选择

$$
\mathrm{estimate\_cells}
\le
\mathrm{max\_dp\_cells}(8000000)
$$

→ 精确 DP，否则 → 分支定界。

理由在 `-v` 日志里给出。

## 6. 结果与状态

`XaccReverseResult`（src/xacc_solver.hpp:37）：

`status / message / counts / score_units / actual_acc / exact / optimal / distance_units / cost_units / solver / nodes / pruned / incumbent_updates / states / cells / elapsed_ms / note`

`XaccStatus`（src/xacc_model.hpp:28）区分「输入非法」「配置非法」「超出可达范围」「数学上不可达」「预算耗尽」，不再是一个笼统的「算不出来」。

顶点调用：

`xacc_reverse_search(model, target, total, fixed_counts, xperfect, options)`（src/xacc_solver.hpp:63），由 `tuf::xacc_reverse(...)`（src/tools.cpp）包装给菜单。

## 8. 文件

| 文件                       | 内容                                        |
| ------------------------ | ----------------------------------------- |
| src/xacc_model.hpp/.cpp  | 整数模型、xacc.json 读写与迁移、目标解析与窗口、格式化          |
| src/xacc_solver.hpp/.cpp | 三种求解器、判据、剪枝、预算、trace                      |
| src/weights.cpp          | `jd_keys()`、`fixed_jd_weights()`（编译期判定权重） |
| src/tools.cpp            | `xacc_calc` / `xacc_reverse` 薄包装          |
| src/menus.cpp            | 菜单 1/2/3/4（正向、逆推、难度系数、求解器）                |

## 9. 已知限制

* 权重与难度系数**都相同**的判定在数学上可互换，第 4 键只保证「结果确定且各求解器一致」，不保证符合直觉。默认配置下 XPerfect 的 `+perfect` / `xperfect` / `-perfect` 权重都是 \(1.0\)，而 `xperfect` 系数为 0，因此逆推默认全部落在 `xperfect`；要让另外两个出现，需给它们不同的**难度系数**（菜单 3）。
* 预算耗尽时不保证最优（会明确标注）。
* 精确 DP 的格数随**自由音符数 × `max_ur`**增长，大物量必须走分支定界。
* 求解器选择不持久化，只在当前进程内有效。
