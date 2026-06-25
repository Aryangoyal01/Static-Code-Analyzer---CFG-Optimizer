cmake_minimum_required(VERSION 3.10)
project(StaticAnalyzer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Clang/LLVM
find_package(Clang REQUIRED)
find_package(LLVM REQUIRED)

include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})

# Place the built executable directly inside the build directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# The only executable
add_executable(analyzer CFGBuilder.cpp)
target_link_libraries(analyzer clangTooling clangBasic clangAST)
