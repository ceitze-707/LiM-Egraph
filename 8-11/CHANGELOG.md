**IMCCompiler 项目修改日志**



**项目目标：复现IMCCompiler（迭代编译器），并在此基础上用E-graph优化。**

**论文：High-Quality Iterative Compiler for SIMD Logic-in-Memory Architecture with Tight Coupling of Synthesis and Scheduling**

原仓库：https://github.com/SJTU-ECTL/IMCCompiler





**环境配置**



修改1：utils.h — 修正Z3和Gurobi路径

&#x20; 文件：IMCCompiler/utils.h

&#x20; Z3 include：  "z3include\\\\z3++.h"  →  绝对路径指向 z3-5.0.0-x64-win\\include\\

&#x20; Z3 lib：      "C:\\\\z3\\\\libZ3.lib"   →  绝对路径指向 z3-5.0.0-x64-win\\bin\\

&#x20; Gurobi include： "C:\\\\gurobi1001\\\\..."  →  "D:\\\\gurobi1001\\\\..."

&#x20; Z3和Gurobi装在D盘，原代码指向C盘。仅改路径，不改变逻辑。



准备：

&#x20; Z3 5.0.0           z3-5.0.0-x64-win\\         已就位

&#x20; Gurobi 10.0.1      D:\\gurobi1001\\             已就位

&#x20; Mockturtle         mockturtle\\                已就位

&#x20; lorina             lorina\\                    已就位

&#x20; IMCCompiler原代码  IMCCompiler\\               已就位

&#x20; IMCScheduler原代码 IMCScheduler\\              已就位（参考用）



修改2：新建 CMakeLists.txt

&#x20; 文件：IMCCompiler/CMakeLists.txt（新建）

&#x20; 原因：原代码只有VS工程，没有CMake构建文件。用CMake统一管理include/lib路径。

&#x20; 内容：指定Z3、Gurobi、Mockturtle、lorina的include和lib路径，C++17标准，链接libz3.lib、gurobi100.lib、gurobi\_c++md2017.lib。



修改3：CMakeLists.txt — 添加Mockturtle依赖库路径

&#x20; 问题：编译报错 "无法打开包括文件: kitty/cube.hpp"

&#x20; 原因：Mockturtle虽是header-only，但内部依赖kitty/fmt/bill/percy等，在mockturtle/lib/下，需显式加入include路径。

&#x20; 解决：添加10个Mockturtle子库的include路径。



修改4：CMakeLists.txt — 修复max宏冲突 + nauty路径

&#x20; 问题1：std::max编译错误。Windows <windows.h>定义了max宏。

&#x20; 解决1：加 add\_compile\_definitions(NOMINMAX) 禁止Windows的min/max宏。

&#x20; 问题2：percy库依赖nauty但缺include路径。

&#x20; 解决2：添加mockturtle/lib/nauty到include目录。



修改5：nauty的Windows兼容 — 创建unistd.h桩文件

&#x20; 问题：unistd.h是Unix/POSIX头文件，Windows不存在。nauty被percy间接引用。

&#x20; 解决：在mockturtle/lib/nauty/下创建空的unistd.h桩文件。



修改6：去掉nauty依赖 — nauty与Windows SDK冲突

&#x20; 问题：nauty定义set类型与C++ std::set冲突，且unistd.h桩让nauty.h暴露后污染winnt.h。

&#x20; 根因：项目不使用nauty/percy（精确综合）功能，它们是Mockturtle可选依赖。

&#x20; 解决：从CMakeLists.txt去掉nauty/include路径。



修改7：utils.h — 修复exact\_library API兼容性

&#x20; 问题：新版Mockturtle的exact\_library模板参数从(Ntk, Type)改为(Ntk, NInputs=4)，旧代码exact\_library<xmg\_network, xmg\_npn\_resynthesis>编译失败。

&#x20; 解决：改为exact\_library<xmg\_network>（第二个参数默认4），共2处（Aig2Xmg和Bliff2Xmg函数内）。



修改8：Netlist.h / Scheduler.cpp / Netlist.cpp — 修复nauty::set与std::set冲突

&#x20; 问题：nauty.h定义 typedef setword set; 与 using namespace std; 引入的std::set冲突。

