# Static Code Analyzer & CFG Optimizer

A Clang-based static analyzer for C code with CFG visualization, liveness analysis, constant folding/propagation, dead-code removal, and unreachable-function pruning. The Streamlit interface runs the analyzer on pasted C code, renders DOT CFG output, and displays rewritten optimized code.

## Files

- `CFGBuilder.cpp` - Clang Tooling backend and optimizer.
- `webinterface.py` - Streamlit dashboard.
- `CMakeLists.txt` - backend build configuration.
- `CMakePresets.json` - Windows preset for a local LLVM install.
- `CS-202 Project Group-16 Report.pdf` - project report.

## Build

The existing `build/` directory may contain stale machine-specific CMake cache files. Prefer a fresh build directory.

On Windows, install one compatible toolchain path:

- Visual Studio Build Tools with the Windows SDK, plus LLVM/Clang installed at `C:\LLVM`; or
- MSYS2 UCRT LLVM/Clang packages, with matching LLVM/Clang CMake package files.

With Visual Studio Build Tools/Windows SDK available in your shell:

```bat
cmake --preset windows-msvc-llvm
cmake --build --preset windows-msvc-llvm
```

With MSYS2 UCRT LLVM packages installed:

```bat
cmake --preset windows-mingw-llvm
cmake --build --preset windows-mingw-llvm
```

If your LLVM paths differ, update `CMakePresets.json` or pass `LLVM_DIR` and `Clang_DIR` manually.

## Run

```bat
python -m streamlit run webinterface.py
```

The dashboard searches for `build/analyzer.exe`, `analyzer.exe`, `build/analyzer`, then `analyzer`. On Windows it rejects Linux/WSL ELF binaries and asks for a native `analyzer.exe`.

