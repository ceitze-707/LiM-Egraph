# Stage 2：e-graph 嵌入 IMCCompiler 子网优化

## 目的

baseline 先提取待优化子网。Stage 2 对同一子网同时生成原 IMC 候选和 e-graph 候选，分别调度、回填完整网表，再以全局 `(Size, MF)` 的 Pareto 关系决定是否保留候选。

与 Stage 1 的关键差异是：Stage 2 不把“少门”直接当作成功，而是要求经过完整 IMC 调度后的全局结果有意义。

## 目录

* `source`：IMCCompiler 源码、`EgraphSubnetOptimizer.h` 和 Rust e-graph 引擎源码。
* `testdata`：`int2float`、`ctrl`、`router` 三个最小实验输入。
* `results`：代表性 stdout/stderr、最终结果 CSV 和候选指标 CSV。

## 依赖

Windows + Visual Studio 2022、CMake、Rust/Cargo、Mockturtle/Lorina、Z3 5.0.0、Gurobi 10.0.1。

`source/CMakeLists.txt` 中保存的是原实验机路径；换机器时需修改 include、library 路径。运行前设置 Gurobi 许可证，例如：

```powershell
$env:GRB_LICENSE_FILE = 'D:\gurobi1001\win64\bin\gurobi.lic'
```

## 构建与运行

先构建 Rust 引擎：

```powershell
cd source\cut_egraph\rust
cargo build --release
cd ..\..\..
```

再在 `stage2/source` 下构建并运行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
$env:IMC_SEED = '1'
$env:IMC_THREADS = '48'
$env:IMC_RUNS = '2'
$env:IMC_EGRAPH_REPLACEMENTS = '1'
$env:IMC_BENCHMARKS_FILE = 'benchmarks_stage2.txt'
.\build\Release\IMCCompiler.exe
```

`benchmarks_stage2.txt` 默认运行 `int2float`。程序必须从 `source` 目录启动，以便定位 `cut_egraph/rust/target/release`。

## 正确性与结果解释

每个实际写回的 Stage 2 候选都会在局部边界输入上进行穷尽功能等价检查；默认边界最多 12 个输入，因此最多检查 `2^12=4096` 组。MFFC 门数默认不超过 32，过大的候选会跳过而不是降级为未验证替换。

代表性结论：

* `int2float, seed=1`：`(237,18) -> (236,18)`，e-graph 支配原 IMC。
* `int2float, seed=2`：`(242,17) -> (240,18)`，是 Pareto 权衡。
* `ctrl, seed=1`：`(137,25) -> (135,25)`，e-graph 支配原 IMC。
* `router, seed=1`：无验证候选，保持原 IMC 结果。

因此当前结论是 e-graph 能扩展 IMC 候选空间，但尚不能声称对所有电路均有提升。
