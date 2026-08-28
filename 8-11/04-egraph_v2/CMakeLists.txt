cmake_minimum_required(VERSION 3.16)
project(egraph_v2 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

include_directories(
    "${CMAKE_SOURCE_DIR}"
    "D:/DESKBOOK/0809/mockturtle/include"
    "D:/DESKBOOK/0809/lorina/include"
    "D:/DESKBOOK/0809/z3-5.0.0-x64-win/z3-5.0.0-x64-win/include"
    "D:/gurobi1001/win64/include"
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

add_executable(egraph_opt
    main.cpp
    "D:/DESKBOOK/0809/mockturtle/lib/fmt/fmt/format.cc"
    "D:/DESKBOOK/0809/mockturtle/lib/fmt/fmt/os.cc"
)
if(MSVC)
    set_property(TARGET egraph_opt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
