cmake_minimum_required(VERSION 3.10)
project(StaticAnalyzer)

# Find Clang/LLVM
find_package(Clang REQUIRED)
find_package(LLVM REQUIRED)

include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})

# The only executable
add_executable(analyzer CFGBuilder.cpp)
target_link_libraries(analyzer clangTooling clangBasic clangAST)