&#x20;       尝试过 #define set nauty\_set（误伤所有std::set）和NAUTY\_IN\_MAGMA宏（缺defs.h），均失败。

&#x20; 解决：在Netlist.h(24)、Netlist.cpp(560)、Scheduler.cpp(574)中将裸set改为std::set（3处）。

&#x20;       使用perl而非sed（Windows sed的\\t不兼容）。



修改9：CMakeLists.txt — 修复链接错误（fmt + CRT运行时）

&#x20; 问题1：fmt的format\_float/snprintf\_float等函数在header-only模式下不可用，Mockturtle的balancing函数链接时报undefined symbol。

&#x20; 解决1：在add\_executable中添加fmt源文件format.cc和os.cc。

&#x20; 问题2：Gurobi库（gurobi\_c++md2017.lib）用/MD编译，CMake设了/MT，链接时报RuntimeLibrary不匹配。

&#x20; 解决2：将MSVC\_RUNTIME\_LIBRARY从MultiThreaded改为MultiThreadedDLL。



修改10：Scheduler.cpp — 修复Gurobi 10 API兼容性

&#x20; 问题：程序在env.start()处静默崩溃。定位过程：

&#x20;   - m\_nGraphBound=300跳过划分 → 程序正常 → 问题在PartitionILP()

&#x20;   - 加debug cout → 只打印到\[1]，不打印\[2] → 崩溃在env.start()

&#x20;   - Gurobi CLI测试正常 → license没问题

&#x20; 根因：Gurobi 10中GRBEnv(true) + env.start()的行为变化。

&#x20; 解决：GRBEnv env = GRBEnv(true); env.start(); → GRBEnv env = GRBEnv();

&#x20;       运行时需设置环境变量：set GRB\_LICENSE\_FILE=D:\\gurobi1001\\win64\\bin\\gurobi.lic







**Baseline复现完成**



在router.v（EPFL benchmark，201门）上运行IMCCompiler原始代码：

&#x20; Size: 201 → 184 (-8.5%)

&#x20; MF:   18 → 10  (-44.4%)

30轮迭代，Pareto前沿正常收敛。结果文件：01-baseline/router.v\_result\_baseline.csv







**版本v3 — S-表达式路线**



策略：E-graph不替代resyn2，而是换起点。

&#x20; Sub.m\_net → E-graph改结构 → resyn2抛光 → 新局部最优。



关键代码：

&#x20; - Scheduler.cpp：v3 E-graph块（export + rebuild + verify + USING!）

&#x20; - goto skip\_resyn2标签在while(true)前面（不是跳过while）

&#x20; - E-graph设置NewNet，resyn2 while循环以E-graph结果作为起点继续优化



流程：

&#x20; 1. ExportXmgForEgraph → S-表达式文件

&#x20; 2. CallEgraphOptimizer → Rust egg饱和+提取

&#x20; 3. ApplyEgraphResult → 重建xmg\_network（PI保持）

&#x20; 4. write\_verilog + read\_verilog → 往返验证

&#x20; 5. 如果gate数 <= 原版 → goto skip\_resyn2

&#x20; 6. 调度 + SubstituteSub + ConfigMF



关键文件：

&#x20; Scheduler.cpp：v3 E-graph块 + skip\_resyn2标签

&#x20; Scheduler.h：移除了TryEgraphVariant

&#x20; egraph\_helpers.h：ExportXmgForEgraph, CallEgraphOptimizer, ApplyEgraphResult

&#x20; sexpr\_parser.h：ParseIntoXmg, NextSExprToken

&#x20; xmg\_egraph/src/main.rs：22条XMG重写规则，SymbolLang



结果（router.v）：

&#x20; 最小Size: 153 (baseline 184, -17%)

&#x20; 最小MF:    9 (baseline 10, -10%)

已知问题：部分轮次MF=-1（SubstituteSub兼容性）。



Bug修复：S-表达式缓存导致指数爆炸

&#x20; 问题：XmgNode2SExpr只缓存PI/常量，不缓存中间节点，表达式从几十KB膨胀到几MB。

&#x20; 解决：加 cache\[n] = expr; 缓存所有中间节点。



Bug修复：PO索引错误——子网表PO映射到垃圾节点

&#x20; 问题：ExportXmgForEgraph使用外部传入的vecPOOrigIndex，子网表节点编号不同，

