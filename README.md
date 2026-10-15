# Static Code Analyzer & CFG Optimizer

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=c%2B%2B)
![LLVM/Clang](https://img.shields.io/badge/LLVM%20%2F%20Clang-AST%20%26%20CFG-262D3A?style=flat-square&logo=llvm)
![CMake](https://img.shields.io/badge/CMake-Ninja-064F8C?style=flat-square&logo=cmake)
![Streamlit](https://img.shields.io/badge/Frontend-Streamlit-FF4B4B?style=flat-square&logo=streamlit)

A simple yet powerful Clang-based static analyzer for C code with Control Flow Graph (CFG) visualization, liveness analysis, constant folding/propagation, dead-code removal, and unreachable-function pruning. The analyzer is wrapped in a Streamlit UI for easy interaction.

---

## 🛠 Prerequisites

To build and run this project, you will need:

- **C++ Compiler**: Clang / Clang++ (MSYS2 UCRT64), GCC, or MSVC.
- **CMake & Ninja**: To configure and execute the build system.
- **LLVM / Clang Developer Libraries**: Required for the C++ backend to use the Clang AST and Rewriter APIs.
- **Python 3**: For running the web interface.

---

## 1. Building the C++ Analyzer (Backend)

The C++ backend (`CFGBuilder.cpp`) is compiled into an executable using CMake and Ninja.

Open your terminal in the root project directory and run the following commands:

**Step 1: Create the build directory and navigate into it**
```powershell
mkdir build
cd build
```

**Step 2: Configure the project using Ninja and Clang (pointing prefix path to your LLVM install)**
```powershell
cmake -G "Ninja" `
  -DCMAKE_C_COMPILER="clang" `
  -DCMAKE_CXX_COMPILER="clang++" `
  -DCMAKE_PREFIX_PATH="C:/msys64/ucrt64" `
  ..
```

**Step 3: Compile the analyzer binary**
```powershell
ninja
```

> After a successful build, the executable will be generated at:  
> `build/analyzer.exe` (on Windows) or `build/analyzer` (on Linux/macOS).

---

## 2. Running the Web Interface (Frontend)

The frontend is built using Python and Streamlit. It takes your C code, passes it to the `analyzer.exe`, and visualizes the generated DOT graph.

Open your terminal in the root project directory and run:

**Step 1: Install the required Python library**
```powershell
pip install streamlit
```

**Step 2: Run the web application**
```powershell
py -m streamlit run webinterface.py
```

This will start a local server and automatically open the application in your default web browser at **`http://localhost:8501`**.

---

## ⚙️ How It Works

1. **CFG Construction** — The backend builds a Clang CFG for each function.
2. **Dominator Analysis** — Identifies loop back-edges (blue edges in the graph).
3. **Constant Folding & Propagation** — Evaluates constant expressions block-locally and substitutes constants.
4. **Liveness Analysis** — Computes Live IN/OUT sets via a backward dataflow fixed-point iteration.
5. **Dead-Code Elimination** — Removes statements whose defined variables are not live and have no side effects.
6. **Unreachable Function Removal** — Prunes functions not reachable from `main`.

---

## 📝 Notes

> [!NOTE]
> - Constants are propagated **block-locally** to avoid changing program semantics across branches, loops, or function calls.
> - If the analyzer binary is not found, the UI will show an error. Make sure you ran the build step successfully.
> - DOT graphs are rendered inline using Streamlit's built-in Graphviz support.