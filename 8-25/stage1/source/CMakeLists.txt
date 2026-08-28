# [05-ADD] cut-based e-graph 工具的构建文件
# 依赖路径与 04-egraph_v2 一致：mockturtle/lorina 在 D:/DESKBOOK/0809/ 下
cmake_minimum_required(VERSION 3.16)
project(cut_egraph LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
# [05-ADD] 关键：源文件含中文注释(UTF-8)，必须让 MSVC 按 UTF-8 解析，
#   否则代码页 936(GBK) 会把中文字节错误解析导致编译错乱(报"int fout 重定义"等)。
if(MSVC)
    add_compile_options(/utf-8)
endif()

include_directories(
    "${CMAKE_SOURCE_DIR}"
    "D:/DESKBOOK/0809/mockturtle/include"
    "D:/DESKBOOK/0809/lorina/include"
    "D:/DESKBOOK/0809/mockturtle/lib/kitty"
    "D:/DESKBOOK/0809/mockturtle/lib/fmt"
    "D:/DESKBOOK/0809/mockturtle/lib/bill"
    "D:/DESKBOOK/0809/mockturtle/lib/percy"
    "D:/DESKBOOK/0809/mockturtle/lib/abcesop"
    "D:/DESKBOOK/0809/mockturtle/lib/abcsat"
    "D:/DESKBOOK/0809/mockturtle/lib/json"
    "D:/DESKBOOK/0809/mockturtle/lib/parallel_hashmap"
    "D:/DESKBOOK/0809/mockturtle/lib/rang"
    "D:/DESKBOOK/0809/mockturtle/lib/lorina"
    "D:/DESKBOOK/0809/mockturtle/lib/nauty"
)

add_executable(cut_egraph
    main.cpp
    "D:/DESKBOOK/0809/mockturtle/lib/fmt/fmt/format.cc"
    "D:/DESKBOOK/0809/mockturtle/lib/fmt/fmt/os.cc"
)
if(MSVC)
    set_property(TARGET cut_egraph PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