&#x20;       导致4个PO中3个指向错误节点，输出(maj3 0 0 0)。

&#x20; 解决：改用xmg.foreach\_po()直接遍历子网表自己的PO。



Bug修复：PI映射不匹配——重建网表时PI顺序随机

&#x20; 问题：RebuildXmgFromSExprFile按首次出现顺序创建PI，与子网表原始PI顺序不一致，

&#x20;       SubstituteSub索引越界崩溃。

&#x20; 解决：ExportXmgForEgraph首行输出NUM\_PI <count>，

&#x20;       RebuildXmgFromSExprFile先读PI数量，预创建pi\_0到pi\_{N-1}，确保顺序一致。



技术决策：SymbolLang vs 自定义语言

&#x20; 问题：egg 0.9 define\_language!宏只接受\[Id;N]/Vec<Id>/Box<\[Id]>/Id作为子节点类型。

&#x20; 解决：使用egg内置的SymbolLang——叶子=字符串标识符，操作=字符串名+子节点列表。



诊断：m\_nGraphBound=300定位静默退出问题

&#x20; 问题：程序打印"Partitioning 201 nodes"后静默退出，CSV为空。

&#x20; 方法：临时将m\_nGraphBound从80改为300，让router跳过ILP划分直接走SMT最优调度。

&#x20;       若跑通则证明问题在PartitionILP的Gurobi调用。临时诊断修改，bug定位后恢复80。







**v5 — 编号DAG路线**



