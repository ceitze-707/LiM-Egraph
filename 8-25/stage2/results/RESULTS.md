# 阶段二代表性结果

所有 `.out.log` 是标准输出，`.err.log` 是标准错误，`*_result.csv` 保存对应运行的 Pareto 点。

| 运行材料 | 原 IMC | e-graph | 关系 |
|---|---|---|---|
| `int2float_k1_license_fixed_*` | `(237,18)` | `(236,18)` | e-graph 支配 |
| `int2float_k1_seed2_license_fixed_*` | `(242,17)` | `(240,18)` | Pareto 权衡 |
| `ctrl_k1_seed1_metrics_*` | `(137,25)` | `(135,25)` | e-graph 支配 |
| `router_k1_license_fixed_*` | `(237,22)` | 无验证候选 | 不替换 |

`ctrl.v_csv_schema_v2_candidate_metrics.csv` 给出了 ctrl 正例的候选结构指标，用于追溯 MFFC、边界及局部收益。
