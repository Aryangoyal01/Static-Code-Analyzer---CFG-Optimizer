# Static Code Analyzer & CFG Optimizer

A simple yet powerful Clang-based static analyzer for C code with Control Flow Graph (CFG) visualization, liveness analysis, constant folding/propagation, dead-code removal, and unreachable-function pruning. The analyzer is wrapped in a Streamlit UI for easy interaction.

## Prerequisites

- **Windows**
  - CMake >= 3.10
  - LLVM/Clang toolchain (Visual Studio Build Tools + LLVM, or MSYS2 UCRT packages)
  - Python 3.8+
- **Linux/macOS**
  - CMake >= 3.10
  - LLVM/Clang development packages
  - Python 3.8+

## Quick Start (Windows)

Use the provided `run.bat` to build everything, install Python dependencies, and start the UI:

```bat
run.bat
```

Alternatively, build manually from an **x64 Native Tools Command Prompt** or **MSYS2 UCRT shell**:

```bat
mkdir build
cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
cd ..
python -m pip install -r requirements.txt
python -m streamlit run webinterface.py
```

## Quick Start (Linux/macOS)

```bash
# Build slider backend
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
cd ..

# Install Python deps and launch UI
python3 -m pip install -r requirements.txt
python3 -m streamlit run webinterface.py
```

## Using the UI

1. Open the Streamlit dashboard (usually at `http://localhost:8501`).
2. Paste C source code into the left panel.
3. Click **Analyze & Generate CFG**.
4. The right panel will show:
   - **Visual CFG** — interactive Graphviz chart with liveness sets and loop-back edges.
   - **Optimized Code** — the rewritten source after constant folding, propagation, dead-code removal, and unreachable-function pruning.
   - **Raw Console Output** — expand to view backend stdout/stderr.

## Project Files

| File | Purpose |
|------|---------|
| `CFGBuilder.cpp` | Clang Tooling backend. Performs CFG construction, liveness analysis, constant folding/propagation, dead-code elimination, and unreachable-function removal. |
| `webinterface.py` | Streamlit front-end. Runs the analyzer, renders DOT graphs, and shows optimized code. |
| `CMakeLists.txt` | Build configuration for the analyzer. |
| `run.bat` | One-click build + launch script for Windows. |
| `requirements.txt` | Python dependencies. |
| `build.bat` | Basic build script (kept from original project). |
| `README.md` | This documentation. |
| `.gitignore` | Git ignore rules. |

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

