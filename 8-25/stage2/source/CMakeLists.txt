cmake_minimum_required(VERSION 3.16)
project(IMCCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# [06-ADD] 防止 Windows SDK 的 min/max 宏和 nauty/RPC 名称污染 mockturtle。
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# === Include directories ===
include_directories(
    "${CMAKE_SOURCE_DIR}"
    "D:/DESKBOOK/0809/mockturtle/include"
    "D:/DESKBOOK/0809/lorina/include"
    "D:/DESKBOOK/0809/z3-5.0.0-x64-win/z3-5.0.0-x64-win/include"
    "D:/gurobi1001/win64/include"
    # [06-ADD] mockturtle 是 header-only 主库，但其依赖也必须显式加入。
    "D:/DESKBOOK/0809/mockturtle/lib/kitty"
    "D:/DESKBOOK/0809/mockturtle/lib/fmt"
    "D:/DESKBOOK/0809/mockturtle/lib/bill"
    # dont_cares（被原有 resub/rewrite 使用）同样经由适配头依赖 percy。
    "D:/DESKBOOK/0809/mockturtle/lib/percy"
    "D:/DESKBOOK/0809/mockturtle/lib/abcesop"
    "D:/DESKBOOK/0809/mockturtle/lib/abcsat"
    "D:/DESKBOOK/0809/mockturtle/lib/json"
    "D:/DESKBOOK/0809/mockturtle/lib/parallel_hashmap"
    "D:/DESKBOOK/0809/mockturtle/lib/rang"
    "D:/DESKBOOK/0809/mockturtle/lib/lorina"
    "D:/DESKBOOK/0809/mockturtle/lib/nauty"
)

# === Library directories ===
link_directories(
    "D:/DESKBOOK/0809/z3-5.0.0-x64-win/z3-5.0.0-x64-win/bin"
    "D:/gurobi1001/win64/lib"
)

# === Source files ===
add_executable(IMCCompiler
    IMCCompiler.cpp
    Scheduler.cpp
    Scheduler.h
    Netlist.cpp
    Netlist.h
    utils.h
    # [06-ADD] fmt 非 header-only 部分，供 mockturtle 使用。
    "D:/DESKBOOK/0809/mockturtle/lib/fmt/fmt/format.cc"
    "D:/DESKBOOK/0809/mockturtle/lib/fmt/fmt/os.cc"
)

# === Link libraries ===
target_link_libraries(IMCCompiler
    "D:/DESKBOOK/0809/z3-5.0.0-x64-win/z3-5.0.0-x64-win/bin/libz3.lib"
    "D:/gurobi1001/win64/lib/gurobi100.lib"
    "D:/gurobi1001/win64/lib/gurobi_c++md2017.lib"
)

# === Windows: use MSVC multi-threaded runtime ===
if(MSVC)
    # [06-MOD] 本机 Gurobi C++ 库只提供 /MD ABI；Debug 目标也须采用 /MD 才能链接。
    # 这只影响诊断构建的运行库选择，不改变 Release 的既有 /MD 行为。
    set_property(TARGET IMCCompiler PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    # [06-ADD] mockturtle 的模板实例化在 Debug 下超过 MSVC 默认 COFF 节数量；
    # /bigobj 只扩展对象文件格式上限，不改变程序语义或 Release 优化策略。
    target_compile_options(IMCCompiler PRIVATE /utf-8 /bigobj)
    # [06-ADD] Gurobi 的 C++ 包以 release iterator ABI 发布；否则 Debug 链接会报
    # _ITERATOR_DEBUG_LEVEL 不匹配。该设置仅用于生成可运行的诊断二进制。
    target_compile_definitions(IMCCompiler PRIVATE $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>)
endif()
