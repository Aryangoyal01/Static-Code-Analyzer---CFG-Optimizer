cmake_minimum_required(VERSION 3.10)
project(StaticAnalyzer)

# Find LLVM and Clang packages
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

# Add LLVM/Clang include directories
include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable large code model to prevent 32-bit PC-relative offset overflow in big Clang libraries
add_compile_options(-mcmodel=medium)

# Create the executable
add_executable(analyzer CFGBuilder.cpp)

# Link all required Clang and LLVM libraries
target_link_libraries(analyzer PRIVATE
    clangTooling
    clangFrontend
    clangSerialization
    clangDriver
    clangParse
    clangSema
    clangAnalysis
    clangEdit
    clangAST
    clangLex
    clangBasic
    clangRewrite
)

# Use lld linker (bundled with your msys2 clang) and handle multiple definitions
target_link_options(analyzer PRIVATE 
    "-fuse-ld=lld"
    "-Wl,--allow-multiple-definition"
)