借鉴**E-morphic(DAC'25) DAG-to-DAG思路**。数字索引替代S-表达式名字，根除PI重排。



格式：PI\_COUNT / GATE\_COUNT / PI\_MAP / PI行 / C0/C1 / X/M/N门行 / P输出行



关键修复：PI和P行区分、拓扑序导出、边界保护、去掉verilog往返。



v3问题定性：S-表达式用名字pi\_N指代PI，重建后PI新编号与原网不一致，

&#x20; SubstituteSub偶发越界崩溃。即使不崩，索引错位导致结果不可信。



v5正确性自查：

&#x20; 1. PI顺序：出口写PI 0..N-1，解析器按序创建 → 一致

&#x20; 2. 拓扑序：foreach\_gate天然拓扑，孩子先于父母 → 无前向引用

&#x20; 3. TI检测：ConfigWithXMG用fIndex >= nOrigOffset，两边条件相同

&#x20; 4. SubstituteSub：ti = fIndex - nOrigOffset，公式一致 → vecTIOrigIndex映射正确

&#x20; 5. 运行验证：30轮0次崩溃，0个MF=-1，所有SubstituteSub成功



**结果：Best Size=155, MF=9（baseline 184/10，改善16%）**。



**v5.1 — 变体探索版**



改动：Rust侧用id2flat做Id→flat index反向映射，每个门取egg e-class中第一个e-node，

&#x20;     子节点换序则输出变体行。

效果：110/137门产出变体，管道通，无崩溃。但结果与baseline一致—12条规则只有交换律，resyn2将变体压回同一局部最优。





**v5.2 — Per-PO提取版**



架构：Per-PO独立提取。每个PO从e-graph提取最优表达式树 → PO间不共享中间门 → 全局去重。

链路：Per-PO提取 → egg饱和(18规则) → 各PO独立最优 → tree\_to\_lines(去重) → C++重建 → resyn2 → SubstituteSub

结果：Size=5, MF=5（baseline 184/10）。不可信——DAG共享丢失，过度简化。

18条规则：xor/maj交换律 + 幂等律 + 常量折叠 + 吸收律 + 非门分配律。

稳定性：30轮完成，1次MF=-1。





**v6 — 去掉Verilog往返**



改动：Scheduler.cpp:1127-1131，egNet.clone()直接使用，不再write\_verilog→read\_verilog。

原因：verilog往返每轮丢10-26个门的优化信息，Mockturtle的verilog读写无法完整保留XMG内部表示。



代码变更：

&#x20; 旧：egNet → write\_verilog → read\_verilog → egNet2.clone() → 使用（verilog损失）

&#x20; 新：egNet → egNet.clone() → 直接使用（零损失）



结果（router.v）：

&#x20; Size=35, MF=3（0规则透传）。对比baseline Size=184, MF=10。

&#x20; NO-EGRAPH测试（Sub.m\_net直接进resyn2）恢复Size=184。

&#x20; Verilog往返加回后仍异常。排除node索引和verilog损失的问题。



定位结论：重建XMG的内部节点索引与Sub.m\_net不同，ConfigWithXMG(nOrigOffset)的TI识别失效。

子网表嵌入方案的根本架构缺陷——TI/PI边界与Mockturtle内部索引耦合。

&#x20;→ 转向egraph\_v2全电路方案。





管道Bug修复（2026-08-10）



Bug 1：PI索引偏移 — index\_to\_node(0)返回常量节点

&#x20; 文件：egraph\_helpers.h — ExportXmgForEgraphNumbered()

&#x20; 问题：XMG中node 0是常量节点，PI从node 1开始。

&#x20;       导出代码用 for(i=0;i<nPI;i++) index\_to\_node(i) 把常量当成了PI\_0，

&#x20;       PI\_{nPI-1}没有fidx，fidx\[未映射节点]返回0（常量）。

&#x20; 发现：test\_tiny.cpp两门手动电路导出后功能不等价，逐门dump fanin发现fidx映射错乱。

&#x20;       换成foreach\_pi()后两门测试8/8通过。

&#x20; 修复：两处 for(i=0;i<nPI;i++){xmg.index\_to\_node(i)} → xmg.foreach\_pi(\[\&](auto n){...})

&#x20; 影响：egraph\_helpers.h, egraph\_helpers\_v6.h, egraph\_helpers\_v5.2.h



Bug 2：C0/C1共享节点导致fidx覆盖

&#x20; 文件：egraph\_helpers.h — ExportXmgForEgraphNumbered()

&#x20; 问题：XMG中get\_constant(true)返回!get\_constant(false)，两者get\_node()返回同一节点。

&#x20;       fidx\[const0\_node]和fidx\[const1\_node]写的同一个key，后者覆盖前者。

&#x20;       所有引用C0的门变成了C1。

&#x20; 发现：修完Bug 1后test\_pipeline在router.v（201门）上仍MISMATCH，

&#x20;       orig和reb输出几乎互补（1100... vs 0011...），定位到常量索引互换。

&#x20; 修复：不用fidx\[node]存常量索引，改为独立变量c0\_fidx/c1\_fidx。

&#x20;       门fanin和PO遍历时用is\_constant()判断后直接取对应索引。





**正确性验证测试**



测试方法：ExportXmgForEgraphNumbered → RebuildXmgFromNumbered → simulate<bool>(随机输入)比较PO。不经过Rust/e-graph，C++端管道验证。



测试结果：

&#x20; test\_tiny       两门手动XMG（XOR3+互补fanin）  8/8 PASS

&#x20; test\_tiny2      单门手动XMG（XOR3(!a,b,c)）    8/8 PASS

&#x20; test\_two\_gate   两门手动XMG（复用fanin）        8/8 PASS

&#x20; test\_pipeline   router.v全电路（201门60PI30PO） 2000/2000 PASS

&#x20; test\_pipeline   子网表Verilog（4门60PI2PO）      2000/2000 PASS





**下一步方向**



子网表嵌入方案（v3/v5/v6）结论：

&#x20; v5编号DAG路线管道全通，Best Size=155/MF=9（改善16%），但受限于交换律规则，

&#x20; 更强的结构改变规则（恒等、折叠、吸收）在子网表嵌入架构下无法安全启用。

&#x20; v6定位到根因：重建XMG与ConfigWithXMG的TI/PI索引不兼容。



新方向（egraph\_v2）：

&#x20; 全电路独立工具，Verilog → e-graph → 优化Verilog → IMCCompiler。

&#x20; 避开子网表提取、TI/PI边界等所有集成问题。透传已验证2000/2000 PASS。

&#x20; 待完成：修复Per-PO提取的PO索引越界bug，逐条验证18条规则。



下一步：

&#x20; 1. 修复egraph\_v2 Rust端Per-PO提取bug

&#x20; 2. 逐条加规则验证：0→4(换序)→+恒等律→+常量折叠→...，每条跑2000 patterns

&#x20; 3. 找到可用规则子集后，跑通 router.v → egraph\_v2 → router\_opt.v → IMCCompiler 完整流程

&#x20; 4. 多benchmark交叉验证

