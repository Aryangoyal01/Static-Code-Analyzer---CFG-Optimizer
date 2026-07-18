# Static Code Analyzer & CFG Optimizer

A simple yet powerful Clang-based static analyzer for C code with Control Flow Graph (CFG) visualization, liveness analysis, constant folding/propagation, dead-code removal, and unreachable-function pruning. The analyzer is wrapped in a Streamlit UI for easy interaction.

## Prerequisites

To build and run this project, you will need:
- **C++ Compiler**: A compiler like MSVC (Visual Studio) or GCC.
- **CMake**: To configure the build system.
- **LLVM / Clang Developer Libraries**: Required for the C++ backend to use the Clang AST and Rewriter APIs.
- **Python 3**: For running the web interface.

## 1. Building the C++ Analyzer (Backend)

The C++ backend (`CFGBuilder.cpp`) must be compiled into an executable using CMake. Because LLVM/Clang on Windows are typically distributed in "Release" mode, you must build the project in **Release** mode to avoid standard library mismatch errors (`LNK2038`).

Open your terminal in the root project directory and run the following commands:

```powershell
# Create the build directory and navigate into it
mkdir build
cd build

# Configure the project using CMake
cmake ..

# Compile the backend in Release mode
cmake --build . --config Release
```

After a successful build, the executable will be generated at:
`build/Release/analyzer.exe` (on Windows) or `build/analyzer` (on Linux/macOS).

## 2. Running the Web Interface (Frontend)

The frontend is built using Python and Streamlit. It takes your C code, passes it to the `analyzer.exe`, and visualizes the generated DOT graph.

Open your terminal in the root project directory and run:

```powershell
# Install the required Python library
pip install streamlit

# Run the web application
py -m streamlit run webinterface.py
```

This will start a local server and automatically open the application in your default web browser at `http://localhost:8501`.

## How It Works

1. **CFG Construction** — The backend builds a Clang CFG for each function.
2. **Dominator Analysis** — Identifies loop back-edges (blue edges in the graph).
3. **Constant Folding & Propagation** — Evaluates constant expressions block-locally and substitutes constants.
4. **Liveness Analysis** — Computes Live IN/OUT sets via a backward dataflow fixed-point iteration.
5. **Dead-Code Elimination** — Removes statements whose defined variables are not live and have no side effects.
6. **Unreachable Function Removal** — Prunes functions not reachable from `main`.

## Notes

- Constants are propagated **block-locally** to avoid changing program semantics across branches, loops, or function calls.
- If the analyzer binary is not found, the UI will show an error. Make sure you ran the build step successfully.
- DOT graphs are rendered inline using Streamlit's built-in Graphviz support.

