cmake_minimum_required(VERSION 3.16)
project(IMCCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# === Include directories ===
include_directories(
    "${CMAKE_SOURCE_DIR}"
    "D:/DESKBOOK/0809/mockturtle/include"
    "D:/DESKBOOK/0809/lorina/include"
    "D:/DESKBOOK/0809/z3-5.0.0-x64-win/z3-5.0.0-x64-win/include"
    "D:/gurobi1001/win64/include"
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
)

# === Link libraries ===
target_link_libraries(IMCCompiler
    "D:/DESKBOOK/0809/z3-5.0.0-x64-win/z3-5.0.0-x64-win/bin/libz3.lib"
    "D:/gurobi1001/win64/lib/gurobi100.lib"
    "D:/gurobi1001/win64/lib/gurobi_c++md2017.lib"
)

# === Windows: use MSVC multi-threaded runtime ===
if(MSVC)
    set_property(TARGET IMCCompiler PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
