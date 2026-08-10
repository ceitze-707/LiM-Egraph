**0810 代码归档**



**项目：E-graph优化存内计算逻辑综合与调度**





**环境依赖**



Visual Studio 2022 (MSVC + C++17)



CMake 3.16+



Gurobi 10.0.1 (D:\\\\gurobi1001\\\\)



Z3 5.0.0 (z3-5.0.0-x64-win\\\\)



Mockturtle + Lorina (header-only, mockturtle/include/ + lorina/include/)



Rust 1.97+ + egg 0.9





环境配置要点：



utils.h中修改Gurobi和Z3的路径；用CMake配置include/lib；



Mockturtle需额外添加lib/下的10个子库路径；需定义NOMINMAX防Windows宏冲突；



Gurobi库需/MD运行时（MultiThreadedDLL）；fmt库需编译format.cc和os.cc。



详见01-baseline/CMakeLists.txt。







**目录结构**



01-baseline/                  IMCCompiler原始代码（可编译运行）



02-egraph-attempts/     E-graph集成到IMCCompiler的尝试（v3/v5/v6）



03-tests/                        管道正确性验证测试程序



04-egraph\\\_v2/              新方案——全电路独立工具（当前工作方向）



CHANGELOG.txt             日志记录



High-Quality\_Iterative\_Compiler\_for\_SIMD\_Logic-in-Memory\_Architecture\_with\_Tight\_Coupling\_of\_Synthesis\_and\_Scheduling   参考论文



LiM0810.docx               周进度





**01-baseline — IMCCompiler原始代码**



内容：



IMCCompiler全部源文件（\\\_源文件备份）、CMakeLists.txt、baseline运行结果CSV、README（环境配置说明）



用途：编译运行IMCCompiler（迭代编译器），**得到baseline结果**



**结果：Size=184, MF=10 (router.v)**



依赖：Mockturtle, Lorina, Gurobi 10.0.1, Z3 5.0.0, MSVC





**02-egraph-attempts — E-graph集成尝试**



v3/      S-表达式路线。结果随机（PI名字重排导致索引错乱）





v5/      编号DAG路线



结果：Size=155, MF=9（改善16%，vs baseline 184/10）



&#x20;         30轮0崩溃，5条正确性自查全部通过



&#x20;局限：仅交换律生效，更强的规则未启用





v6/      去掉Verilog往返版



结果异常（Size=35，vs baseline 184）



根因：重建XMG的节点索引与ConfigWithXMG不兼容



→ 直接导致了思路转向全电路方案





版本说明.txt      各版本方法、结果、问题简述





**03-tests — pipeline验证测试**



test\\\_pipeline.cpp      全电路export→rebuild→simulate验证 (2000/2000 PASS)



test\\\_tiny.cpp             单门电路验证 (8/8 PASS)



test\\\_tiny2.cpp           单门互补fanin验证 (8/8 PASS)



test\\\_two\\\_gate.cpp     两门复用fanin验证 (8/8 PASS)



用途：独立于IMCCompiler，验证Export→Rebuild管道的功能正确性。



&#x20;这些测试发现了PI索引偏移bug和C0/C1 fidx覆盖bug。





**04-egraph\\\_v2 — 新方案独立工具**



思路转变：不再嵌入IMCCompiler的子网表，改为独立工具做全电路优化。



&#x20;**router.v → egraph\\\_v2 → router\\\_opt.v → IMCCompiler**



架构：C++主程序(main.cpp) + Rust e-graph(main.rs)



&#x20;C++负责：读Verilog、导出编号DAG、调Rust、重建XMG、仿真验证(2000 patterns)、写优化Verilog



&#x20;Rust负责：egg饱和、Per-PO提取



当前状态：透传验证通过(2000/2000 PASS)，Per-PO提取有PO索引bug待修



文件：main.cpp, main.rs, egraph\\\_helpers.h, sexpr\\\_parser.h,  CMakeLists.txt, Cargo.toml, router.v

