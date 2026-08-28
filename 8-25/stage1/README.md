# Stage 1：前端 e-graph 局部优化

## 目的

在 XMG 网表的局部 cut 中枚举候选，计算 MFFC，在 Rust `egg` 引擎中做等价饱和，提取较小实现后回写。阶段一以局部门数为主要选择标准；最终使用全网随机仿真做检查。

## 目录

* `source`：C++ cut 枚举/替换代码与 Rust e-graph 引擎源码。
* `examples/ctrl.v`：最小演示输入。
* `results/ctrl_stage1_demo_opt.v`：一次运行得到的优化后网表。

## 构建

先在 `source/rust` 中执行：

```powershell
cargo build --release
```

再在 `source` 中执行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`CMakeLists.txt` 中的 Mockturtle/Lorina 路径是原实验机器的路径；换机器时请改为本机安装位置。

## 运行

必须在 `source` 目录运行，以便程序找到 `rust/target/release/xmg_egraph_cut.exe`：

```powershell
.\build\Release\cut_egraph.exe ..\examples\ctrl.v ..\results\ctrl_opt_run.v 25
```

## 已保存结果

历史 `ctrl` 正例：原网表 174 门，25 次替换后为 148 门，实际减少 26 门。该阶段的验证为 2000 组随机全网仿真；它不是阶段二使用的局部穷尽证明。
