# 8-25 项目材料

本目录整理了本项目的两个阶段、代表性实验结果和直接参考的论文。

* `stage1`：前端局部 e-graph 候选探索，对完整逻辑网表做安全的局部替换。
* `stage2`：将 e-graph 候选嵌入 IMCCompiler 的子网优化环节，按完整网表的 `(Size, MF)` 评价。
* `papers`：PPT 中直接引用的六篇论文：baseline、eLogic、E-morphic、SkyEgg，以及两篇 XMG/MIG 重写规则来源论文。

目录只保留源码、最小测试输入和代表性结果；不包含 `build`、Rust `target`、许可证、第三方依赖、调试缓存和批量中间文件。

运行顺序建议：先阅读 `stage1/README.md`，再阅读 `stage2/README.md`。各阶段的 `results` 目录保存了用于汇报的原始日志和 CSV。

> 说明：目录名 `8-25` 对应 8/25 版本。Gurobi 许可证和第三方库均未复制，请在本机按 README 配置。
